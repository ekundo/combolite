/* Сервер HTTP: API секторов и статика. Оговорки — в http.h. */

#include "http.h"

#include "ota.h"
#include "roam.h"
#include "ata.h"
#include "board.h"
#include "fpga.h"
#include "auth.h"
#include "jtag.h"
#include "xvc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <strings.h>       /* strncasecmp */
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define HDR_MAX   2048
#define PATH_MAX_ 256
#define CHUNK     512

/* Кусок отдачи файла — НЕ 512.
 *
 * Морда это 211 КБ, и по 512 байт она уезжала 12 секунд: 15 КБ/с, то есть
 * около тридцати посылок в секунду. Столько же, сколько даёт задержанное
 * подтверждение TCP: приёмник ждёт второй сегмент, отправитель ждёт ACK, и на
 * каждый кусок уходит около 33 мс. Сеть тут ни при чём -- заголовок приходит за
 * 0,1 с, а мелкий файл отдаётся мгновенно.
 *
 * 4 КБ это почти три сегмента по 1440, так что ждать нечего и некому. Буфер
 * СТАТИЧЕСКИЙ: у главной задачи на плате всего 8 КБ стека, и половину съедать
 * под него нельзя. Сервер однопоточный -- srv_run обходит сокеты по очереди в
 * одном цикле, -- так что делить буфер не с кем. */
#define FILE_CHUNK 4096

/* Повторов при отъёме шины — как в net.c и по той же причине. */
#define PREEMPT_TRIES 3

/* ------------------------------------------------------------- мелочи сокета */

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

/* keep — оставляем ли соединение открытым. Заявленное в заголовке должно
 * совпадать с тем, что мы сделаем на самом деле, иначе клиент будет ждать
 * следующий ответ в сокете, который мы закрыли. */
static int say(int s, int keep, int code, const char *reason, const char *type,
               long len, const char *extra)
{
    char h[256];
    int n = snprintf(h, sizeof h,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %ld\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "\r\n",
                     code, reason, type, len, keep ? "keep-alive" : "close",
                     extra ? extra : "");
    return wr_all(s, h, (size_t)n);
}

static void fail_hdr(int s, int keep, int code, const char *reason,
                     const char *msg, const char *extra)
{
    char body[192];
    int n = snprintf(body, sizeof body, "{\"error\":\"%s\"}\n", msg);
    if (say(s, keep, code, reason, "application/json", n, extra) == 0)
        wr_all(s, body, (size_t)n);
}

#define fail(s, keep, code, reason, msg) fail_hdr(s, keep, code, reason, msg, 0)

/* --------------------------------------------------------------- пропуск */

/* Значение заголовка по имени, без учёта регистра. Возвращает out или ноль. */
static const char *hdr_val(const char *hdr, const char *end, const char *name,
                           char *out, size_t max)
{
    size_t nlen = strlen(name);
    const char *p;

    for (p = hdr; p + nlen < end; p++) {
        const char *v;
        size_t i = 0;
        if (p != hdr && p[-1] != '\n') continue;      /* только начало строки */
        if (strncasecmp(p, name, nlen) || p[nlen] != ':') continue;
        v = p + nlen + 1;
        while (*v == ' ' || *v == '\t') v++;
        while (v[i] && v[i] != '\r' && v[i] != '\n' && i < max - 1) { out[i] = v[i]; i++; }
        out[i] = '\0';
        return out;
    }
    return 0;
}

/* Разрешение записи: до какого момента оно действует.
 *
 * Статик, а не поле настройки: сервер в процессе один и цикл у него один, а
 * настройка приходит как const. Заводить ради этого ещё один изменяемый
 * объект и протаскивать его сквозь все обработчики — дороже пользы. */
static uint32_t unlock_until;

/* Часы те же, что у цикла обслуживания: монотонные, без привязки к дате. */
static uint32_t now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

/* 1 — писать можно. Сравнение вычитанием: часы заворачиваются на 49 сутках. */
static int writable(const http_cfg_t *cfg)
{
    if (cfg->read_only) return 0;
    if (!cfg->write_window_s) return 1;          /* окна не требуется */
    return (int32_t)(now_s() - unlock_until) < 0;
}

/* Дочитать и выбросить остаток тела.
 *
 * ЗАЧЕМ. Закрыть сокет, в приёмном буфере которого лежат непрочитанные данные,
 * значит послать RST вместо FIN — а RST затирает уже отправленный ответ, и
 * клиент вместо внятного кода ошибки видит обрыв связи. Поэтому при любом
 * отказе на запрос с телом тело надо сперва дочитать.
 *
 * Ограничение сверху есть: дочитывать мегабайты ради вежливости незачем, такое
 * соединение проще закрыть. Возвращает 0, если можно продолжать. */
#define DRAIN_MAX 1048576L

static int drain(int s, long n, const uint8_t *pre, size_t pre_len)
{
    uint8_t junk[CHUNK];

    (void)pre;                 /* сам кусок не нужен — важно лишь, сколько его */
    if (n < 0 || n > DRAIN_MAX) return -1;
    if ((long)pre_len >= n) return 0;
    n -= (long)pre_len;
    while (n > 0) {
        size_t k = (size_t)(n < (long)sizeof junk ? n : (long)sizeof junk);
        if (rd_all(s, junk, k) < 0) return -1;
        n -= (long)k;
    }
    return 0;
}

/* ------------------------------------------------------------------- диск */

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

/* --------------------------------------------------------------- статика */

