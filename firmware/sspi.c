/* Прошивка флеша ПЛИС по SSPI, см. sspi.h. Команды -- FPGA-TN-02155, табл. 9.3. */

#include "sspi.h"

#include <string.h>

/* Регистр состояния, табл. 9.4. */
#define ST_PWD_PROT (1u << 5)
#define ST_OTP      (1u << 6)
#define ST_DONE     (1u << 8)
#define ST_WREN     (1u << 10)
#define ST_BUSY     (1u << 12)
#define ST_FAIL     (1u << 13)

#define ERASE_MS    60000u   /* стирание флеша 7000 -- секунды; ждём с запасом */
#define PAGE_MS     20u      /* страница -- около 200 мкс */
#define REFRESH_MS  5000u

static void cmd4(const sspi_io *io, uint8_t op, uint8_t a, uint8_t b, uint8_t c)
{
    const uint8_t o[4] = { op, a, b, c };
    io->txn(io->ctx, o, 4, 0, 0);
}

uint32_t sspi_idcode(const sspi_io *io)
{
    static const uint8_t o[4] = { 0xE0, 0, 0, 0 };
    uint8_t in[4];
    io->txn(io->ctx, o, 4, in, 4);
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
}

uint32_t sspi_status(const sspi_io *io)
{
    static const uint8_t o[4] = { 0x3C, 0, 0, 0 };
    uint8_t in[4];
    io->txn(io->ctx, o, 4, in, 4);
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
}

/* Флаг занятости (0xF0, разряд 7). Ждём по нему, а не выдержками: для
 * стирания TN прямо запрещает фиксированные задержки. */
static int wait_ready(const sspi_io *io, uint32_t timeout_ms, uint32_t poll_us)
{
    static const uint8_t o[4] = { 0xF0, 0, 0, 0 };
    uint32_t t0 = io->ms(io->ctx);
    uint8_t b;

    for (;;) {
        io->txn(io->ctx, o, 4, &b, 1);
        if (!(b & 0x80)) return 0;
        if ((uint32_t)(io->ms(io->ctx) - t0) > timeout_ms) return -1;
        io->delay_us(io->ctx, poll_us);
    }
}

/* Выйти из режима прошивки. В прозрачном режиме ПЛИС от этого не
 * перезагружается и остаётся на прежнем образе. */
static void disable(const sspi_io *io)
{
    static const uint8_t o[3] = { 0x26, 0, 0 };
    io->txn(io->ctx, o, 3, 0, 0);
    io->delay_us(io->ctx, 10);
}

static int fail(const sspi_io *io, sspi_report *rep, int rc, uint32_t row)
{
    rep->status = sspi_status(io);
    rep->row = row;
    disable(io);
    return rc;
}

