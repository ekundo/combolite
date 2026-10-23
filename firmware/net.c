/* Сервер NBD поверх ata.c.
 *
 * Сокеты BSD, а не что-то своё: у ESP-IDF они есть через lwIP.
 */

#include "net.h"
#include "ata.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define NBD_MAGIC        0x4e42444d41474943ull   /* "NBDMAGIC" */
#define NBD_IHAVEOPT     0x49484156454F5054ull   /* "IHAVEOPT" */
#define NBD_OPT_REPLY    0x0003e889045565a9ull
#define NBD_REQUEST      0x25609513u
#define NBD_SIMPLE_REPLY 0x67446698u

#define F_FIXED_NEWSTYLE 1
#define F_NO_ZEROES      2
#define F_HAS_FLAGS      1
#define F_READ_ONLY      2
#define F_SEND_FLUSH     4

#define OPT_EXPORT_NAME  1
#define OPT_ABORT        2
#define OPT_GO           7

#define REP_ACK          1u
#define REP_INFO         3u
#define REP_ERR_UNSUP    0x80000001u
#define REP_ERR_UNKNOWN  0x80000006u

#define INFO_EXPORT      0

#define CMD_READ   0
#define CMD_WRITE  1
#define CMD_DISC   2
#define CMD_FLUSH  3

#define E_PERM   1
#define E_IO     5
#define E_INVAL 22
#define E_NOSPC 28

/* Повторов при отъёме шины. Сектор занимает шину меньше миллисекунды, а предел
 * владения — десять, так что отъём посреди сектора означает, что ESP надолго
 * отвлеклась. Пары повторов достаточно; бесконечные только скрыли бы беду. */
#define PREEMPT_TRIES 3

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)get16(p) << 16) | get16(p + 2); }
static uint64_t get64(const uint8_t *p) { return ((uint64_t)get32(p) << 32) | get32(p + 4); }

static int rd_all(int s, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t k = recv(s, p, n, 0);
        if (k == 0) return -1;
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

static int wr_all(int s, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t k = send(s, p, n, 0);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= (size_t)k;
    }
    return 0;
}

/* ---------------------------------------------------------------- диск

   Шину НЕ держим на весь запрос, хотя NBD может попросить и 128 Кбайт.
   Причина в арбитре: предел владения 10 мс (fpga/hdl/ide_arb.v), а такой запрос —
   это сотня миллисекунд, то есть гарантированный отъём посреди дела.

   Отпускать после каждого сектора дёшево: счётчик тишины в арбитре бежит сам и
   насыщается, обнуляет его только обращение процессора к портам привода
   (fpga/hdl/ide_arb.v:79). Пока «Вектор» диском не пользуется, повторный захват
   стоит два такта. А когда пользуется — мы уступаем ему после каждого сектора,
   а не запираем диск на всю длину запроса. */

static int one_sector(int write, uint32_t lba, uint8_t *buf)
{
    int rc = ATA_OK, t;
    for (t = 0; t < PREEMPT_TRIES; t++) {
        rc = write ? ata_write(lba, buf) : ata_read(lba, buf);
        if (rc != ATA_E_PREEMPT) return rc;
    }
    return rc;
}

/* Чтение пачкой с повтором по отъёму. Повторяем ИМЕННО кусок, а не весь заказ:
 * иначе отъём на последнем куске заставил бы перечитывать всё сначала. */
static int read_span(uint32_t lba, uint32_t count, uint8_t *buf, uint32_t *at)
{
    uint32_t first = lba;
    while (count) {
        uint32_t k = count > ATA_RUN ? ATA_RUN : count, done = 0;
        int rc = ATA_OK, t;
        for (t = 0; t < PREEMPT_TRIES; t++) {
            rc = ata_read_run(lba, k, buf, &done);
            if (rc != ATA_E_PREEMPT) break;
        }
        if (rc != ATA_OK) {
            if (at) *at = lba + done;       /* именно этот сектор и не дался */
            return rc;
        }
        lba += k; count -= k; buf += (size_t)k * ATA_SECTOR;
    }
    if (at) *at = first;
    return ATA_OK;
}

static uint32_t nbd_errno(int rc)
{
    switch (rc) {
    case ATA_OK:        return 0;
    case ATA_E_RANGE:   return E_NOSPC;
    case ATA_E_DRIVE:   return E_IO;
    case ATA_E_PREEMPT: return E_IO;
    default:            return E_IO;
    }
}

/* ------------------------------------------------------------- рукопожатие */

static int opt_reply(int s, uint32_t opt, uint32_t type, const uint8_t *data, uint32_t len)
{
    uint8_t h[20];
    put64(h, NBD_OPT_REPLY);
    put32(h + 8, opt);
    put32(h + 12, type);
    put32(h + 16, len);
    if (wr_all(s, h, sizeof h) < 0) return -1;
    return len ? wr_all(s, data, len) : 0;
}

