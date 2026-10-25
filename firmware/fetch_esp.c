/* Скачивание для моста Gopher на плате (fetch.h): esp_http_client в своей
 * задаче, байты -- через StreamBuffer к задаче модема.
 *
 * Своя задача, а не вызовы из modem_poll: открытие связи (DNS, TLS) идёт
 * секундами и блокирует, а модем в это время должен отвечать «Вектору» и
 * слушать +++. Сертификат проверяется набором корневых ESP-IDF, как у
 * проверки обновлений (update.c, там же -- почему без проверки нельзя).
 * За перенаправлениями идём сами: в потоковом режиме esp_http_client их не
 * доводит (update.c, open_following). */

#include "fetch.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "fetch";

#define SB_SIZE 4096

static StreamBufferHandle_t sb;
static char           url_buf[512];
static volatile int   state;            /* 0 -- нет, 1 -- идёт, 2 -- кончилось, 3 -- не вышло */
static volatile int   stop;             /* прервать: задача выходит при первой возможности */
static volatile int   running;          /* задача ещё жива */
static volatile int   status;

static int open_following(esp_http_client_handle_t c)
{
    int hop;

    for (hop = 0; hop < 5; hop++) {
        int code;

        if (esp_http_client_open(c, 0) != ESP_OK) return 0;
        esp_http_client_fetch_headers(c);
        code = esp_http_client_get_status_code(c);
        if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) return code;
        esp_http_client_close(c);
        if (esp_http_client_set_redirection(c) != ESP_OK) return 0;
    }
    return 0;
}

static void fetch_task(void *arg)
{
    esp_http_client_config_t cfg = { 0 };
    esp_http_client_handle_t c;
    static uint8_t buf[512];
    long total = 0;
    int code, n;

    (void)arg;
    cfg.url = url_buf;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;             /* перенаправления GitHub длинные (update.c) */
    cfg.buffer_size_tx = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 5;
    c = esp_http_client_init(&cfg);
    code = c ? open_following(c) : 0;
    ESP_LOGI(TAG, "%s: %d", url_buf, code);
    if (code != 200) {
        status = code;
        state = 3;
    } else {
        for (;;) {
            if (stop) break;
            n = esp_http_client_read(c, (char *)buf, sizeof buf);
            if (n < 0) { status = 0; state = 3; break; }
            if (n == 0) { state = 2; break; }
            total += n;
            if (total > FETCH_MAX) { state = 2; break; }   /* дальше «Вектору» всё равно не уместить */
            {
                int off = 0;
                while (off < n && !stop)    /* модем забирает -- место освободится */
                    off += (int)xStreamBufferSend(sb, buf + off, (size_t)(n - off), pdMS_TO_TICKS(100));
            }
        }
    }
    if (c) esp_http_client_cleanup(c);
    running = 0;
    vTaskDelete(0);
}

int fetch_open(const char *url)
{
    if (running) return -1;
    if (!sb) sb = xStreamBufferCreate(SB_SIZE, 1);
    if (!sb) return -1;
    if (strlen(url) >= sizeof url_buf) return -1;
    xStreamBufferReset(sb);
    strcpy(url_buf, url);
    stop = 0;
    status = 0;
    state = 1;
    running = 1;
    /* TLS и разбор ответа берут стек: 8 КБ, как у задачи модема со списком BBS */
    if (xTaskCreate(fetch_task, "fetch", 8192, 0, 2, 0) != pdPASS) {
        running = 0;
        state = 0;
        return -1;
    }
    return 0;
}

int fetch_read(uint8_t *buf, int max)
{
    size_t n;

    if (!sb) return FETCH_FAIL;
    n = xStreamBufferReceive(sb, buf, (size_t)max, 0);
    if (n > 0) return (int)n;
    if (state == 2 && !running) return FETCH_DONE;
    if (state == 3 && !running) return FETCH_FAIL;
    return 0;
}

int fetch_status(void) { return status; }

/* Прервать и дождаться задачи: она выходит, как только отдаст кусок или
 * дождётся ответа сервера (таймаут -- 15 с). Пока ждём, буфер освобождаем,
 * чтобы задача не висела на отправке. */
void fetch_close(void)
{
    stop = 1;
    while (running) {
        if (sb) xStreamBufferReset(sb);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    state = 0;
}
