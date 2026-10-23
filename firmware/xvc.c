/* Xilinx Virtual Cable — разбор протокола. Описание в xvc.h. */
#include "xvc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

/* Буферы статические, а не на стеке: три по два килобайта больше стека задачи
 * на ESP32, а переполнение там не падает громко, а портит соседей. Соединение в
 * каждый момент одно. */
static uint8_t tms_buf[XVC_MAX], tdi_buf[XVC_MAX], tdo_buf[XVC_MAX];

static int rd_all(int fd, void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n) {
        ssize_t r = recv(fd, b, n, 0);
        if (r <= 0) return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static int wr_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w <= 0) return -1;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8
         | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

int xvc_session(int fd, const jtag_io *io, long *shifts)
{
    char cmd[8];
    long n_shift = 0;
    int rc = -1;

    for (;;) {
        /* Первые два байта различают все три команды, но имя дочитываем
         * целиком: иначе его хвост уехал бы в данные. */
        if (rd_all(fd, cmd, 2) < 0) { rc = 0; break; }      /* клиент ушёл */

        if (!memcmp(cmd, "ge", 2)) {
            char info[32];
            int k;
            if (rd_all(fd, cmd, 6) < 0 || memcmp(cmd, "tinfo:", 6)) break;
            k = snprintf(info, sizeof info, "xvcServer_v1.0:%d\n", XVC_MAX);
            if (wr_all(fd, info, (size_t)k) < 0) break;
        }
        else if (!memcmp(cmd, "se", 2)) {
            uint8_t per[4];
            if (rd_all(fd, cmd, 5) < 0 || memcmp(cmd, "ttck:", 5)) break;
            if (rd_all(fd, per, 4) < 0) break;
            /* Частоту задаёт jtag_speed(), а не клиент: проводам JTAG
             * быстрее не надо. Ответ тем же числом по протоколу значит
             * «принято», и клиент не спорит. */
            if (wr_all(fd, per, 4) < 0) break;
        }
        else if (!memcmp(cmd, "sh", 2)) {
            uint8_t nb[4];
            uint32_t bits, bytes, i;
            if (rd_all(fd, cmd, 4) < 0 || memcmp(cmd, "ift:", 4)) break;
            if (rd_all(fd, nb, 4) < 0) break;
            bits = le32(nb);
            bytes = (bits + 7) / 8;
            /* Больше, чем объявили в getinfo, честный клиент не пришлёт.
             * Пришёл — поток разъехался, и дальше всё было бы мусором. */
            if (bytes > XVC_MAX) break;
            if (rd_all(fd, tms_buf, bytes) < 0) break;
            if (rd_all(fd, tdi_buf, bytes) < 0) break;
            memset(tdo_buf, 0, bytes);
            for (i = 0; i < bits; i++) {
                uint32_t B = i >> 3, m = 1u << (i & 7);
                int tdo = io->pulse(io->ctx, (tms_buf[B] & m) != 0,
                                    (tdi_buf[B] & m) != 0);
                if (tdo) tdo_buf[B] |= (uint8_t)m;
            }
            if (wr_all(fd, tdo_buf, bytes) < 0) break;
            n_shift++;
        }
        else break;                         /* неизвестная команда */
    }
    if (shifts) *shifts = n_shift;
    return rc;
}
