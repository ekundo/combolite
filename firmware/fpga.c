/* Образ частями в память и прошивка по SSPI, см. fpga.h. */

#include "fpga.h"
#include "board.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

#define BLK       4096
#define MAX_ROWS  0x3FFFu                       /* 14 разрядов адреса страницы */
#define MAX_BYTES (FPGA_HDR + MAX_ROWS * SSPI_ROW)
#define NBLK      ((MAX_BYTES + BLK - 1) / BLK)
#define IDLE_S    300u

static uint8_t *blk[NBLK];
static uint32_t total, have, crc_want, now_cache, last_s;
static sspi_image img;

static void drop(void)
{
    size_t i;
    for (i = 0; i < NBLK; i++) { free(blk[i]); blk[i] = 0; }
    total = have = 0;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Байты образа с позиции pos: страница может лечь на стык блоков -- заголовок
 * в 40 байт сдвигает их с границы. */
static void read_at(uint32_t pos, uint8_t *out, size_t n)
{
    while (n) {
        size_t k = BLK - pos % BLK;
        if (k > n) k = n;
        memcpy(out, blk[pos / BLK] + pos % BLK, k);
        pos += (uint32_t)k; out += k; n -= k;
    }
}

static int get_row(void *ctx, uint32_t i, uint8_t out[SSPI_ROW])
{
    uint32_t pos = FPGA_HDR + i * SSPI_ROW;
    (void)ctx;
    if (i >= img.rows || pos + SSPI_ROW > have) return -1;
    read_at(pos, out, SSPI_ROW);
    return 0;
}

static uint32_t crc32_upd(uint32_t c, const uint8_t *p, size_t n)
{
    int k;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

int fpga_part(uint32_t off, const uint8_t *data, size_t n, const char **msg)
{
    size_t done = 0;

    if (off == 0) {
        uint32_t rows;
        drop();
        if (n < FPGA_HDR || memcmp(data, "CJED", 4) || data[4] != 1 || data[6] != FPGA_HDR) {
            *msg = "not a CJED image: combo.py fpga builds it from the .jed";
            return -1;
        }
        rows = le32(data + 28);
        if (!rows || rows > MAX_ROWS) { *msg = "page count out of range"; return -1; }
        memset(&img, 0, sizeof img);
        img.idcode   = le32(data + 8);
        img.usercode = le32(data + 12);
        memcpy(img.feature, data + 16, 8);
        memcpy(img.feabits, data + 24, 2);
        img.rows = rows;
        img.row  = get_row;
        crc_want = le32(data + 32);
        total    = FPGA_HDR + rows * SSPI_ROW;
    }
    else if (!total || off != have) {
        *msg = "parts must come in order, starting at offset 0";
        return -1;
    }
    if ((uint64_t)have + n > total) {
        drop();
        *msg = "more data than the header declares";
        return -1;
    }
    while (done < n) {
        uint32_t pos = have + (uint32_t)done;
        size_t k = BLK - pos % BLK;
        if (k > n - done) k = n - done;
        if (!blk[pos / BLK] && !(blk[pos / BLK] = malloc(BLK))) {
            drop();
            *msg = "out of memory for the image";
            return -2;
        }
        memcpy(blk[pos / BLK] + pos % BLK, data + done, k);
        done += k;
    }
    have += (uint32_t)n;
    last_s = now_cache;
    return 0;
}

void fpga_progress(uint32_t *h, uint32_t *t) { *h = have; *t = total; }

void fpga_drop(void) { drop(); }

/* ------------------------------------------------- что прошито последним */

#ifdef ESP_PLATFORM

#define NS  "fpga"
#define KEY "sha"

static int sha_load(uint8_t sha[32])
{
    nvs_handle_t h;
    size_t n = 32;
    int rc = -1;

    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    if (nvs_get_blob(h, KEY, sha, &n) == ESP_OK && n == 32) rc = 0;
    nvs_close(h);
    return rc;
}

static void sha_save(const uint8_t sha[32])
{
    nvs_handle_t h;

    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, KEY, sha, 32) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

#else

/* Без ESP-IDF -- в памяти, как настройки платы (board.c). */
static uint8_t saved_sha[32];
static int     have_sha;

static int sha_load(uint8_t sha[32])
{
    if (!have_sha) return -1;
    memcpy(sha, saved_sha, 32);
    return 0;
}

static void sha_save(const uint8_t sha[32])
{
    memcpy(saved_sha, sha, 32);
    have_sha = 1;
}

#endif

int fpga_flashed(uint8_t sha[32]) { return sha_load(sha); }

int fpga_can_flash(void)
{
    const sspi_io *io = fpga_io_claim();
    if (io) fpga_io_release();
    return io != 0;
}

/* sha256 всего образа, как он пришёл: тот же, что указан в манифесте. */
static void image_sha(uint8_t out[32])
{
    sha256_t c;
    uint8_t buf[256];
    uint32_t pos;

    combo_sha256_init(&c);
    for (pos = 0; pos < total; pos += sizeof buf) {
        size_t k = total - pos < sizeof buf ? total - pos : sizeof buf;
        read_at(pos, buf, k);
        combo_sha256_update(&c, buf, k);
    }
    combo_sha256_final(&c, out);
}

int fpga_flash(int flags, sspi_report *rep, const char **msg)
{
    const sspi_io *io;
    uint8_t buf[256];
    uint32_t pos, crc = 0;
    int rc;

    if (!total || have != total) { *msg = "the image is not complete"; return SSPI_E_IMAGE; }
    for (pos = FPGA_HDR; pos < total; pos += sizeof buf) {
        size_t k = total - pos < sizeof buf ? total - pos : sizeof buf;
        read_at(pos, buf, k);
        crc = crc32_upd(crc, buf, k);
    }
    if (crc != crc_want) { drop(); *msg = "image CRC mismatch, upload it again"; return SSPI_E_IMAGE; }

    if (!(io = fpga_io_claim())) { *msg = sspi_strerror(SSPI_E_NOHW); return SSPI_E_NOHW; }
    rc = sspi_program(io, &img, flags, rep);
    fpga_io_release();
    *msg = sspi_strerror(rc);
    /* Удачно -- образ больше не нужен, а ПЛИС после перезагрузки опять с
     * умолчательными настройками. Отказ -- образ остаётся для повтора. */
    if (rc == SSPI_OK) {
        uint8_t sha[32];
        image_sha(sha);
        sha_save(sha);
        drop();
        if (flags & SSPI_REFRESH) board_reapply_later();
    }
    return rc;
}

int fpga_json(char *out, int max)
{
    const sspi_io *io = fpga_io_claim();
    uint32_t id = 0, st = 0;
    uint8_t sha[32];
    char hex[65] = "";
    int n, i;

    if (io) { id = sspi_idcode(io); st = sspi_status(io); fpga_io_release(); }
    if (sha_load(sha) == 0)
        for (i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    n = snprintf(out, (size_t)max,
                 "{\"sspi\":%s,\"idcode\":\"0x%08x\",\"status\":\"0x%08x\",\"done\":%s,"
                 "\"upload\":{\"have\":%u,\"total\":%u},\"flashed\":\"%s\"",
                 io ? "true" : "false", (unsigned)id, (unsigned)st,
                 (st & (1u << 8)) ? "true" : "false", (unsigned)have, (unsigned)total, hex);
    if (n < 0 || n >= max) return n;
    n += fpga_io_extra(out + n, max - n);
    if (n < max - 2) { out[n++] = '}'; out[n++] = '\n'; out[n] = '\0'; }
    return n;
}

void fpga_tick(uint32_t now_s)
{
    now_cache = now_s;
    if (total && (uint32_t)(now_s - last_s) > IDLE_S) drop();
}
