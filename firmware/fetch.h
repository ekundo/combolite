/* Скачать файл по HTTPS -- для моста «Gopher поверх HTTPS» в модеме (modem.c).
 *
 * GitHub Pages отдаёт только HTTPS, а GOPHER.COM ходит сырым TCP на порт 70.
 * Модем сам снимает шифрование: принимает строку выбора от «Вектора»,
 * скачивает файл и отдаёт его байты как ответ Gopher. Сам модем остаётся
 * переносимым, а скачивание -- за этим API:
 *   на плате  -- fetch_esp.c: esp_http_client в своей задаче, сертификат
 *                проверяется набором корневых ESP-IDF, байты -- через
 *                StreamBuffer;
 *   на маке   -- fetch_curl.c: curl в трубе (проверочные стенды, v06x,
 *                Emu80). Переменная COMBO_FETCH_BASE подменяет «https://хост»
 *                -- стенд проверяет мост без сети.
 *
 * Одно скачивание за раз. Чтение не блокирует. */
#ifndef COMBO_FETCH_H
#define COMBO_FETCH_H

#include <stdint.h>

#define FETCH_MAX  (1024 * 1024)        /* больше не отдаём: на «Векторе» столько и не уместить */

/* Начать скачивание. 0 -- пошло, иначе -- не вышло (уже идёт другое, нет памяти). */
int  fetch_open(const char *url);

/* Байты скачанного в buf, не больше max. Возвращает >0 -- сколько, 0 --
 * пока нечего, FETCH_DONE -- файл кончился, FETCH_FAIL -- не вышло
 * (fetch_status: код HTTP, 0 -- сеть). */
#define FETCH_DONE  (-1)
#define FETCH_FAIL  (-2)
int  fetch_read(uint8_t *buf, int max);
int  fetch_status(void);

/* Закончить (и прервать, если ещё идёт). */
void fetch_close(void);

#endif
