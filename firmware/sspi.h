/* Прошивка флеша ПЛИС через ведомый SPI конфигурации (SSPI).
 *
 * ЗАЧЕМ. JTAG ПЛИС выведен только на контрольные точки, к модулю ESP он не
 * подключён. С ПЛИС модуль связан двумя шинами: рабочим SPI (ide_remote.v) и
 * SSPI конфигурации (IO4..IO7, IO15). Им и прошиваем.
 *
 * ТОЛЬКО ПРОЗРАЧНЫЙ РЕЖИМ (0x74). В нём стираются и пишутся флеш конфигурации
 * и UFM, а строку настроек (Feature Row) кристалл не трогает вовсе, и ПЛИС всё
 * это время работает на прежнем образе. Строка настроек решает, какие порты
 * конфигурации живы, -- испорченная заперла бы и этот путь, и плата вернулась
 * бы к программатору на контрольных точках. Поэтому строки настроек здесь нет
 * в записи совсем: первую прошивку чистой ПЛИС делает Diamond по JTAG, а сюда
 * приходят обновления с той же строкой (sspi_program сверяет её и без
 * SSPI_FORCE откажется, если строка в файле другая).
 *
 * Порядок команд и сроки -- FPGA-TN-02155, раздел 9.3 и таблица 9.3. SSPI --
 * режим 0, старший разряд вперёд (раздел 6.4). Строка флеша -- 128 бит; бит 0
 * строки JEDEC уходит в кристалл первым, то есть старшим разрядом первого
 * байта. */
#ifndef COMBO_SSPI_H
#define COMBO_SSPI_H

#include <stddef.h>
#include <stdint.h>

/* Одна посылка при опущенном SN: задвинуть out_n байт, потом вычитать in_n,
 * задвигая нули. Ноги -- забота того, кто это реализует (sspi_esp.c). */
typedef struct {
    void (*txn)(void *ctx, const uint8_t *out, size_t out_n, uint8_t *in, size_t in_n);
    void (*delay_us)(void *ctx, uint32_t us);
    uint32_t (*ms)(void *ctx);          /* монотонные миллисекунды */
    void *ctx;
} sspi_io;

#define SSPI_ROW 16                      /* байт в странице флеша */

typedef struct {
    uint32_t idcode;                     /* какой кристалл ждёт файл */
    uint32_t usercode;
    uint8_t  feature[8], feabits[2];     /* строка настроек из файла, для сверки */
    uint32_t rows;                       /* страниц флеша конфигурации, с нулевой */
    /* Страница i -- 16 байт. 0 -- отдана, иначе данных нет. */
    int    (*row)(void *ctx, uint32_t i, uint8_t out[SSPI_ROW]);
    void    *ctx;
} sspi_image;

#define SSPI_REFRESH 1                   /* после прошивки загрузить новый образ */
#define SSPI_FORCE   2                   /* строка настроек в файле другая -- всё равно */
#define SSPI_NOVERIFY 4                  /* не сверять страницы (только для замеров) */

#define SSPI_OK          0
#define SSPI_E_ID       -1               /* не тот кристалл или SSPI молчит */
#define SSPI_E_FEATURE  -2               /* строка настроек в файле не та, что в кристалле */
#define SSPI_E_ENABLE   -3               /* не вошли в режим прошивки */
#define SSPI_E_LOCKED   -4               /* флеш защищён: безопасность, пароль, OTP */
#define SSPI_E_ERASE    -5
#define SSPI_E_PROG     -6
#define SSPI_E_VERIFY   -7
#define SSPI_E_DONE     -8
#define SSPI_E_TIMEOUT  -9
#define SSPI_E_IMAGE   -10               /* образ без страниц или страница не отдана */
#define SSPI_E_NOHW    -11               /* на этой сборке SSPI нет */

typedef struct {
    uint32_t idcode;                     /* что ответил кристалл */
    uint8_t  feature[8], feabits[2];     /* его строка настроек */
    uint32_t status;                     /* регистр состояния в момент отказа */
    uint32_t row;                        /* страница, на которой отказали */
    uint32_t erase_ms, prog_ms, verify_ms;
} sspi_report;

/* Прочитать код кристалла. Ничего не меняет. */
uint32_t sspi_idcode(const sspi_io *io);

/* Регистр состояния (0x3C), 32 бита. */
uint32_t sspi_status(const sspi_io *io);

/* Прошить флеш конфигурации образом. Всё, что можно проверить до стирания,
 * проверяется до стирания: код кристалла, строка настроек, защита. Отказ
 * после стирания оставляет ПЛИС на прежнем образе в SRAM -- до перезагрузки;
 * повтор прошивки это лечит. */
int sspi_program(const sspi_io *io, const sspi_image *img, int flags, sspi_report *rep);

const char *sspi_strerror(int rc);

#endif
