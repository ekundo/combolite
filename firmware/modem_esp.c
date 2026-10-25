/* Wi-Fi модем на плате: своя задача (modem.h).
 *
 * Своя задача, а не опрос канала: набор номера разрешает имя хоста
 * блокирующим getaddrinfo, и опрос канала на это время замер бы, а с ним и
 * признак «ESP жива» у «Вектора». Трубы модема -- кольца с одним писателем и
 * одним читателем, так что замка между задачами не нужно. */

#include "modem.h"
#include "link.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void modem_task(void *arg)
{
    (void)arg;
    for (;;) {
        modem_poll(link_ms());
        vTaskDelay(1);
    }
}

void modem_start(void)
{
    modem_init();
    xTaskCreate(modem_task, "modem", 8192, 0, 2, 0);   /* скачивание списка BBS держит буферы на стеке */
}
