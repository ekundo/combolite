/* Транспорт до ПЛИС: единственное место, где верхние слои (combo.c, ata.c,
 * net.c) касаются железа. Реализация — link_espidf.c, SPI через ESP-IDF.
 */
#ifndef COMBO_LINK_H
#define COMBO_LINK_H

#include <stddef.h>
#include <stdint.h>

int     link_open(void);              /* 0 — успех, иначе отрицательный код */
void    link_close(void);

void    link_select(void);            /* CS активен */
void    link_deselect(void);
uint8_t link_xfer(uint8_t out);       /* отдать байт, получить байт */

/* Пачкой, полным дуплексом. out == 0 — слать нули, in == 0 — ответ выбросить.
 * Ради этого и заведено: на ESP-IDF каждая транзакция SPI стоит около
 * микросекунды накладных, и 512 отдельных транзакций на сектор съели бы
 * полмиллисекунды впустую. */
void    link_xfer_block(const uint8_t *out, uint8_t *in, size_t n);
void    link_delay_us(uint32_t us);

/* Монотонные миллисекунды от какого-то момента в прошлом. Нужны там, где
 * ждать нельзя, а срок соблюсти надо: фоновые дела меряют время сами. */
uint32_t link_ms(void);

#endif