int sspi_program(const sspi_io *io, const sspi_image *img, int flags, sspi_report *rep)
{
    sspi_report dummy;
    uint8_t page[4 + SSPI_ROW], back[SSPI_ROW];
    uint32_t i, st, t;

    if (!rep) rep = &dummy;
    memset(rep, 0, sizeof *rep);
    if (!img->rows || img->rows > 0x3FFF) return SSPI_E_IMAGE;

    /* 1. Тот ли кристалл. Это же проверка проводов: молчащий SSPI даёт нули
     *    или единицы, а не код Lattice. */
    rep->idcode = sspi_idcode(io);
    if (rep->idcode != img->idcode) return SSPI_E_ID;

    /* 2. Прозрачный режим, флеш (разряд 19 операнда). */
    cmd4(io, 0x74, 0x08, 0x00, 0x00);
    io->delay_us(io->ctx, 5);
    if (wait_ready(io, 1000, 10) != 0) return fail(io, rep, SSPI_E_TIMEOUT, 0);
    st = sspi_status(io);
    if (st & ST_FAIL) return fail(io, rep, SSPI_E_ENABLE, 0);
    if ((st & (ST_PWD_PROT | ST_OTP)) || !(st & ST_WREN))
        return fail(io, rep, SSPI_E_LOCKED, 0);

    /* 3. Строка настроек кристалла против строки в файле. Писать её этот путь
     *    не умеет (sspi.h), поэтому другая строка значит, что новый образ
     *    рассчитан на другие порты и выводы конфигурации. */
    {
        static const uint8_t fr[4] = { 0xE7, 0, 0, 0 }, fb[4] = { 0xFB, 0, 0, 0 };
        io->txn(io->ctx, fr, 4, rep->feature, 8);
        io->txn(io->ctx, fb, 4, rep->feabits, 2);
        if (!(flags & SSPI_FORCE) &&
            (memcmp(rep->feature, img->feature, 8) || memcmp(rep->feabits, img->feabits, 2)))
            return fail(io, rep, SSPI_E_FEATURE, 0);
    }

    /* 4. Страницы образа берём заранее: отказ отдать страницу должен
     *    случиться до стирания, а не посреди записи. */
    for (i = 0; i < img->rows; i++)
        if (img->row(img->ctx, i, back) != 0) return fail(io, rep, SSPI_E_IMAGE, i);

    /* 5. Стереть флеш конфигурации (разряд 18). UFM и строку настроек не
     *    трогаем. */
    t = io->ms(io->ctx);
    cmd4(io, 0x0E, 0x04, 0x00, 0x00);
    if (wait_ready(io, ERASE_MS, 10000) != 0) return fail(io, rep, SSPI_E_TIMEOUT, 0);
    if (sspi_status(io) & ST_FAIL) return fail(io, rep, SSPI_E_ERASE, 0);
    rep->erase_ms = io->ms(io->ctx) - t;

    /* 6. Страницы подряд от начала: адрес шагает сам. */
    t = io->ms(io->ctx);
    cmd4(io, 0x46, 0, 0, 0);
    page[0] = 0x70; page[1] = 0; page[2] = 0; page[3] = 1;
    for (i = 0; i < img->rows; i++) {
        img->row(img->ctx, i, page + 4);
        io->txn(io->ctx, page, sizeof page, 0, 0);
        if (wait_ready(io, PAGE_MS, 50) != 0) return fail(io, rep, SSPI_E_TIMEOUT, i);
        if ((i & 255) == 255 && (sspi_status(io) & ST_FAIL))
            return fail(io, rep, SSPI_E_PROG, i);
    }
    if (sspi_status(io) & ST_FAIL) return fail(io, rep, SSPI_E_PROG, img->rows - 1);

    if (img->usercode) {
        const uint8_t u[8] = { 0xC2, 0, 0, 0, (uint8_t)(img->usercode >> 24),
                               (uint8_t)(img->usercode >> 16), (uint8_t)(img->usercode >> 8),
                               (uint8_t)img->usercode };
        io->txn(io->ctx, u, 8, 0, 0);
        if (wait_ready(io, PAGE_MS, 50) != 0 || (sspi_status(io) & ST_FAIL))
            return fail(io, rep, SSPI_E_PROG, 0);
    }
    rep->prog_ms = io->ms(io->ctx) - t;

    /* 7. Сверка постранично: адрес задаём явно (0xB4), читаем одну страницу
     *    (0x73, счёт 1). Многостраничное чтение у MachXO2 повторяет первую
     *    страницу и вставляет пустые байты (TN, рис. 9.8) -- не связываемся. */
    if (!(flags & SSPI_NOVERIFY)) {
        t = io->ms(io->ctx);
        for (i = 0; i < img->rows; i++) {
            uint8_t a[8] = { 0xB4, 0, 0, 0, 0x00, 0x00, (uint8_t)((i >> 8) & 0x3F), (uint8_t)i };
            static const uint8_t rd[4] = { 0x73, 0x00, 0x00, 0x01 };
            io->txn(io->ctx, a, 8, 0, 0);
            io->txn(io->ctx, rd, 4, back, SSPI_ROW);
            img->row(img->ctx, i, page + 4);
            if (memcmp(back, page + 4, SSPI_ROW)) return fail(io, rep, SSPI_E_VERIFY, i);
        }
        rep->verify_ms = io->ms(io->ctx) - t;
    }

    /* 8. DONE: без него кристалл образ из флеша не возьмёт. */
    cmd4(io, 0x5E, 0, 0, 0);
    if (wait_ready(io, 1000, 50) != 0) return fail(io, rep, SSPI_E_TIMEOUT, 0);
    st = sspi_status(io);
    if ((st & ST_FAIL) || !(st & ST_DONE)) return fail(io, rep, SSPI_E_DONE, 0);

    disable(io);

    /* 9. Перезагрузка из флеша -- как PROGRAMN. Машина при этом тоже
     *    перезагрузится: новая ПЛИС с включения держит ВВОД (autostart.v). */
    if (flags & SSPI_REFRESH) {
        static const uint8_t r[3] = { 0x79, 0, 0 };
        io->txn(io->ctx, r, 3, 0, 0);
        t = io->ms(io->ctx);
        for (;;) {
            io->delay_us(io->ctx, 10000);
            st = sspi_status(io);
            if (!(st & ST_BUSY) && (st & ST_DONE)) break;
            if ((uint32_t)(io->ms(io->ctx) - t) > REFRESH_MS) {
                rep->status = st;
                return SSPI_E_DONE;
            }
        }
    }
    rep->status = sspi_status(io);
    return SSPI_OK;
}

const char *sspi_strerror(int rc)
{
    switch (rc) {
    case SSPI_OK:        return "ok";
    case SSPI_E_ID:      return "wrong FPGA or no answer on SSPI";
    case SSPI_E_FEATURE: return "the file's feature row differs from the FPGA's: program it with Diamond over JTAG";
    case SSPI_E_ENABLE:  return "the FPGA refused programming mode";
    case SSPI_E_LOCKED:  return "flash is locked (security, password or OTP)";
    case SSPI_E_ERASE:   return "flash erase failed";
    case SSPI_E_PROG:    return "page programming failed";
    case SSPI_E_VERIFY:  return "verify mismatch";
    case SSPI_E_DONE:    return "the FPGA did not configure from the new image";
    case SSPI_E_TIMEOUT: return "the FPGA stayed busy";
    case SSPI_E_IMAGE:   return "bad image";
    case SSPI_E_NOHW:    return "no SSPI on this build";
    default:             return "unknown error";
    }
}
