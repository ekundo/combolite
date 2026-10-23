/* Журнал платы в памяти: см. eslog.c. */
#ifndef COMBO_ESLOG_H
#define COMBO_ESLOG_H

#include <stddef.h>

/* Начать копить журнал. Звать как можно раньше: до этого вызова строки уходят
 * только в UART, и первые из них — про подключение к сети — самые нужные. */
void eslog_start(void);

/* Последние строки журнала. Возвращает длину записанного или -1. */
int eslog_text(char *out, size_t max);

#endif
