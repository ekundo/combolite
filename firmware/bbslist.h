/* Список BBS для модема: Telnet BBS Guide (www.telnetbbsguide.com).
 *
 * ЗАЧЕМ ТАК. Списки BBS быстро устаревают, а вшивать в прошивку чужой список
 * условия Telnet BBS Guide не велят (info.txt внутри архива: в составе
 * программы -- только с разрешения). Поэтому ESP скачивает месячный выпуск
 * сама, для владельца платы, и держит его в памяти, пока работает; через
 * BBS_KEEP_MS скачает заново.
 *
 * ОТКУДА. Страница /lists/download-list/ ссылается на месячный архив
 * bbslist/ibbsMMYY.zip (дневной ibbsMMDDYY.zip -- только для себя, его не
 * берём). Из архива нужен один bbslist.csv: он четвёртый, сжатый около 34 КБ
 * на полтора мегабайта архива, поэтому архив читается по порядку, и соединение
 * рвётся, как только файл получен. Всё -- простым HTTP, без TLS: так один и тот
 * же код работает на плате, в эмуляторе и в стендах.
 *
 * ХРАНЕНИЕ. В памяти -- только сжатый CSV; поиск распаковывает его на лету
 * (inflate.c, окно 32 КБ берётся на время поиска). Номера -- по порядку BBS с
 * адресом telnet в выпуске: ATDS n набирает n-ю. */
#ifndef COMBO_BBSLIST_H
#define COMBO_BBSLIST_H

#include <stdint.h>

#define BBS_KEEP_MS (24u * 3600u * 1000u)

typedef struct {
    int         num;
    const char *name, *host, *software, *location;
    int         port;
} bbs_t;

/* Скачать, если нет или старый. Возвращает число BBS или -1 (ошибка -- в
 * bbs_error). */
int  bbs_fetch(uint32_t now_ms);
const char *bbs_error(void);
const char *bbs_issue(void);            /* "ibbs1026" -- выпуск, октябрь 2026 */

/* BBS, где слово (без учёта регистра) есть в имени, адресе, программе или
 * месте: по each() на первые max, возврат -- сколько всего. */
int  bbs_search(const char *word, int max, void (*each)(const bbs_t *b, void *arg), void *arg);
/* n-я BBS: 1 -- есть. */
int  bbs_get(int num, char *host, int hostlen, int *port);

/* Для стендов: откуда качать (по умолчанию www.telnetbbsguide.com:80). */
void bbs_source(const char *host, int port);

#endif