/* Сравнение за постоянное время — см. ту же оговорку в http.c. */
static int same_secret(const char *a, size_t la, const char *b)
{
    size_t lb = strlen(b), i;
    unsigned diff = (unsigned)(la ^ lb);

    for (i = 0; i < la && i < lb; i++)
        diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

/* 0 — можно переходить к передаче, 1 — клиент отсоединился, -1 — сбой */
static int handshake(int s, uint64_t size, uint16_t tflags, const char *token)
{
    uint8_t hdr[18], o[16], info[12], exp[10];
    uint32_t cflags;

    put64(hdr, NBD_MAGIC);
    put64(hdr + 8, NBD_IHAVEOPT);
    put16(hdr + 16, F_FIXED_NEWSTYLE | F_NO_ZEROES);
    if (wr_all(s, hdr, sizeof hdr) < 0) return -1;
    if (rd_all(s, o, 4) < 0) return -1;
    cflags = get32(o);

    for (;;) {
        uint32_t opt, len;
        if (rd_all(s, o, 16) < 0) return -1;
        if (get64(o) != NBD_IHAVEOPT) return -1;
        opt = get32(o + 8);
        len = get32(o + 12);

        /* Стек задачи ESP-IDF невелик, а имена экспортов короткие: длинное
         * согласование нам просто не от кого получить. */
        if (len > 512) return -1;
        {
            uint8_t data[513];
            const char *name = "";
            size_t nlen = 0;

            if (len && rd_all(s, data, len) < 0) return -1;
            data[len] = 0;

            /* Имя экспорта — наш секрет. У OPT_EXPORT_NAME это все данные
             * целиком, у OPT_GO — длина и имя в начале. */
            if (opt == OPT_EXPORT_NAME) { name = (const char *)data; nlen = len; }
            else if (opt == OPT_GO && len >= 4) {
                uint32_t k = get32(data);
                if (k <= len - 4) { name = (const char *)data + 4; nlen = k; }
            }

            if ((opt == OPT_EXPORT_NAME || opt == OPT_GO) &&
                token && *token && !same_secret(name, nlen, token)) {
                /* У OPT_EXPORT_NAME отказать по протоколу нечем — только
                 * закрыть соединение; у OPT_GO есть внятный код. */
                if (opt == OPT_GO) opt_reply(s, opt, REP_ERR_UNKNOWN, 0, 0);
                return -1;
            }
        }

        switch (opt) {
        case OPT_EXPORT_NAME:
            put64(exp, size);
            put16(exp + 8, tflags);
            if (wr_all(s, exp, sizeof exp) < 0) return -1;
            if (!(cflags & F_NO_ZEROES)) {
                uint8_t pad[124];
                memset(pad, 0, sizeof pad);
                if (wr_all(s, pad, sizeof pad) < 0) return -1;
            }
            return 0;

        case OPT_GO:
            put16(info, INFO_EXPORT);
            put64(info + 2, size);
            put16(info + 10, tflags);
            if (opt_reply(s, opt, REP_INFO, info, sizeof info) < 0) return -1;
            if (opt_reply(s, opt, REP_ACK, 0, 0) < 0) return -1;
            return 0;

        case OPT_ABORT:
            opt_reply(s, opt, REP_ACK, 0, 0);
            return 1;

        default:
            if (opt_reply(s, opt, REP_ERR_UNSUP, 0, 0) < 0) return -1;
            break;
        }
    }
}

/* --------------------------------------------------------------- передача */

static int reply(int s, const uint8_t *handle, uint32_t err)
{
    uint8_t r[16];
    put32(r, NBD_SIMPLE_REPLY);
    put32(r + 4, err);
    memcpy(r + 8, handle, 8);
    return wr_all(s, r, sizeof r);
}

/* ОДИН запрос. Возвращает 0, если можно ждать следующий, и отрицательное,
 * если соединение пора закрывать.
 *
 * Разбито именно так не из красоты: с бесконечным циклом здесь, пока диск
 * смонтирован по NBD, вебморда не отвечала бы вовсе — общий цикл опроса не
 * получал бы управления до самого размонтирования. После рукопожатия у NBD нет
 * состояния, так что запросы прекрасно обслуживаются поодиночке. */
int nbd_request(int s, const net_cfg_t *cfg)
{
    uint64_t size = (uint64_t)cfg->sectors * ATA_SECTOR;
    int read_only = cfg->read_only;
    uint8_t sec[ATA_SECTOR];

    {
        uint8_t req[28];
        uint32_t type, len;
        uint64_t off;
        uint32_t lba, n, i;

        if (rd_all(s, req, sizeof req) < 0) return -1;     /* клиент ушёл */
        if (get32(req) != NBD_REQUEST) return -1;
        type = get16(req + 6);
        off  = get64(req + 16);
        len  = get32(req + 24);

        if (type == CMD_DISC) return -1;

        if (type == CMD_FLUSH) {
            int rc = ata_flush();
            if (reply(s, req + 8, nbd_errno(rc)) < 0) return -1;
            return 0;
        }

        /* Работаем только целыми секторами. Клиенты это соблюдают: размер
         * блока объявлен, а ядро и так ходит по 512 байт. */
        if ((off % ATA_SECTOR) || (len % ATA_SECTOR) || off + len > size) {
            if (reply(s, req + 8, E_INVAL) < 0) return -1;
            if (type == CMD_WRITE) {                  /* данные всё равно придут */
                uint32_t left = len;
                while (left >= ATA_SECTOR) { if (rd_all(s, sec, ATA_SECTOR) < 0) return -1; left -= ATA_SECTOR; }
            }
            return 0;
        }

        lba = (uint32_t)(off / ATA_SECTOR);
        n   = len / ATA_SECTOR;

        if (type == CMD_WRITE) {
            uint32_t err = 0;
            int held = 0;
            if (read_only) err = E_PERM;
            else if (ata_lock_long() == ATA_OK) held = 1;
            else err = E_IO;                /* шину не дали */
            for (i = 0; i < n; i++) {
                if (rd_all(s, sec, ATA_SECTOR) < 0) return -1;
                if (!err) {
                    int rc = one_sector(1, lba + i, sec);
                    if (rc != ATA_OK) err = nbd_errno(rc);
                }
            }
            if (held) ata_unlock();
            if (reply(s, req + 8, err) < 0) return -1;
            return 0;
        }

        if (type == CMD_READ) {
            /* Заголовок ответа уходит до данных, поэтому об ошибке на середине
             * доложить уже нечем — рвём соединение. Клиент это увидит и не
             * примет обрывок за содержимое диска. */
            /* Читаем пачками, а весь заказ в память не берём: у NBD он
             * бывает и в сотню килобайт, а куча на ESP32 не резиновая. */
            static uint8_t run[ATA_RUN * ATA_SECTOR];
            uint32_t done = 0;
            int sent = 0;
            while (done < n) {
                uint32_t k = n - done > ATA_RUN ? ATA_RUN : n - done;
                int rc = read_span(lba + done, k, run, 0);
                if (rc != ATA_OK) {
                    if (sent) return -1;    /* заголовок ушёл, доложить нечем */
                    if (reply(s, req + 8, nbd_errno(rc)) < 0) return -1;
                    return 0;
                }
                if (!sent) {
                    if (reply(s, req + 8, 0) < 0) return -1;
                    sent = 1;
                }
                if (wr_all(s, run, (size_t)k * ATA_SECTOR) < 0) return -1;
                done += k;
            }
            return 0;
        }

        if (reply(s, req + 8, E_INVAL) < 0) return -1;
        return 0;
    }
}

int net_resolve(net_cfg_t *cfg, ata_dev_t *out)
{
    ata_dev_t dev;
    int rc;

    if (cfg->sectors) return 0;
    rc = ata_init(&dev);
    if (rc != ATA_OK) {
        fprintf(stderr, "drive identify failed: %s\n", ata_strerror(rc));
        /* Отдаём именно код, а не -1: вызывающему важно отличить молчащую ПЛИС
         * (ATA_E_LINK) от молчащего привода. На плате это разные починки, а
         * сообщение выше уходит в stderr и в её журнал не попадает. */
        return rc;
    }
    if (out) *out = dev;
    cfg->sectors = dev.sectors;
    printf("drive: %s, serial %s, %u sectors\n", dev.model, dev.serial,
           (unsigned)cfg->sectors);
    return 0;
}

int nbd_listen(uint16_t port)
{
    struct sockaddr_in a;
    int ls, one = 1;

    ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0) { close(ls); return -1; }
    if (listen(ls, 4) < 0) { close(ls); return -1; }
    return ls;
}