static const char *mime(const char *path)
{
    const char *d = strrchr(path, '.');
    if (!d) return "application/octet-stream";
    if (!strcmp(d, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(d, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(d, ".js"))   return "application/javascript; charset=utf-8";
    /* Модуль с чужим типом браузер выполнять откажется, а морда без него
     * не поднимется вовсе. */
    if (!strcmp(d, ".mjs"))  return "application/javascript; charset=utf-8";
    if (!strcmp(d, ".json")) return "application/json";
    if (!strcmp(d, ".svg"))  return "image/svg+xml";
    if (!strcmp(d, ".png"))  return "image/png";
    if (!strcmp(d, ".ico"))  return "image/x-icon";
    if (!strcmp(d, ".woff2"))return "font/woff2";
    if (!strcmp(d, ".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/* Путь приходит снаружи, поэтому проверяется, а не чинится. Любая точка-точка,
 * обратная косая или управляющий знак — отказ целиком: чинить такие пути
 * значит соревноваться с тем, кто их придумывает. */
static int path_ok(const char *p)
{
    size_t i;
    if (p[0] != '/') return 0;
    for (i = 0; p[i]; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == 0x7F) return 0;
        if (c == '\\') return 0;
        if (c == '.' && p[i + 1] == '.') return 0;
    }
    return i < PATH_MAX_ - 32;
}

static void serve_file(int s, int keep, const http_cfg_t *cfg, const char *path)
{
    char full[PATH_MAX_ + 64];
    static uint8_t buf[FILE_CHUNK];
    FILE *f;
    long len;

    if (!path_ok(path)) { fail(s, keep, 400, "Bad Request", "bad path"); return; }
    snprintf(full, sizeof full, "%s%s", cfg->root,
             strcmp(path, "/") == 0 ? "/index.html" : path);

    f = fopen(full, "rb");
    if (!f) { fail(s, keep, 404, "Not Found", "no such file"); return; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (say(s, keep, 200, "OK", mime(full), len, 0) == 0) {
        size_t k;
        while ((k = fread(buf, 1, sizeof buf, f)) > 0)
            if (wr_all(s, buf, k) < 0) break;
    }
    fclose(f);
}

/* ------------------------------------------------------------------- API */

/* Опознание делается ОДИН РАЗ при старте, а сюда приходит готовое: полный
 * цикл IDENTIFY с захватом шины на каждую загрузку страницы шёл бы поперёк
 * работы «Вектора». */
static const char *drive_word(void)
{
    switch (ata_drive_state()) {
    case ATA_DRIVE_RESET: return "resetting";
    case ATA_DRIVE_READY: return "ready";
    case ATA_DRIVE_DEAD:  return "dead";
    default:              return "unknown";
    }
}

/* Ответ, который целиком пишет сама плата: обзор эфира, журнал. Текстом, как
 * есть: это ответ ЧЕЛОВЕКУ, и раскладывать его по полям, чтобы морда собрала
 * обратно, незачем. Килобайта хватает обоим -- журнал отдаёт хвост. */
static void api_text(int s, int keep, int (*make)(char *, size_t),
                     const char *absent)
{
    static char text[4096];
    int n = make ? make(text, sizeof text) : -1;

    if (n < 0)
        fail(s, keep, 501, "Not Implemented", absent);
    else if (say(s, keep, 200, "OK", "text/plain", n, 0) == 0)
        wr_all(s, text, (size_t)n);
}

/* Приём обновления: прошивки или статики морды.
 *
 * Отличается от всего прочего тем, что тело НЕ ПОМЕЩАЕТСЯ в память: около
 * мегабайта при сотнях килобайт свободных. Поэтому оно течёт кусками прямо в
 * раздел, а хеш считается по дороге — и подпись проверяется В КОНЦЕ, когда
 * хеш готов.
 *
 * Порядок важен: пока подпись не сошлась, ota_finish(0) забывает написанное,
 * и загрузочным остаётся прежний раздел. То есть чужой запрос может испортить
 * запасной раздел, но не может подсунуть плате свою прошивку.
 *
 * Со статикой хуже: раздел под неё один, и неудачная запись портит морду до
 * следующей попытки. Прошивка при этом цела, попытку принять есть чему —
 * потому такой обмен и допустим.
 */
static int api_ota(int s, int keep, const http_cfg_t *cfg, const char *path,
                   const char *signed_path, const char *method,
                   const char *hdr, const char *body, size_t body_have,
                   long body_len)
{
    static uint8_t chunk[2048];
    int what = strcmp(path, "/api/ota/www") ? OTA_APP : OTA_WWW;
    sha256_t sum;
    uint8_t hash[SHA256_SIZE];
    long left = body_len;
    int ok = 1;

    if (body_len <= 0) {
        fail(s, keep, 400, "Bad Request", "empty body");
        return keep ? 0 : -1;
    }
    if (ota_begin(what, (size_t)body_len) < 0) {
        if (drain(s, body_len, (const uint8_t *)body, body_have) < 0) keep = 0;
        fail(s, keep, 501, "Not Implemented", ota_error());
        return keep ? 0 : -1;
    }

    combo_sha256_init(&sum);

    /* Хвост заголовка — это уже начало тела: читая заголовок, мы прихватили
       сколько пришло, и терять его нельзя. */
    if (body_have) {
        size_t take = body_have < (size_t)left ? body_have : (size_t)left;
        combo_sha256_update(&sum, body, take);
        if (ota_write((const uint8_t *)body, take) < 0) ok = 0;
        left -= (long)take;
    }
    while (left > 0) {
        size_t want = (size_t)left < sizeof chunk ? (size_t)left : sizeof chunk;
        if (rd_all(s, chunk, want) < 0) { ok = 0; keep = 0; break; }
        combo_sha256_update(&sum, chunk, want);
        /* Читать продолжаем даже после сбоя записи: недочитанное тело оставило
           бы в сокете мусор, который следующий запрос принял бы за свой. */
        if (ok && ota_write(chunk, want) < 0) ok = 0;
        left -= (long)want;
    }
    combo_sha256_final(&sum, hash);

    /* Вот теперь подпись — по хешу, посчитанному на лету. */
    if (ok && cfg->auth && auth_on(cfg->auth)) {
        char n[AUTH_NONCE_HEX + 8], q[24], m[SHA256_SIZE * 2 + 8];
        int rc = auth_check(cfg->auth, now_s(),
                            hdr_val(hdr, body, "X-Combo-Nonce", n, sizeof n),
                            hdr_val(hdr, body, "X-Combo-Seq", q, sizeof q),
                            hdr_val(hdr, body, "X-Combo-Mac", m, sizeof m),
                            method, signed_path, hash, 0);
        if (rc != AUTH_OK) {
            ota_finish(0);
            fail(s, keep, rc == AUTH_HELD ? 429 : 401,
                 rc == AUTH_HELD ? "Too Many Requests" : "Unauthorized",
                 auth_strerror(rc));
            return keep ? 0 : -1;
        }
    }

    if (ota_finish(ok) < 0) {
        fail(s, keep, 500, "Internal Server Error", ota_error());
        return keep ? 0 : -1;
    }

    {
        /* Перезагрузку заказывает морда отдельным запросом, а не мы сами:
           ответ должен дойти. Оборви мы соединение перезагрузкой, человек
           увидел бы «сеть недоступна» и не понял бы, приняла плата обновление
           или нет. */
        const char *out = what == OTA_APP
            ? "{\"ok\":true,\"reboot\":\"required\"}\n"
            : "{\"ok\":true,\"reboot\":\"not needed\"}\n";
        long k = (long)strlen(out);
        if (say(s, keep, 200, "OK", "application/json", k, 0) == 0)
            wr_all(s, out, (size_t)k);
    }
    return keep ? 0 : -1;
}

/* Выдрать строковое поле из JSON: "ssid":"дом". Свой разбор, а не библиотека,
   потому что полей ровно два и оба простые; тащить сюда парсер JSON значило бы
   отвечать на вопрос, которого никто не задавал. Экранирование понимаем только
   для кавычки и обратной косой — большего в имени сети и пароле не бывает. */
static int json_str(const char *body, long len, const char *key,
                    char *out, size_t max)
{
    char pat[24];
    const char *p, *end = body + len;
    size_t at = 0;
    int k = snprintf(pat, sizeof pat, "\"%s\"", key);

    out[0] = '\0';
    if (k <= 0 || len <= 0) return -1;
    for (p = body; p + k < end; p++)
        if (!strncmp(p, pat, (size_t)k)) break;
    if (p + k >= end) return -1;

    p += k;
    while (p < end && (*p == ' ' || *p == ':')) p++;
    if (p >= end || *p != '"') return -1;
    p++;
    while (p < end && *p != '"' && at + 1 < max) {
        if (*p == '\\' && p + 1 < end) p++;
        out[at++] = *p++;
    }
    out[at] = '\0';
    return 0;
}

/* Настройка сети. Все три запроса идут БЕЗ ПОДПИСИ и только пока плата в
 * режиме настройки: секрета человек ещё не знает, а спросить его неоткуда.
 * Безопасно это ровно потому, что в этот миг плата не в сети — дотянуться до
 * неё можно лишь через её собственную точку, стоя рядом. */
static void api_setup(int s, int keep, const http_cfg_t *cfg, const char *path,
                      const char *method, const char *body, long body_len)
{
    char out[512];
    int n;

    if (!cfg->setup) {
        fail(s, keep, 404, "Not Found", "the board is not in setup mode");
        return;
    }

    if (!strcmp(path, "/api/setup") && !strcmp(method, "GET")) {
        n = snprintf(out, sizeof out, "{\"setup\":true,\"secret\":\"%s\"}\n",
                     cfg->setup_secret ? cfg->setup_secret : "");
        if (say(s, keep, 200, "OK", "application/json", n, 0) == 0)
            wr_all(s, out, (size_t)n);
        return;
    }

    if (!strcmp(path, "/api/setup/nets") && !strcmp(method, "GET")) {
        static combo_net_t nets[24];
        int k = cfg->nets ? cfg->nets(nets, (int)(sizeof nets / sizeof nets[0])) : -1;
        int i;

        if (k < 0) { fail(s, keep, 503, "Service Unavailable", "scan failed"); return; }
        n = snprintf(out, sizeof out, "[");
        for (i = 0; i < k && n < (int)sizeof out - 80; i++)
            n += snprintf(out + n, sizeof out - (size_t)n,
                          "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                          i ? "," : "", nets[i].ssid, nets[i].rssi,
                          nets[i].secure ? "true" : "false");
        n += snprintf(out + n, sizeof out - (size_t)n, "]\n");
        if (say(s, keep, 200, "OK", "application/json", n, 0) == 0)
            wr_all(s, out, (size_t)n);
        return;
    }

    if (!strcmp(path, "/api/setup/join") && !strcmp(method, "POST")) {
        char ssid[33], pass[65];

        if (json_str(body, body_len, "ssid", ssid, sizeof ssid) < 0 || !ssid[0]) {
            fail(s, keep, 400, "Bad Request", "no network name given");
            return;
        }
        if (json_str(body, body_len, "pass", pass, sizeof pass) < 0) pass[0] = '\0';
        if (!cfg->join || cfg->join(ssid, pass) < 0) {
            fail(s, keep, 500, "Internal Server Error", "cannot store the network");
            return;
        }
        /* Перезагрузка — не лень, а честность. Подключиться на ходу значило бы
           уронить точку, через которую с нами сейчас говорят, и человек увидел
           бы обрыв, не зная, принято ли. А так ответ уходит, и плата поднимается
           уже в новой сети. */
        n = snprintf(out, sizeof out,
                     "{\"ok\":true,\"joining\":\"%s\"}\n", ssid);
        if (say(s, keep, 200, "OK", "application/json", n, 0) == 0)
            wr_all(s, out, (size_t)n);
        ota_reboot();
        return;
    }

    fail(s, keep, 404, "Not Found", "no such setup call");
}

/* Что известно про обновление и что с ним делать.
 *
 * Три действия под одним путём: узнать, спросить заново, поставить. Все под
 * подписью — это распоряжения плате, а не сведения о ней.
 *
 * Проверка и установка идут ДОЛГО: скачать мегабайт по чужой сети — секунды и
 * десятки секунд, и всё это время сервер занят, он однопоточный. Морда об этом
 * знает и ждёт; заводить ради такого отдельную задачу и опрос состояния
 * значило бы усложнить обе стороны ради дела, которое делают раз в месяц. */
static void api_update(int s, int keep, const http_cfg_t *cfg, const char *path,
                       const char *method)
{
    char out[400];
    int n;

    if (!cfg->update) {
        fail(s, keep, 501, "Not Implemented", "this build cannot update itself");
        return;
    }

    if (!strcmp(path, "/api/update/check") && !strcmp(method, "POST")) {
        cfg->update_check();
    }
    else if (!strcmp(path, "/api/update/apply") && !strcmp(method, "POST")) {
        if (cfg->update_apply() < 0) {
            const update_state_t *u = cfg->update();
            fail(s, keep, 502, "Bad Gateway",
                 u->error[0] ? u->error : "update failed");
            return;
        }
        n = snprintf(out, sizeof out, "{\"ok\":true,\"reboot\":\"%s\"}\n",
                     cfg->update()->reboot ? "required" : "none");
        if (say(s, keep, 200, "OK", "application/json", n, 0) == 0)
            wr_all(s, out, (size_t)n);
        return;
    }
    else if (strcmp(path, "/api/update") || strcmp(method, "GET")) {
        fail(s, keep, 404, "Not Found", "no such update call");
        return;
    }

    {
        const update_state_t *u = cfg->update();
        n = snprintf(out, sizeof out,
                     "{\"checked\":%s,\"available\":%s,\"app\":%s,\"fpga\":%s,"
                     "\"version\":\"%s\",\"error\":\"%s\"}\n",
                     u->checked ? "true" : "false",
                     u->available ? "true" : "false",
                     u->app ? "true" : "false",
                     u->fpga ? "true" : "false",
                     u->version, u->error);
        if (say(s, keep, 200, "OK", "application/json", n, 0) == 0)
            wr_all(s, out, (size_t)n);
    }
}

static void api_info(int s, int keep, const http_cfg_t *cfg)
{
    /* 640, а не 512: модель и серийник у настоящего привода длинные, и с
     * двумя счётчиками наблюдателя строка в 512 не влезает. Обрезанный JSON
     * морда не разберёт вовсе, и выглядело бы это как «плата не отвечает». */
    char body[1152];
    uint16_t cc; uint8_t ch, cs;
    int n;

    ata_chs(&cc, &ch, &cs);
    n = snprintf(body, sizeof body,
                 "{\"model\":\"%s\",\"serial\":\"%s\",\"sectors\":%u,"
                 "\"chs\":[%u,%u,%u],"
                 "\"sector_size\":%d,\"read_only\":%s,"
                 "\"max_count\":%d,\"scan_bytes\":%d,\"drive\":\"%s\","
                 "\"write_window\":%u,\"write_left\":%d,"
                 "\"vector_touched\":%s,\"vector_quiet\":%s,"
                 "\"rssi\":%d,\"signal_fair\":%d,\"signal_poor\":%d,"
                 "\"kvaz\":%u,\"kvaz_seen\":%u,\"kvaz_writes\":%u,"
                 "\"kvaz_std\":%u,\"kvaz_bar\":%u,\"kvaz_stk\":%u,"
                 "\"stk_unstable\":%u,\"ras_glitch\":%u,"
                 "\"stk_late\":%u,\"ras_lo_min\":%u,\"ras_hi_min\":%u,"
                 "\"rd_len_min\":%u,\"stb_after_min\":%u,"
                 "\"rd_ras_max\":%u,\"wr_ras_max\":%u,\"rd_len_max\":%u,"
                 "\"ras_to_rd_min\":%u,\"ras_to_rd_max\":%u,"
                 "\"stb_after_max\":%u,\"stb_max\":%u,"
                 "\"vid_state\":%u,\"vid_lost\":%u,"
                 "\"stb_before_min\":%u,\"stb_before_max\":%u,"
                 "\"firmware\":\"%s\",\"version\":\"%s\",\"setup\":%s}\n",
                 cfg->model ? cfg->model : "", cfg->serial ? cfg->serial : "",
                 /* Свежий объём, а не снятая при старте копия: после сброса
                  * привод переопознаётся, и число меняется. */
                 (unsigned)(ata_sectors() ? ata_sectors() : cfg->sectors),
                 (unsigned)cc, (unsigned)ch, (unsigned)cs,
                 ATA_SECTOR,
                 cfg->read_only ? "true" : "false",
                 HTTP_MAX_COUNT, HTTP_SCAN_BYTES, drive_word(),
                 (unsigned)cfg->write_window_s,
                 /* Сколько секунд разрешение ещё действует: -1 — окна не
                  * требуется, 0 — заперто. Морда рисует по этому обратный
                  * отсчёт, чтобы «почему не пишется» не было загадкой. */
                 cfg->read_only ? 0
                 : !cfg->write_window_s ? -1
                 : (int32_t)(now_s() - unlock_until) < 0
                   ? (int)(unlock_until - now_s()) : 0,
                 /* «Вектор» трогал привод с тех пор, как спрашивали в прошлый
                  * раз, — значит его каталог мог разойтись с нашим, и морде
                  * пора перечитать. И трогает ли прямо сейчас: пока нет
                  * тишины, запись не пройдёт, и лучше сказать об этом заранее,
                  * чем отказом на середине. */
                 ata_cpu_touched() ? "true" : "false",
                 ata_cpu_quiet()   ? "true" : "false",
                 /* Сила сигнала: ноль, когда радио молчит. */
                 cfg->rssi ? cfg->rssi() : 0,
                 /* Границы, по которым МОРДА красит показатель сигнала. Отдаёт
                  * их плата, а не морда знает сама, и это не педантизм: по этим
                  * же числам плата решает, перебираться ли на другую точку
                  * (см. roam.h). Знай их морда отдельно — они разошлись бы при
                  * первой же правке порогов, и цвет начал бы врать про то, что
                  * плата собирается делать. */
                 ROAM_POOR_SOFT, ROAM_POOR,
                 /* ДИАГНОСТИКА КВАЗИДИСКА. Регистр порта 0x10 на плате только
                  * для записи, и что пишет туда MicroDOS, иначе не узнать: от
                  * этого зависит выбранная страница, стековый режим и окна
                  * Баркаря (разряды 6-7 открывают 0x8000-0x9FFF и
                  * 0xE000-0xFFFF, то есть область экрана и верхнюю память).
                  * Липкий байт важнее прочих: одиночную запись мусора в
                  * регистр по текущему значению не поймать вообще. */
                 (unsigned)ata_kvaz_diag(0),
                 (unsigned)ata_kvaz_diag(1),
                 (unsigned)ata_kvaz_diag(2),
                 /* Обращений по окнам. kvaz_bar важнее прочих: если он не ноль,
                  * плата отвечает за 0x8000-0x9FFF и 0xE000-0xFFFF, то есть за
                  * область экрана и верхнюю память, где живёт сама MicroDOS. */
                 (unsigned)ata_kvaz_diag(3),
                 (unsigned)ata_kvaz_diag(4),
                 (unsigned)ata_kvaz_diag(5),
                 /* Наблюдатель за шиной: аномалии на СТЕК и ~RAS, bus_watch.v */
                 (unsigned)ata_kvaz_diag(6),
                 (unsigned)ata_kvaz_diag(7),
                 /* Второй блок: измерения наблюдателя. Минимумы приходят от
                  * 255 вниз, и 255 читается как «ни одного законченного
                  * уровня не видели» — например, на старом битстриме. */
                 (unsigned)ata_kvaz_diag2(0),
                 (unsigned)ata_kvaz_diag2(1),
                 (unsigned)ata_kvaz_diag2(2),
                 /* Минимумы по НАШИМ чтениям — длина строба и число спадов
                  * ~CAS внутри него. Установку адреса по ~ШАВВ мерить
                  * бесполезно: защёлку 574 тактует ~ШАП, которой в ПЛИС нет
                  * вовсе. */
                 (unsigned)ata_kvaz_diag2(3),
                 (unsigned)ata_kvaz_diag2(4),
                 /* Сколько спадов ~RAS попало ВНУТРЬ одного строба, сверх
                  * начального. Единица у чтения означала бы, что защёлка
                  * адреса внутри чтения не перезахватывается. */
                 (unsigned)ata_kvaz_diag2(5),
                 (unsigned)ata_kvaz_diag2(6),
                 (unsigned)ata_kvaz_diag2(7),
                 /* Фаза строба и ПЕРИОДИЧНОСТЬ ВИДЕОСПАДОВ ~RAS. По документу
                  * Шашкова (1994) такт 1 каждого цикла шины — обращение
                  * видеоадаптера, значит видеоспады идут строго через 1333 нс.
                  * Различает потоки НЕ по периоду (оба периодичны из-за READY),
                  * а по стробу: строб принадлежит предыдущему спаду. `cpu_off` —
                  * смещение процессорного спада от видео, оно и выдаёт подмену
                  * потока. `vid_state`: разряд 0 захват, 1 окно, 7:4 счёт
                  * совпадений. */
                 (unsigned)ata_kvaz_diag3(0),
                 (unsigned)ata_kvaz_diag3(1),
                 (unsigned)ata_kvaz_diag3(2),
                 (unsigned)ata_kvaz_diag3(3),
                 (unsigned)ata_kvaz_diag3(4),
                 (unsigned)ata_kvaz_diag3(5),
                 (unsigned)ata_kvaz_diag3(6),
                 (unsigned)ata_kvaz_diag3(7),
                 /* Из какого раздела работаем. После обновления это
                  * единственный способ убедиться, что плата поднялась с новой
                  * прошивки, а не откатилась к прежней. */
                 ota_running(),
                 /* Версия прошивки, а не имя раздела: раздел говорит, откуда
                  * плата загрузилась, и это полезно после обновления, но на
                  * вопрос «что у меня стоит» не отвечает вовсе. */
                 ota_version(),
                 /* Морда по этому признаку показывает не диск, а выбор сети. */
                 cfg->setup ? "true" : "false");
    /* snprintf возвращает длину, которая ПОНАДОБИЛАСЬ БЫ. Без этой проверки
     * wr_all при переполнении читал бы за границей буфера: ответ рос вместе с
     * числом полей, а размер буфера — нет. Обрезанный JSON морда не разберёт,
     * и это правильно: молча отдать половину хуже, чем явно сломаться. */
    if (n < 0) return;
    if (n >= (int)sizeof body) n = (int)sizeof body - 1;
    if (say(s, keep, 200, "OK", "application/json", n, 0) == 0)
        wr_all(s, body, (size_t)n);
}

/* Обход меток. Ответ собирается в памяти целиком — 32 Кбайт потолок, — потому
 * что иначе об ошибке на середине пришлось бы сообщать обрывом соединения:
 * длина в заголовке уже объявлена. */
static void api_scan(int s, int keep, uint32_t start, uint32_t stride,
                     uint32_t count, uint32_t len)
{
    uint8_t sec[ATA_SECTOR], *out;
    uint32_t i;
    int rc = ATA_OK;

    out = malloc((size_t)count * len);
    if (!out) { fail(s, keep, 503, "Service Unavailable", "out of memory"); return; }

    for (i = 0; i < count; i++) {
        rc = one_sector(0, start + i * stride, sec);
        if (rc != ATA_OK) break;
        memcpy(out + (size_t)i * len, sec, len);
    }

    if (rc != ATA_OK) {
        char msg[128];
        snprintf(msg, sizeof msg, "%s at label %u (sector %u)",
                 ata_strerror(rc), (unsigned)i, (unsigned)(start + i * stride));
        fail(s, keep, rc == ATA_E_RANGE ? 416 : 502,
             rc == ATA_E_RANGE ? "Range Not Satisfiable" : "Bad Gateway", msg);
    }
    else {
        char extra[64];
        snprintf(extra, sizeof extra, "X-Combo-Stride: %u\r\n", (unsigned)stride);
        if (say(s, keep, 200, "OK", "application/octet-stream",
                (long)count * len, extra) == 0)
            wr_all(s, out, (size_t)count * len);
    }
    free(out);
}

/* Разбор ?a=1&b=2: значение по имени или заданное по умолчанию. */
static unsigned long qval(const char *query, const char *name, unsigned long dflt)
{
    size_t n = strlen(name);
    const char *p = query;

    while (p && *p) {
        if (!strncmp(p, name, n) && p[n] == '=')
            return strtoul(p + n + 1, 0, 10);
        p = strchr(p, '&');
        if (p) p++;
    }
    return dflt;
}

/* Слово из ?k=vvod: копия до '&' или конца, не длиннее max-1. 0 -- нет такого. */
static int qword(const char *query, const char *name, char *out, size_t max)
{
    size_t n = strlen(name), k = 0;
    const char *p = query;

    while (p && *p) {
        if (!strncmp(p, name, n) && p[n] == '=') {
            p += n + 1;
            while (p[k] && p[k] != '&' && k + 1 < max) { out[k] = p[k]; k++; }
            out[k] = '\0';
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

/* Узлы ПЛИС помимо привода, board.h. Настройки меняются по одной: что не
 * названо в запросе, остаётся прежним. */
static void api_board(int s, int keep)
{
    char b[1400];
    int n = board_json(b, sizeof b);
    if (n < 0) return;
    if (n >= (int)sizeof b) n = (int)sizeof b - 1;
    if (say(s, keep, 200, "OK", "application/json", n, "Cache-Control: no-store\r\n") == 0)
        wr_all(s, b, (size_t)n);
}

static void api_board_settings(int s, int keep, const char *query)
{
    const unsigned long NONE = 0xFFFFFFFFul;
    board_settings_t st;
    unsigned long v;

    board_get(&st);
    if ((v = qval(query, "dev_off", NONE)) != NONE) st.dev_off  = (uint8_t)(v > 255 ? 255 : v);
    if ((v = qval(query, "fdc_rdy", NONE)) != NONE) st.fdc_rdy  = (uint8_t)(v > 255 ? 255 : v);
    if ((v = qval(query, "fdc_pc",  NONE)) != NONE) st.fdc_pc   = (uint8_t)(v > 255 ? 255 : v);
    if ((v = qval(query, "vga60",   NONE)) != NONE) st.vga60    = (uint8_t)(v > 255 ? 255 : v);
    if ((v = qval(query, "vi53",    NONE)) != NONE) st.vi53_off = v == 0 ? 1 : v == 1 ? 0 : 255;
    if ((v = qval(query, "boot",    NONE)) != NONE) st.boot     = (uint8_t)(v > 255 ? 255 : v);
    if ((v = qval(query, "fdd_cable", NONE)) != NONE) st.fdd_cable = (uint8_t)(v > 255 ? 255 : v);
    if (board_set(&st) != 0) {
        fail(s, keep, 400, "Bad Request",
             "dev_off 0..63, fdc_rdy 0..2, fdc_pc 0..31, vga60, vi53 and fdd_cable 0 or 1, boot 0..2");
        return;
    }
    api_board(s, keep);
}

/* Снимок экрана машины: 256 строк по 128 байт, раскладка в board.h, и за ними
 * 17 байт, если битстрим их отдаёт: палитра (BBGGGRRR, ячейки 0..15) и порт B
 * (разряд 4 -- режим 512 точек). Без них -- ровно 32768 байт, и цвета морда
 * берёт условные. */
/* Журнал записей в порты палитры (fpga/hdl/vid_rec.v): 2048 байт как есть, разбор
 * -- tools/combo.py palog. */
static void api_palog(int s, int keep)
{
    uint8_t *buf = malloc(PAL_LOG_BYTES);

    if (!buf) { fail(s, keep, 503, "Service Unavailable", "out of memory"); return; }
    if (board_pal_log(buf) != 0)
        fail(s, keep, 501, "Not Implemented", "no palette readout in this bitstream");
    else if (say(s, keep, 200, "OK", "application/octet-stream",
                 (long)PAL_LOG_BYTES, "Cache-Control: no-store\r\n") == 0)
        wr_all(s, buf, PAL_LOG_BYTES);
    free(buf);
}

static void api_screen(int s, int keep)
{
    const size_t fsz = (size_t)SCR_ROWS * SCR_ROW_BYTES;
    uint8_t *frame = malloc(fsz + SCR_PAL_BYTES);
    size_t n = fsz;
    int rc;

    if (!frame) { fail(s, keep, 503, "Service Unavailable", "out of memory"); return; }
    rc = board_screen(frame);
    if (rc == 0 && board_palette(frame + fsz) == 0) n += SCR_PAL_BYTES;
    if (rc == -1)
        fail(s, keep, 501, "Not Implemented", "no frame generator in this bitstream");
    else if (rc != 0)
        fail(s, keep, 502, "Bad Gateway", "screen row did not arrive: the fetch is not running");
    else if (say(s, keep, 200, "OK", "application/octet-stream",
                 (long)n, "Cache-Control: no-store\r\n") == 0)
        wr_all(s, frame, n);
    free(frame);
}

/* Прошивка ПЛИС по SSPI, fpga.h: образ частями, потом одна команда. */
static void api_fpga_part(int s, int keep, const char *query,
                          const uint8_t *body, long body_len)
{
    const char *msg = "";
    unsigned long off = qval(query, "off", 0xFFFFFFFFul);
    uint32_t have, total;
    char b[96];
    int rc, n;

    if (off == 0xFFFFFFFFul || body_len <= 0) {
        fail(s, keep, 400, "Bad Request", "off= and a body are required");
        return;
    }
    rc = fpga_part((uint32_t)off, body, (size_t)body_len, &msg);
    if (rc != 0) {
        fail(s, keep, rc == -2 ? 507 : 400, rc == -2 ? "Insufficient Storage" : "Bad Request", msg);
        return;
    }
    fpga_progress(&have, &total);
    n = snprintf(b, sizeof b, "{\"have\":%u,\"total\":%u}\n", (unsigned)have, (unsigned)total);
    if (say(s, keep, 200, "OK", "application/json", n, 0) == 0) wr_all(s, b, (size_t)n);
}

static void api_fpga_flash(int s, int keep, const char *query)
{
    const char *msg = "";
    sspi_report rep;
    char b[512];
    int flags = 0, rc, n;

    if (qval(query, "refresh", 1)) flags |= SSPI_REFRESH;
    if (qval(query, "force", 0))   flags |= SSPI_FORCE;
    /* Без сверки -- запасной выход: если сверка на живом кристалле разойдётся
     * ложно, DONE не выставится, и ПЛИС не загрузится после выключения. */
    if (!qval(query, "verify", 1)) flags |= SSPI_NOVERIFY;
    rc = fpga_flash(flags, &rep, &msg);
    n = snprintf(b, sizeof b,
                 "{\"ok\":%s,\"error\":\"%s\",\"idcode\":\"0x%08x\",\"status\":\"0x%08x\","
                 "\"row\":%u,\"feature\":\"%02x%02x%02x%02x%02x%02x%02x%02x\","
                 "\"feabits\":\"%02x%02x\",\"erase_ms\":%u,\"prog_ms\":%u,\"verify_ms\":%u}\n",
                 rc == SSPI_OK ? "true" : "false", rc == SSPI_OK ? "" : msg,
                 (unsigned)rep.idcode, (unsigned)rep.status, (unsigned)rep.row,
                 rep.feature[0], rep.feature[1], rep.feature[2], rep.feature[3],
                 rep.feature[4], rep.feature[5], rep.feature[6], rep.feature[7],
                 rep.feabits[0], rep.feabits[1],
                 (unsigned)rep.erase_ms, (unsigned)rep.prog_ms, (unsigned)rep.verify_ms);
    /* Коды: образ не тот -- 400, кристалл не тот или строка настроек --
     * 409 (повтор с force решает человек), нет SSPI -- 501, всё прочее --
     * отказ железа, 502. Тело одно: в нём отчёт. */
    {
        int code = rc == SSPI_OK ? 200 : rc == SSPI_E_IMAGE ? 400
                 : rc == SSPI_E_ID || rc == SSPI_E_FEATURE || rc == SSPI_E_LOCKED ? 409
                 : rc == SSPI_E_NOHW ? 501 : 502;
        const char *why = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 409 ? "Conflict"
                        : code == 501 ? "Not Implemented" : "Bad Gateway";
        if (n >= (int)sizeof b) n = (int)sizeof b - 1;
        if (say(s, keep, code, why, "application/json", n, 0) == 0) wr_all(s, b, (size_t)n);
    }
}

/* Клавиши и загрузчик: действие, а не настройка, -- ничего не сохраняется. */
static void api_board_act(int s, int keep, const char *query, int loader)
{
    char k[8];
    int what = -1, rc;

    if (qword(query, "k", k, sizeof k)) {
        if (!loader) what = !strcmp(k, "vvod") ? KEY_VVOD : !strcmp(k, "sbr") ? KEY_SBR
                          : !strcmp(k, "release") ? KEY_RELEASE : -1;
        else         what = !strcmp(k, "off") ? LOADER_OFF : !strcmp(k, "arm") ? LOADER_ARM
                          : !strcmp(k, "boot") ? LOADER_BOOT : -1;
    }
    if (what < 0) {
        fail(s, keep, 400, "Bad Request",
             loader ? "k=off|arm|boot" : "k=vvod|sbr|release");
        return;
    }
    rc = loader ? board_loader(what) : board_keys(what);
    if (rc != 0) {
        fail(s, keep, 501, "Not Implemented",
             loader ? "no boot loader in this bitstream" : "no keys in this bitstream");
        return;
    }
    api_board(s, keep);
}

/* Ответ собирается в памяти ЦЕЛИКОМ, и только потом уходит.
 *
 * При отдаче потоком сразу за заголовком у запоздалой ошибки нет места:
 * `Content-Length` уже объявлен, и сбой на середине остаётся сообщать обрывом
 * соединения. Клиент видит, что ответ не дочитан, но не знает ни причины
 * (сбойный сектор? выход за край? отобрали шину?), ни номера сектора — а
 * значит не может ни показать внятное, ни повторить точечно.
 *
 * Поэтому читаем всё, а потом отвечаем: любая беда превращается в честный код
 * с номером сектора в теле. Платим пределом в 64 сектора, то есть 32 Кбайт. */
static int api_read(int s, int keep, uint32_t lba, uint32_t count)
{
    size_t len = (size_t)count * ATA_SECTOR;
    uint8_t *buf = malloc(len);
    uint32_t at = lba;
    int rc, sent;

    if (!buf) { fail(s, keep, 503, "Service Unavailable", "out of memory"); return keep ? 0 : -1; }

    rc = read_span(lba, count, buf, &at);

    if (rc != ATA_OK) {
        char msg[128];
        /* Состояние привода в сообщении: без него «не поднял DRQ» ничего не
         * говорит о том, чем он в это время был занят. */
        snprintf(msg, sizeof msg, "%s at sector %u, status 0x%02x, error 0x%02x",
                 ata_strerror(rc), (unsigned)at, ata_last_status(),
                 ata_last_error());
        fail(s, keep, rc == ATA_E_RANGE ? 416 : 502,
             rc == ATA_E_RANGE ? "Range Not Satisfiable" : "Bad Gateway", msg);
        free(buf);
        return keep ? 0 : -1;
    }

    sent = say(s, keep, 200, "OK", "application/octet-stream",
               (long)len, "Cache-Control: no-store\r\n") == 0
           && wr_all(s, buf, len) == 0;
    free(buf);
    return sent && keep ? 0 : -1;
}

/* pre/pre_len — начало тела, уже прочитанное вместе с заголовком. Тело часто
 * приходит тем же пакетом, и без этого куска чтение поехало бы со сдвигом. */
/* Тело приходит уже прочитанным целиком: его хеш входит в подпись, и читать
 * его по ходу записи нельзя — проверять подпись было бы поздно. */
static int api_write(int s, int keep, const http_cfg_t *cfg, uint32_t lba,
                     const uint8_t *buf, long body_len)
{
    uint32_t n, i;
    int rc = ATA_OK;

    if (body_len <= 0 || body_len % ATA_SECTOR) {
        fail(s, keep, 400, "Bad Request", "body length must be a multiple of 512");
        return keep ? 0 : -1;
    }
    n = (uint32_t)(body_len / ATA_SECTOR);
    if (n > HTTP_MAX_COUNT) {
        fail(s, keep, 413, "Payload Too Large", "too many sectors");
        return keep ? 0 : -1;
    }

    /* Всю запись держим шину ДОЛГИМ захватом: между секторами процессор не
     * должен вклиниться. Иначе машина увидит диск наполовину переписанным, а
     * её собственный кэш — тем более. Зависшую сторону страхует сторож в ПЛИС.
     *
     * Вложенные захваты внутри ata_write режима не меняют. */
    if (cfg->read_only) {
        fail(s, keep, 403, "Forbidden", "disk is exported read-only");
        return keep ? 0 : -1;
    }
    if (!writable(cfg)) {
        fail(s, keep, 403, "Forbidden", "writing is locked, POST /api/unlock first");
        return keep ? 0 : -1;
    }
    if (ata_lock_long() != ATA_OK) {
        fail(s, keep, 503, "Service Unavailable", "bus not granted");
        return keep ? 0 : -1;
    }

    for (i = 0; i < n && rc == ATA_OK; i++)
        rc = one_sector(1, lba + i, (uint8_t *)(buf + (size_t)i * ATA_SECTOR));

    ata_unlock();

    if (rc == ATA_E_RANGE)   fail(s, keep, 416, "Range Not Satisfiable", ata_strerror(rc));
    /* 409, а не 502: это не сбой, а занятость, и повторить попытку через
     * секунду совершенно разумно. Клиент по коду и отличает одно от другого,
     * не разбирая текст. */
    else if (rc == ATA_E_CPU_BUSY)
        fail(s, keep, 409, "Conflict", ata_strerror(rc));
    else if (rc != ATA_OK)   fail(s, keep, 502, "Bad Gateway", ata_strerror(rc));
    else {
        const char *ok = "{\"written\":%u}\n";
        char body[64];
        int k = snprintf(body, sizeof body, ok, (unsigned)n);
        if (say(s, keep, 200, "OK", "application/json", k, 0) == 0)
            wr_all(s, body, (size_t)k);
    }
    return keep ? 0 : -1;
}

/* --------------------------------------------------------------- разбор */

int http_listen(uint16_t port)
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
    if (listen(ls, 8) < 0) { close(ls); return -1; }
    return ls;
}

/* Разбор пути и вызов обработчика. Вынесено из http_request отдельной
 * функцией не ради красоты: тело запроса лежит в куче, выходов отсюда полтора
 * десятка, и освобождать его на каждом — верный способ однажды забыть. */
static int dispatch(int fd, int keep, const http_cfg_t *cfg,
                    const char *line, const char *path, const char *query,
                    const uint8_t *bodybuf, long body_len)
{
    if (strncmp(path, "/api/", 5) != 0) {
        if (strcmp(line, "GET") != 0) {
            fail(fd, keep, 405, "Method Not Allowed", "GET only");
            return keep ? 0 : -1;
        }
        serve_file(fd, keep, cfg, path);
        return keep ? 0 : -1;
    }

    if (!strncmp(path, "/api/update", 11)) {
        api_update(fd, keep, cfg, path, line);
        return keep ? 0 : -1;
    }
    if (!strncmp(path, "/api/setup", 10)) {
        api_setup(fd, keep, cfg, path, line, (const char *)bodybuf, body_len);
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/info") && !strcmp(line, "GET")) {
        api_info(fd, keep, cfg);
        return keep ? 0 : -1;
    }
    /* Что плата видит в эфире. Ровно то же, что печатает консольная команда
     * wifi, — и по той же нужде: «сигнал слабый» звучит неправдоподобно, пока
     * не увидишь, что точек с одним именем несколько и выбрана не та.
     *
     * Отдаётся ТЕКСТОМ. Морда показывает его как есть, без разбора: это ответ
     * человеку, и раскладывать его по полям, чтобы потом собрать обратно,
     * незачем. */
    if (!strcmp(path, "/api/wifi") && !strcmp(line, "GET")) {
        api_text(fd, keep, cfg->survey, "no radio on this build");
        return keep ? 0 : -1;
    }
    /* Журнал платы. Читается ровно тогда, когда что-то пошло не так, и до сих
     * пор для этого нужен был кабель — при том что плату ставят ради обратного. */
    if (!strcmp(path, "/api/log") && !strcmp(line, "GET")) {
        api_text(fd, keep, cfg->log, "this build keeps no log");
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/scan") && !strcmp(line, "GET")) {
        unsigned long start  = qval(query, "start", 0);
        unsigned long stride = qval(query, "stride", 0);
        unsigned long count  = qval(query, "count", 0);
        unsigned long len    = qval(query, "len", 64);

        if (!stride || !count)
            fail(fd, keep, 400, "Bad Request", "stride and count are required");
        else if (len < 1 || len > ATA_SECTOR)
            fail(fd, keep, 400, "Bad Request", "len must be 1..512");
        else if (count * len > HTTP_SCAN_BYTES)
            fail(fd, keep, 400, "Bad Request", "count*len over the limit");
        /* Переполнение проверяем до обращения к диску, а не после. */
        else if (start > 0x0FFFFFFFul || stride > 0x0FFFFFFFul ||
                 start + (count - 1) * stride > 0x0FFFFFFFul)
            fail(fd, keep, 416, "Range Not Satisfiable", "scan runs past LBA28");
        else
            api_scan(fd, keep, (uint32_t)start, (uint32_t)stride,
                     (uint32_t)count, (uint32_t)len);
        return keep ? 0 : -1;
    }
    /* Разрешение записи. Повтор ему НЕ безразличен — он продлевает окно, — и
     * потому проверка требует уже заведённой сессии (need_slot в auth.c). */
    if (!strcmp(path, "/api/unlock") && !strcmp(line, "POST")) {
        char out[96];
        int k;
        if (cfg->read_only) {
            fail(fd, keep, 403, "Forbidden", "disk is exported read-only");
            return keep ? 0 : -1;
        }
        if (!cfg->write_window_s) {
            fail(fd, keep, 409, "Conflict", "no write window configured");
            return keep ? 0 : -1;
        }
        unlock_until = now_s() + cfg->write_window_s;
        k = snprintf(out, sizeof out, "{\"write_left\":%u}\n",
                     (unsigned)cfg->write_window_s);
        if (say(fd, keep, 202, "Accepted", "application/json", k, 0) == 0)
            wr_all(fd, out, (size_t)k);
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/lock") && !strcmp(line, "POST")) {
        unlock_until = now_s();          /* сразу истёкшее */
        if (say(fd, keep, 204, "No Content", "application/json", 0, 0) != 0) keep = 0;
        return keep ? 0 : -1;
    }

    /* «Я учла, что «Вектор» трогал привод» — снять липкий признак.
     *
     * Без этого признак бесполезен: он взводится любым обращением машины и,
     * раз поднявшись, стоит навсегда, так что отличить «трогала когда-то» от
     * «трогала пока мы читали» было бы нечем. Порядок такой: снять — прочитать
     * — спросить снова. */
    /* Перезагрузка — отдельным запросом, после обновления прошивки. Разрешение
     * на запись не требуется: диска это не касается вовсе, а сессия и так
     * проверена подписью. */
    if (!strcmp(path, "/api/reboot") && !strcmp(line, "POST")) {
        if (say(fd, keep, 202, "Accepted", "application/json",
                (long)sizeof "{\"rebooting\":true}\n" - 1, 0) == 0)
            wr_all(fd, "{\"rebooting\":true}\n", sizeof "{\"rebooting\":true}\n" - 1);
        ota_reboot();
        return -1;
    }
    /* Забыть сеть и перезагрузиться в настройку. Под подписью: у платы,
     * которая уже в сети, это действие уводит её из этой сети, и делать такое
     * без спросу нельзя. Нужно, когда сеть сменилась, а шнурка нет. */
    if (!strcmp(path, "/api/wifi/forget") && !strcmp(line, "POST")) {
        if (!cfg->join || cfg->join("", "") < 0)
            fail(fd, keep, 500, "Internal Server Error", "cannot clear the network");
        else {
            static const char out[] = "{\"forgotten\":true}\n";
            if (say(fd, keep, 202, "Accepted", "application/json",
                    (long)sizeof out - 1, 0) == 0)
                wr_all(fd, out, sizeof out - 1);
            ota_reboot();
        }
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/forget") && !strcmp(line, "POST")) {
        ata_cpu_forget();
        if (say(fd, keep, 204, "No Content", "application/json", 0, 0) != 0) keep = 0;
        return keep ? 0 : -1;
    }

    /* Сброс НЕ ждёт завершения: привод поднимается секунды, а мёртвый не
     * поднимется никогда, и всё это время цикл сервера никого бы не обслужил.
     * Отвечаем «начат», дальше клиент смотрит поле drive в /api/info. */
    /* Опознание ПЛИС по JTAG. В кристалле не меняет НИЧЕГО: после сброса TAP
     * в IR по стандарту стоит IDCODE, и мы лишь сдвигаем тридцать два бита.
     * Годится как первая проверка проводов JTAG — до того, как этим путём
     * начнут что-то писать. */
    /* Прозвонка проводов JTAG. Отвечает на вопрос, который сам JTAG решить не
     * может: провод не припаян или кристалл не отвечает. Разбор признаков —
     * в jtag.h. */
    /* Поиск замыканий между линиями JTAG: одну вести вниз, остальные слушать.
     * Вольтметром этого не поймать, если все выходы задать одинаковыми: при
     * равных уровнях перемычка невидима. */
    if (!strcmp(path, "/api/jtag/bridge") && !strcmp(line, "GET")) {
        int m[4][4], i, j;
        if (jtag_bridges(m) != 0) {
            fail(fd, keep, 501, "Not Implemented", "no JTAG on this build");
        } else {
            char b[256];
            int k = snprintf(b, sizeof b, "{\"m\":[");
            for (i = 0; i < 4; i++) {
                k += snprintf(b + k, sizeof b - (size_t)k, "%s[", i ? "," : "");
                for (j = 0; j < 4; j++)
                    k += snprintf(b + k, sizeof b - (size_t)k,
                                  "%s%d", j ? "," : "", m[i][j]);
                k += snprintf(b + k, sizeof b - (size_t)k, "]");
            }
            k += snprintf(b + k, sizeof b - (size_t)k, "]}\n");
            if (say(fd, keep, 200, "OK", "application/json", k, 0) == 0)
                wr_all(fd, b, (size_t)k);
        }
        return keep ? 0 : -1;
    }
    /* Пройти по автомату TAP вручную и замереть. Состояние между вызовами
     * сохраняется: без фронтов TCK автомат не движется. Позволяет довести TAP
     * до Shift-DR отдельными шагами и померить TDO вольтметром. */
    if (!strcmp(path, "/api/jtag/step") && !strcmp(line, "GET")) {
        int tms  = (int)qval(query, "tms", 0);
        int tdi  = (int)qval(query, "tdi", 0);
        int clk  = (int)qval(query, "clocks", 1);
        int secs = (int)qval(query, "secs", 5);
        jtag_speed((int)qval(query, "us", 1));
        if (jtag_step(tms, tdi, clk, secs) != 0) {
            fail(fd, keep, 501, "Not Implemented", "no JTAG on this build");
        } else {
            char b[96];
            int n = snprintf(b, sizeof b,
                             "{\"clocks\":%d,\"tms\":%d,\"tdi\":%d,\"secs\":%d}\n",
                             clk, tms, tdi, secs);
            if (say(fd, keep, 200, "OK", "application/json", n, 0) == 0)
                wr_all(fd, b, (size_t)n);
        }
        return keep ? 0 : -1;
    }
    /* Удержать постоянные уровни на выходах JTAG, чтобы померить их
     * вольтметром на разъёме. Такт в полмегагерца вольтметром не поймать, а
     * постоянный уровень — можно; это последний способ узнать, доходят ли наши
     * уровни до разъёма, когда провода прозвонены, а автомат не двигается. */
    if (!strcmp(path, "/api/jtag/hold") && !strcmp(line, "GET")) {
        int tck  = (int)qval(query, "tck", 0);
        int tms  = (int)qval(query, "tms", 0);
        int tdi  = (int)qval(query, "tdi", 0);
        int secs = (int)qval(query, "secs", 5);
        if (jtag_hold(tck, tms, tdi, secs) != 0) {
            fail(fd, keep, 501, "Not Implemented", "no JTAG on this build");
        } else {
            char b[96];
            int n = snprintf(b, sizeof b,
                             "{\"held\":{\"tck\":%d,\"tms\":%d,\"tdi\":%d},"
                             "\"secs\":%d}\n", tck, tms, tdi, secs);
            if (say(fd, keep, 200, "OK", "application/json", n, 0) == 0)
                wr_all(fd, b, (size_t)n);
        }
        return keep ? 0 : -1;
    }
    /* Поиск заблудившегося провода: опросить разом все свободные ноги ESP.
     * Признак искомой — держится в единице под нашей подтяжкой вниз. */
    if (!strcmp(path, "/api/jtag/scan") && !strcmp(line, "GET")) {
        int pin[16], up[16], down[16], n = 16, i;
        if (jtag_scan(pin, up, down, &n) != 0) {
            fail(fd, keep, 501, "Not Implemented", "no JTAG on this build");
        } else {
            char b[512];
            int k = snprintf(b, sizeof b, "{\"pins\":[");
            for (i = 0; i < n && k < (int)sizeof b - 40; i++)
                k += snprintf(b + k, sizeof b - (size_t)k,
                              "%s{\"pin\":%d,\"up\":%d,\"down\":%d}",
                              i ? "," : "", pin[i], up[i], down[i]);
            k += snprintf(b + k, sizeof b - (size_t)k, "]}\n");
            if (say(fd, keep, 200, "OK", "application/json", k, 0) == 0)
                wr_all(fd, b, (size_t)k);
        }
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/jtag/pins") && !strcmp(line, "GET")) {
        jtag_pins p;
        if (jtag_check_pins(&p) != 0) {
            fail(fd, keep, 501, "Not Implemented", "no JTAG on this build");
        } else {
            char b[160];
            int n = snprintf(b, sizeof b,
                "{\"up\":[%d,%d,%d,%d],\"down\":[%d,%d,%d,%d]}\n",
                p.up[0], p.up[1], p.up[2], p.up[3],
                p.down[0], p.down[1], p.down[2], p.down[3]);
            if (say(fd, keep, 200, "OK", "application/json", n, 0) == 0)
                wr_all(fd, b, (size_t)n);
        }
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/jtag") && !strcmp(line, "GET")) {
        uint32_t id = 0;
        jtag_speed((int)qval(query, "us", 1));
        if (jtag_probe(&id) != 0) {
            fail(fd, keep, 501, "Not Implemented", "no JTAG on this build");
        } else {
            /* Три разных сведения, и каждое отдается отдельно: сам код (он
               прочитан в любом случае), опознан ли он, и чей он. Исправная
               плата с другим кристаллом не должна выглядеть как «неизвестный
               кристалл». */
            const char *dev = jtag_device_name(id);
            char b[160];
            int n = snprintf(b, sizeof b,
                             "{\"idcode\":\"%08X\",\"known\":%s,"
                             "\"device\":\"%s\",\"board\":%s}\n",
                             (unsigned)id, dev ? "true" : "false",
                             dev ? dev : "",
                             id == JTAG_IDCODE_BOARD ? "true" : "false");
            if (say(fd, keep, 200, "OK", "application/json", n, 0) == 0)
                wr_all(fd, b, (size_t)n);
        }
        return keep ? 0 : -1;
    }
    /* Открыть JTAG по сети (XVC, xvc.h) на `secs` секунд — только для того
     * адреса, с которого пришла эта подписанная команда. Заливка битстрима
     * дальше идёт с компьютера openFPGALoader'ом. */
    if (!strcmp(path, "/api/jtag/xvc") && !strcmp(line, "POST")) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int secs = (int)qval(query, "secs", 600);
        if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0) {
            fail(fd, keep, 500, "Internal Server Error", "no peer address");
        } else if (jtag_xvc_open(peer.sin_addr.s_addr, secs) != 0) {
            fail(fd, keep, 501, "Not Implemented", "no JTAG on this build");
        } else {
            char b[64];
            int n = snprintf(b, sizeof b, "{\"port\":%d,\"secs\":%d}\n",
                             XVC_PORT, secs);
            if (say(fd, keep, 200, "OK", "application/json", n, 0) == 0)
                wr_all(fd, b, (size_t)n);
        }
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/board") && !strcmp(line, "GET")) {
        api_board(fd, keep);
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/fpga") && !strcmp(line, "GET")) {
        char b[512];
        int n = fpga_json(b, sizeof b);
        if (n >= (int)sizeof b) n = (int)sizeof b - 1;
        if (n > 0 && say(fd, keep, 200, "OK", "application/json", n, 0) == 0)
            wr_all(fd, b, (size_t)n);
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/fpga/part") && !strcmp(line, "POST")) {
        api_fpga_part(fd, keep, query, bodybuf, body_len);
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/fpga/flash") && !strcmp(line, "POST")) {
        api_fpga_flash(fd, keep, query);
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/screen") && !strcmp(line, "GET")) {
        api_screen(fd, keep);
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/palog") && !strcmp(line, "GET")) {
        api_palog(fd, keep);
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/board/settings") && !strcmp(line, "POST")) {
        api_board_settings(fd, keep, query);
        return keep ? 0 : -1;
    }
    if ((!strcmp(path, "/api/board/keys") || !strcmp(path, "/api/board/loader"))
        && !strcmp(line, "POST")) {
        api_board_act(fd, keep, query, path[11] == 'l');
        return keep ? 0 : -1;
    }
    /* Обнулить счётчики наблюдателя за шиной. Без этого насыщенный счётчик
     * говорит лишь «за всё время с включения хоть раз было»; со сбросом можно
     * мерить заданный отрезок — скажем, минуту прогона теста. */
    if (!strcmp(path, "/api/diag/clear") && !strcmp(line, "POST")) {
        const char *b = "{\"ok\":true}\n";
        ata_kvaz_diag_clr();
        if (say(fd, keep, 200, "OK", "application/json",
                (long)strlen(b), 0) == 0)
            wr_all(fd, b, strlen(b));
        return keep ? 0 : -1;
    }
    /* Заморозка защёлки адреса — опыт, разбор в fpga/hdl/vu_bus.v. Уровень в ПЛИС,
     * по сбросу кристалла возвращается в ноль. */
    if ((!strcmp(path, "/api/ceby/cas") || !strcmp(path, "/api/ceby/stb"))
        && !strcmp(line, "POST")) {
        const char *b = "{\"ok\":true}\n";
        ata_kvaz_ceby(path[10] == 'c');
        if (say(fd, keep, 200, "OK", "application/json",
                (long)strlen(b), 0) == 0)
            wr_all(fd, b, strlen(b));
        return keep ? 0 : -1;
    }
    if ((!strcmp(path, "/api/cegate/on") || !strcmp(path, "/api/cegate/off"))
        && !strcmp(line, "POST")) {
        const char *b = "{\"ok\":true}\n";
        ata_kvaz_cegate(path[12] == 'o' && path[13] == 'n');
        if (say(fd, keep, 200, "OK", "application/json",
                (long)strlen(b), 0) == 0)
            wr_all(fd, b, strlen(b));
        return keep ? 0 : -1;
    }
    if ((!strcmp(path, "/api/latch/on") || !strcmp(path, "/api/latch/off"))
        && !strcmp(line, "POST")) {
        const char *b = "{\"ok\":true}\n";
        ata_kvaz_latch(path[11] == 'o' && path[12] == 'n');
        if (say(fd, keep, 200, "OK", "application/json",
                (long)strlen(b), 0) == 0)
            wr_all(fd, b, strlen(b));
        return keep ? 0 : -1;
    }
    if ((!strcmp(path, "/api/vid/pola") || !strcmp(path, "/api/vid/polb"))
        && !strcmp(line, "POST")) {
        const char *b = "{\"ok\":true}\n";
        ata_kvaz_vid_pol(path[12] == 'b');
        if (say(fd, keep, 200, "OK", "application/json",
                (long)strlen(b), 0) == 0)
            wr_all(fd, b, strlen(b));
        return keep ? 0 : -1;
    }
    if ((!strcmp(path, "/api/vid/on") || !strcmp(path, "/api/vid/off"))
        && !strcmp(line, "POST")) {
        const char *b = "{\"ok\":true}\n";
        ata_kvaz_vid(path[9] == 'o' && path[10] == 'n');
        if (say(fd, keep, 200, "OK", "application/json",
                (long)strlen(b), 0) == 0)
            wr_all(fd, b, strlen(b));
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/reset") && !strcmp(line, "POST")) {
        int rc = ata_reset_start();
        if (rc != ATA_OK) fail(fd, keep, 502, "Bad Gateway", ata_strerror(rc));
        else {
            const char *b = "{\"drive\":\"resetting\"}\n";
            if (say(fd, keep, 202, "Accepted", "application/json",
                    (long)strlen(b), 0) == 0)
                wr_all(fd, b, strlen(b));
        }
        return keep ? 0 : -1;
    }
    if (!strcmp(path, "/api/flush") && !strcmp(line, "POST")) {
        int rc = ata_flush();
        if (rc != ATA_OK) fail(fd, keep, 502, "Bad Gateway", ata_strerror(rc));
        else {
            const char *b = "{\"ok\":true}\n";
            if (say(fd, keep, 200, "OK", "application/json", (long)strlen(b), 0) == 0)
                wr_all(fd, b, strlen(b));
        }
        return keep ? 0 : -1;
    }
    if (!strncmp(path, "/api/sectors/", 13)) {
        char *end;
        unsigned long lba = strtoul(path + 13, &end, 10);
        unsigned long count;

        if (end == path + 13 || *end) {
            fail(fd, keep, 400, "Bad Request", "malformed sector number");
            return keep ? 0 : -1;
        }
        if (lba > 0x0FFFFFFFul) {
            fail(fd, keep, 416, "Range Not Satisfiable", "sector number outside LBA28");
            return keep ? 0 : -1;
        }
        if (!strcmp(line, "GET")) {
            count = qval(query, "count", 1);
            if (count < 1 || count > HTTP_MAX_COUNT) {
                fail(fd, keep, 400, "Bad Request", "count out of range");
                return keep ? 0 : -1;
            }
            return api_read(fd, keep, (uint32_t)lba, (uint32_t)count);
        }
        if (!strcmp(line, "PUT"))
            return api_write(fd, keep, cfg, (uint32_t)lba, bodybuf, body_len);

        fail(fd, keep, 405, "Method Not Allowed", "GET and PUT only");
        return keep ? 0 : -1;
    }
    fail(fd, keep, 404, "Not Found", "no such path");
    return keep ? 0 : -1;
}

int http_request(int fd, const http_cfg_t *cfg)
{
    char hdr[HDR_MAX + 1], *body, *line, *path, *query, *ver;
    char signed_path[PATH_MAX_ + 64];
    uint8_t body_hash[SHA256_SIZE], *bodybuf = 0;
    size_t got = 0, body_have = 0;
    long body_len = 0;
    int keep = 1;

    /* Заголовок читаем до пустой строки. Соединение может молчать — браузер
     * открывает их впрок, — поэтому на приёме стоит таймаут, выставленный
     * вызывающим: без него одно такое соединение застопорило бы весь цикл. */
    for (;;) {
        ssize_t k = recv(fd, hdr + got, HDR_MAX - got, 0);
        if (k <= 0) return -1;
        got += (size_t)k;
        hdr[got] = '\0';
        if (strstr(hdr, "\r\n\r\n")) break;
        if (got >= HDR_MAX) {
            fail(fd, 0, 431, "Request Header Fields Too Large", "request header too large");
            return -1;
        }
    }
    body = strstr(hdr, "\r\n\r\n") + 4;
    body_have = got - (size_t)(body - hdr);

    {   char *p;
        for (p = hdr; p < body; p++) {
            /* Заголовки ищем без учёта регистра, как велит стандарт. */
            if ((*p == 'C' || *p == 'c') && !strncasecmp(p, "Content-Length:", 15))
                body_len = strtol(p + 15, 0, 10);
            else if ((*p == 'C' || *p == 'c') && !strncasecmp(p, "Connection:", 11)) {
                char *v = p + 11;
                while (*v == ' ') v++;
                if (!strncasecmp(v, "close", 5)) keep = 0;
            }
        }
    }

    line = hdr;
    path = strchr(line, ' ');
    if (!path) { fail(fd, 0, 400, "Bad Request", "malformed request"); return -1; }
    *path++ = '\0';
    ver = strchr(path, ' ');
    if (!ver) { fail(fd, 0, 400, "Bad Request", "malformed request"); return -1; }
    *ver++ = '\0';
    /* HTTP/1.0 держать соединение не просит, если только не сказал явно. */
    if (!strncmp(ver, "HTTP/1.0", 8)) keep = 0;

    /* Путь с запросом целиком — его покрывает подпись, поэтому копия снимается
     * ДО разделения: иначе «?count=64» можно было бы подменить безнаказанно. */
    snprintf(signed_path, sizeof signed_path, "%s", path);

    query = strchr(path, '?');
    if (query) *query++ = '\0';

    /* Вызов выдаётся БЕЗ подписи — с него всё и начинается. Состояния он не
     * заводит (см. auth.h), поэтому дёргать его без счёта бесполезно. */
    if (!strcmp(path, "/api/challenge") && !strcmp(line, "GET")) {
        char nonce[AUTH_NONCE_HEX + 1], out[128];
        int k;
        if (body_len > 0 && drain(fd, body_len, (const uint8_t *)body, body_have) < 0)
            keep = 0;
        if (!cfg->auth || !auth_on(cfg->auth)) {
            fail(fd, keep, 404, "Not Found", "authentication is disabled");
            return keep ? 0 : -1;
        }
        auth_challenge(cfg->auth, now_s(), nonce);
        k = snprintf(out, sizeof out, "{\"nonce\":\"%s\",\"ttl\":%u}\n",
                     nonce, (unsigned)AUTH_TTL_S);
        if (say(fd, keep, 200, "OK", "application/json", k, 0) == 0)
            wr_all(fd, out, (size_t)k);
        return keep ? 0 : -1;
    }

    /* ОБНОВЛЕНИЕ ИДЁТ МИМО ОБЩЕГО ПУТИ, и не по прихоти: прошивка весит около
     * мегабайта, а свободной памяти на плате сотни килобайт. Держать тело
     * целиком, как держат его все прочие запросы, тут нельзя.
     *
     * Значит и подпись проверяется иначе — не до дела, а после. Данные текут в
     * запасной раздел, хеш считается по дороге, и лишь когда он сошёлся с
     * подписью, новая прошивка объявляется загрузочной. Не сошёлся — раздел
     * остаётся невостребованным, будто ничего и не было. */
    if (!strncmp(path, "/api/ota", 8) && !strcmp(line, "POST")) {
        return api_ota(fd, keep, cfg, path, signed_path, line,
                       hdr, body, body_have, body_len);
    }

    /* Тело читается ЦЕЛИКОМ и до всего прочего: его хеш входит в подпись, и
     * проверять её после того, как сектора уже записаны, было бы поздно. */
    if (body_len > 0) {
        if (body_len > HTTP_MAX_BODY) {
            if (drain(fd, body_len, (const uint8_t *)body, body_have) < 0) keep = 0;
            fail(fd, keep, 413, "Payload Too Large", "body over the limit");
            return keep ? 0 : -1;
        }
        bodybuf = malloc((size_t)body_len);
        if (!bodybuf) {
            if (drain(fd, body_len, (const uint8_t *)body, body_have) < 0) keep = 0;
            fail(fd, keep, 503, "Service Unavailable", "out of memory");
            return keep ? 0 : -1;
        }
        {
            size_t take = body_have < (size_t)body_len ? body_have : (size_t)body_len;
            memcpy(bodybuf, body, take);
            if (take < (size_t)body_len &&
                rd_all(fd, bodybuf + take, (size_t)body_len - take) < 0) {
                free(bodybuf);
                return -1;
            }
        }
    }
    combo_sha256(bodybuf, (size_t)(body_len > 0 ? body_len : 0), body_hash);

    /* Проверка доступа. Статика идёт без неё: подписать обычную загрузку
     * страницы браузер не может, её тянет он сам, а не наш скрипт. Секретов в
     * статике нет — см. оговорку в http.h. */
    /* Настройка идёт без подписи — но ТОЛЬКО в режиме настройки. У платы,
     * которая уже в сети, /api/setup отвечает «не в режиме настройки», и мимо
     * проверки этот путь не пускает никого. */
    if (cfg->auth && auth_on(cfg->auth) && !strncmp(path, "/api/", 5) &&
        !(cfg->setup && !strncmp(path, "/api/setup", 10))) {
        char n[AUTH_NONCE_HEX + 8], q[24], m[SHA256_SIZE * 2 + 8], msg[128];
        /* Повтор безразличен везде, кроме сброса привода: там он не действие
         * над данными, а рывок железа. Ему нужна уже заведённая сессия. */
        int need = !strcmp(path, "/api/reset") || !strcmp(path, "/api/unlock");
        int rc = auth_check(cfg->auth, now_s(),
                            hdr_val(hdr, body, "X-Combo-Nonce", n, sizeof n),
                            hdr_val(hdr, body, "X-Combo-Seq", q, sizeof q),
                            hdr_val(hdr, body, "X-Combo-Mac", m, sizeof m),
                            line, signed_path, body_hash, need);
        if (rc != AUTH_OK) {
            free(bodybuf);
            snprintf(msg, sizeof msg, "%s", auth_strerror(rc));
            fail(fd, keep, rc == AUTH_HELD ? 429 : 401,
                 rc == AUTH_HELD ? "Too Many Requests" : "Unauthorized", msg);
            return keep ? 0 : -1;
        }
    }

    {
        int r = dispatch(fd, keep, cfg, line, path, query, bodybuf, body_len);
        free(bodybuf);
        return r;
    }
}
