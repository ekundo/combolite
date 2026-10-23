/* Транспорт на плате: SPI через ESP-IDF.
 *
 * ВЫБОР СНЯТИЯ CS ВРУЧНУЮ. Драйвер ESP-IDF снимает CS после каждой транзакции,
 * а у нашего протокола посылка — это команда и следом данные при неснятом CS
 * (см. заголовок fpga/hdl/ide_remote.v). Держать CS флагом SPI_TRANS_CS_KEEP_ACTIVE
 * можно, но тогда о нём надо помнить в каждой транзакции и не забыть снять в
 * последней. Простой вывод GPIO точно ложится на link_select/link_deselect и
 * не оставляет такой возможности ошибиться.
 */

#include "link.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

#ifndef CONFIG_COMBO_SPI_HOST
#define CONFIG_COMBO_SPI_HOST 2
#endif
#ifndef CONFIG_COMBO_PIN_SCK
#define CONFIG_COMBO_PIN_SCK  18
#endif
#ifndef CONFIG_COMBO_PIN_MOSI
#define CONFIG_COMBO_PIN_MOSI 23
#endif
#ifndef CONFIG_COMBO_PIN_MISO
#define CONFIG_COMBO_PIN_MISO 19
#endif
#ifndef CONFIG_COMBO_PIN_CS
#define CONFIG_COMBO_PIN_CS    5
#endif
#ifndef CONFIG_COMBO_SPI_HZ
/* Предел задаёт ведомый в ПЛИС: MISO меняется через 45..65 нс после спада
 * SCK, отсюда ~7,5 МГц. На живой плате обмен рассыпается на 8 МГц, а на 7 и
 * ниже идёт без единой ошибки. Отсюда 4 — вдвое ниже отказа. Подробнее в
 * fpga/hdl/spi_slave.v; отказ выглядит как сдвиг данных на разряд, а не как
 * молчание. */
#define CONFIG_COMBO_SPI_HZ 4000000
#endif

#define BLOCK_MAX 512

static const char *TAG = "combo-link";
static spi_device_handle_t dev;
static uint8_t *dma_tx, *dma_rx;

/* ВЫБОРКА -- ЦЕЛИКОМ ОДНОЙ ЗАДАЧЕЙ. Шину водит цикл обслуживания (srv_run), но
 * отчёты мыши приходят из задачи USB (mouse_esp.c), и ждать с ними до витка
 * цикла -- до секунды -- нельзя. Замок берётся на опускании CS и отпускается
 * на подъёме: чужой байт посреди команды с аргументом ПЛИС приняла бы за
 * аргумент. Между выборками другая задача вклиниться может, и это безопасно:
 * пакет мыши не трогает ни буферов, ни ответа C_RES. */
static SemaphoreHandle_t bus_lock;

/* Ожидание в цикле опроса не должно морить сторожевой таймер задач. Копим
 * занятое ожидание и раз в несколько миллисекунд уступаем планировщику. */
static uint32_t busy_us;

int link_open(void)
{
    esp_err_t e;
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_COMBO_PIN_MOSI,
        .miso_io_num = CONFIG_COMBO_PIN_MISO,
        .sclk_io_num = CONFIG_COMBO_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BLOCK_MAX,
    };
    spi_device_interface_config_t cfg = {
        .clock_speed_hz = CONFIG_COMBO_SPI_HZ,
        .mode = 0,                      /* как spi_slave.v: такт покоится в нуле */
        .spics_io_num = -1,             /* CS ведём сами, см. заголовок */
        .queue_size = 1,
    };
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << CONFIG_COMBO_PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config(&cs);
    gpio_set_level(CONFIG_COMBO_PIN_CS, 1);         /* не выбран */
    if (!bus_lock) bus_lock = xSemaphoreCreateRecursiveMutex();

    e = spi_bus_initialize(CONFIG_COMBO_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) { ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(e)); return -1; }
    e = spi_bus_add_device(CONFIG_COMBO_SPI_HOST, &cfg, &dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(e)); return -1; }

    /* Буферы под ПДП заводим один раз и своими по двум причинам: буфер
     * вызывающего может лежать во внешней памяти, откуда ПДП читать не умеет,
     * и приёмный буфер обязан начинаться с адреса, кратного четырём
     * (spi_master.h:148). heap_caps_malloc даёт и то, и другое. */
    dma_tx = heap_caps_malloc(BLOCK_MAX, MALLOC_CAP_DMA);
    dma_rx = heap_caps_malloc(BLOCK_MAX, MALLOC_CAP_DMA);
    if (!dma_tx || !dma_rx) { ESP_LOGE(TAG, "no DMA-capable memory"); return -1; }

    ESP_LOGI(TAG, "SPI at %d Hz, pins: sck %d, mosi %d, miso %d, cs %d",
             CONFIG_COMBO_SPI_HZ, CONFIG_COMBO_PIN_SCK, CONFIG_COMBO_PIN_MOSI,
             CONFIG_COMBO_PIN_MISO, CONFIG_COMBO_PIN_CS);
    return 0;
}

void link_close(void)
{
    if (dev) { spi_bus_remove_device(dev); dev = 0; }
    spi_bus_free(CONFIG_COMBO_SPI_HOST);
    heap_caps_free(dma_tx); dma_tx = 0;
    heap_caps_free(dma_rx); dma_rx = 0;
}

void link_select(void)
{
    if (bus_lock) xSemaphoreTakeRecursive(bus_lock, portMAX_DELAY);
    gpio_set_level(CONFIG_COMBO_PIN_CS, 0);
}

void link_deselect(void)
{
    gpio_set_level(CONFIG_COMBO_PIN_CS, 1);
    if (bus_lock) xSemaphoreGiveRecursive(bus_lock);
}

uint8_t link_xfer(uint8_t out)
{
    spi_transaction_t t;
    memset(&t, 0, sizeof t);
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.tx_data[0] = out;
    if (spi_device_polling_transmit(dev, &t) != ESP_OK) return 0xFF;
    return t.rx_data[0];
}

void link_xfer_block(const uint8_t *out, uint8_t *in, size_t n)
{
    while (n) {
        size_t k = n > BLOCK_MAX ? BLOCK_MAX : n;
        spi_transaction_t t;

        if (out) memcpy(dma_tx, out, k);
        else     memset(dma_tx, 0, k);

        memset(&t, 0, sizeof t);
        t.length = k * 8;
        t.tx_buffer = dma_tx;
        t.rx_buffer = dma_rx;
        if (spi_device_polling_transmit(dev, &t) != ESP_OK) {
            if (in) memset(in, 0xFF, k);
        }
        else if (in) memcpy(in, dma_rx, k);

        if (out) out += k;
        if (in)  in  += k;
        n -= k;
    }
}

uint32_t link_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void link_delay_us(uint32_t us)
{
    if (us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(us / 1000));
        busy_us = 0;
        us %= 1000;
    }
    if (us) {
        esp_rom_delay_us(us);
        busy_us += us;
    }
    /* Опрос состояния может длиться сотни миллисекунд подряд (ожидание шины от
     * арбитра — до 50 мс тишины). Без этой уступки сторож задач сработает. */
    if (busy_us >= 5000) { vTaskDelay(1); busy_us = 0; }
}