/* Рукопожатие. 0 — можно принимать запросы, иначе соединение закрыть. */
int nbd_greet(int fd, const net_cfg_t *cfg)
{
    uint64_t size = (uint64_t)cfg->sectors * ATA_SECTOR;
    uint16_t tflags = F_HAS_FLAGS | F_SEND_FLUSH | (cfg->read_only ? F_READ_ONLY : 0);
    int one = 1;

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return handshake(fd, size, tflags, cfg->token) == 0 ? 0 : -1;
}

void nbd_serve_conn(int fd, const net_cfg_t *cfg)
{
    if (nbd_greet(fd, cfg) < 0) return;
    while (nbd_request(fd, cfg) == 0)
        ;
}

int net_serve(const net_cfg_t *cfg)
{
    net_cfg_t c = *cfg;
    int ls;

    if (net_resolve(&c, 0) < 0) return -1;
    ls = nbd_listen(c.port);
    if (ls < 0) return -1;

    printf("NBD listening on port %u, export %llu bytes%s\n",
           (unsigned)c.port, (unsigned long long)c.sectors * ATA_SECTOR,
           c.read_only ? " (read-only)" : "");
    fflush(stdout);

    for (;;) {
        int cs = accept(ls, 0, 0);
        if (cs < 0) { if (errno == EINTR) continue; break; }
        nbd_serve_conn(cs, &c);
        close(cs);
        if (c.once) { close(ls); return 0; }
    }
    close(ls);
    return -1;
}
