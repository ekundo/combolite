/* Распаковка deflate (RFC 1951) с выдачей по байту.
 *
 * Своя, а не zlib или miniz из ПЗУ ESP: один и тот же код работает на плате,
 * в эмуляторе (v06x, ветка combolite) и в стендах на маке. Нужна списку BBS
 * (bbslist.c): из архива Telnet BBS Guide берётся один файл, и разбирать его
 * удобнее потоком, не держа распакованное целиком.
 *
 * Окно -- 32 КБ, как требует формат; его даёт вызывающий. Выход -- по байту в
 * put(); put вернул не 0 -- распаковка останавливается (нашли, что искали). */
#ifndef COMBO_INFLATE_H
#define COMBO_INFLATE_H

#include <stdint.h>
#include <stddef.h>

#define INFLATE_WINDOW 32768

/* 0 -- поток кончился как надо или put попросил остановиться, -1 -- поток
 * испорчен или кончился раньше времени. */
int inflate_raw(const uint8_t *in, size_t n, uint8_t *window,
                int (*put)(uint8_t b, void *arg), void *arg);

#endif
