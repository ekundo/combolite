/* Журнал платы, доступный без кабеля.
 *
 * ЗАЧЕМ. Всё, что плата рассказывает о себе, — почему не подключилась, какую
 * точку выбрала, отчего не ожил привод, — уходило в UART. Чтобы это прочесть,
 * нужен провод и монитор, то есть ровно то, ради избавления от чего плату и
 * ставят. Морда при этом показывает «привод: dead» и молчит о причине.
 *
 * КАК. esp_log_set_vprintf подменяет вывод журнала: строка идёт и в UART, как
 * прежде, и в кольцо в памяти. Кольцо отдаётся по /api/wifi-соседу /api/log.
 *
 * ЧЕГО ЗДЕСЬ НЕТ. printf мимо журнала (приветствия при старте, ответы консоли)
 * не перехватывается: подменяется именно вывод ESP_LOGx, а не поток stdout.
 * Ловить stdout целиком пришлось бы через подмену файлового потока, и ради
 * трёх строк приветствия это не стоит риска — важное идёт через журнал.
 *
 * СЕКРЕТ В ЖУРНАЛ НЕ ПОПАДАЕТ. При первом запуске он печатается один раз с
 * пометкой «запишите», и это единственное место, где его видно. Строки от
 * этого тега в кольцо не кладутся: журнал отдаётся по сети, и класть туда то,
 * что даёт доступ к самой плате, нельзя. В UART он по-прежнему выводится.
 */
#include "sdkconfig.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "eslog.h"

/* Восемь килобайт: примерно сотня строк журнала, чего хватает и на разбор
   подключения, и на историю разрывов. Больше — уже расточительно для памяти,
   которой на плате около двухсот килобайт свободной. */
#define RING 8192

static char ring[RING];
static size_t at;          /* куда писать следующий байт */
static int    wrapped;     /* кольцо уже обернулось хоть раз */
static vprintf_like_t next; /* прежний вывод: UART */

/* Тег, чьи строки в кольцо не попадают: там печатается секрет платы. */
static const char *HIDDEN = "secret:";

/* Цвет по дороге выбрасывается.
 *
 * ESP-IDF красит уровни журнала управляющими последовательностями вида
 * ESC[0;32m. В терминале это цвет, в морде — мусор посреди текста, а в кольце
 * ещё и место: по десятку байт на строку, то есть примерно каждый десятый
 * байт памяти под журнал. В UART цвет остаётся: туда строка уходит
 * нетронутой. */
static void put(const char *s, size_t n)
{
    size_t i;
    int esc = 0;

    for (i = 0; i < n; i++) {
        char c = s[i];

        if (esc) {                      /* последовательность идёт до буквы */
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) esc = 0;
            continue;
        }
        if (c == 0x1B) { esc = 1; continue; }

        ring[at] = c;
        if (++at == RING) { at = 0; wrapped = 1; }
    }
}

static int hook(const char *fmt, va_list ap)
{
    char line[256];
    va_list copy;
    int n;

    va_copy(copy, ap);
    n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);

    if (n > 0) {
        size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
        /* Тег стоит после «W (1234) », поэтому ищем по всей строке, а не с
           начала: формат журнала задаёт ESP-IDF, и подстраиваться под его
           разметку надёжнее по подстроке. */
        if (!strstr(line, HIDDEN)) put(line, len);
    }
    return next ? next(fmt, ap) : 0;
}

void eslog_start(void)
{
    next = esp_log_set_vprintf(hook);
}

int eslog_text(char *out, size_t max)
{
    size_t have = wrapped ? RING : at;
    size_t take = have < max - 1 ? have : max - 1;
    size_t start;

    if (!max) return -1;
    /* Отдаём ХВОСТ: когда места меньше, чем накопилось, интересны последние
       строки, а не первые. */
    start = (at + RING - take) % RING;
    if (start + take <= RING) {
        memcpy(out, ring + start, take);
    } else {
        size_t first = RING - start;
        memcpy(out, ring + start, first);
        memcpy(out + first, ring, take - first);
    }
    out[take] = 0;
    return (int)take;
}
