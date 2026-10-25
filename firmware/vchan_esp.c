/* Опрос канала «Вектор» <-> ESP по таймеру (протокол -- vchan.h).
 *
 * ПЛИС обмен начать не может, поэтому опрашиваем сами. По этому же опросу
 * ПЛИС знает, что ESP жива (не дольше 0,34 с назад), иначе LSR у «Вектора» --
 * 00h, так что в тишине опрос -- раз в тик FreeRTOS (10 мс), реже нельзя.
 *
 * СКОРОСТЬ. За опрос к «Вектору» уходит не больше CHAN_DEPTH = 14 байт: при
 * опросе раз в тик это 1,4 КБ/с, и полный экран TERM.COM (около 2,2 КБ
 * разницы) шёл на 1,6 с дольше, чем при мгновенной ESP (замер в v06x,
 * --combolite-poll-us). Поэтому, пока байты идут и ещё FAST_HOLD_MS после,
 * задачу будит esp_timer раз в FAST_US: тик для этого слишком крупный, а
 * поднимать его ради одной задачи -- менять время всем остальным. Опрос --
 * одна команда SPI на 4 МГц, около 60 мкс.
 *
 * Канала нет (старый битстрим, ПЛИС перезаливается) -- пробуем раз в две
 * секунды: перезалитая ПЛИС с каналом подхватывается сама. */

#include "vchan.h"
#include "link.h"
#include "combo.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "vchan";

#define FAST_US      500
#define FAST_HOLD_MS 300

static TaskHandle_t task;

static void wake(void *arg)
{
    (void)arg;
    xTaskNotifyGive(task);
}

static void vchan_task(void *arg)
{
    const esp_timer_create_args_t ta = { .callback = wake, .name = "vchan" };
    esp_timer_handle_t timer = 0;
    int had = -1, on, r, fast = 0;
    uint32_t now, busy = 0, log_at = 0;
    combo_chan_stat_t last = { 0 };

    (void)arg;
    task = xTaskGetCurrentTaskHandle();
    if (esp_timer_create(&ta, &timer) != ESP_OK) timer = 0;
    for (;;) {
        now = link_ms();
        r = vchan_poll(now);
        on = r > 0;
        if (on != had) {
            ESP_LOGI(TAG, "%s", on ? "channel to the Vector is up (UART 16550 at 28h)"
                                   : "no channel to the Vector in this bitstream");
            had = on;
        }
        if (r == 2) busy = now;
        if ((uint32_t)(now - log_at) >= 2000) {    /* журнал канала: только когда что-то сдвинулось */
            combo_chan_stat_t c = combo_chan_stat;
            if (!last.polls || c.in != last.in || c.out != last.out || c.bad != last.bad
                || c.flushes != last.flushes || c.first != last.first)
                ESP_LOGI(TAG, "polls %lu bad %lu in %lu out %lu flushes %lu state %02x",
                         (unsigned long)c.polls, (unsigned long)c.bad, (unsigned long)c.in,
                         (unsigned long)c.out, (unsigned long)c.flushes, c.first);
            last = c;
            log_at = now;
        }
        if (timer && on && (uint32_t)(now - busy) < FAST_HOLD_MS) {
            if (!fast) { esp_timer_start_periodic(timer, FAST_US); fast = 1; }
            ulTaskNotifyTake(pdTRUE, 1);
            continue;
        }
        if (fast) { esp_timer_stop(timer); fast = 0; ulTaskNotifyTake(pdTRUE, 0); }
        vTaskDelay(on ? 1 : pdMS_TO_TICKS(2000));
    }
}

void vchan_start(void)
{
    xTaskCreate(vchan_task, "vchan", 3072, 0, 2, 0);
}
