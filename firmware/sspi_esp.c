/* SSPI к ПЛИС: ноги ESP32-S3, программный SPI в режиме 0.
 *
 * Выводы: IO4 CSSPIN, IO5 MCLK/CCLK, IO6 SI, IO7 SO, IO15 SN. Вне прошивки все
 * они входы: SN и CSSPIN держат наверху R106 и R107 на плате, а ПЛИС с
 * SLAVE_SPI_PORT=ENABLE считает порт своим и без нас.
 *
 * ПРОГРАММНЫЙ, А НЕ КОНТРОЛЛЕР SPI. Оба контроллера общего назначения у S3
 * уже при деле или не нужны (рабочий SPI к ПЛИС -- FSPI), а скорость здесь
 * роли не играет: страница -- 20 байт и 200 мкс записи, весь образ -- около
 * секунды сдвига. Зато фронты видны в коде, и CSSPIN, который TN велит
 * держать снятым при опущенном SN, не забудется. */

#include "fpga.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_CSSPIN CONFIG_COMBO_PIN_SSPI_CSSPIN
#define PIN_CCLK   CONFIG_COMBO_PIN_SSPI_CCLK
#define PIN_SI     CONFIG_COMBO_PIN_SSPI_SI
#define PIN_SO     CONFIG_COMBO_PIN_SSPI_SO
#define PIN_SN     CONFIG_COMBO_PIN_SSPI_SN

/* Прошивка идёт в задаче цикла обслуживания и длится секунды. Сторож задач
 * сработал бы, не дай мы планировщику вздохнуть: уступаем раз в 5 мс. */
static int64_t last_yield;

static void breathe(void)
{
    int64_t now = esp_timer_get_time();
    if (now - last_yield > 5000) { vTaskDelay(1); last_yield = esp_timer_get_time(); }
}

/* Полпериода ~1 мкс: около 400 кГц с учётом gpio_set_level, с запасом от
 * предела SSPI и от звона на проводах, которых тут никто не согласовывал. */
static inline void half(void) { esp_rom_delay_us(1); }

static uint8_t shift(uint8_t out)
{
    uint8_t in = 0;
    int b;
    for (b = 7; b >= 0; b--) {
        gpio_set_level(PIN_SI, (out >> b) & 1);
        half();
        /* SO выставлен кристаллом по прошлому спаду; берём его до фронта --
         * это и есть чтение по фронту в режиме 0. */
        in = (uint8_t)(in << 1 | gpio_get_level(PIN_SO));
        gpio_set_level(PIN_CCLK, 1);
        half();
        gpio_set_level(PIN_CCLK, 0);
    }
    return in;
}

static void txn(void *ctx, const uint8_t *out, size_t out_n, uint8_t *in, size_t in_n)
{
    size_t i;
    (void)ctx;
    gpio_set_level(PIN_SN, 0);
    half();
    for (i = 0; i < out_n; i++) shift(out[i]);
    for (i = 0; i < in_n; i++) in[i] = shift(0x00);
    gpio_set_level(PIN_SI, 0);
    half();
    gpio_set_level(PIN_SN, 1);
    half();
    breathe();
}

static void delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    if (us >= 1000) vTaskDelay(pdMS_TO_TICKS(us / 1000) ? pdMS_TO_TICKS(us / 1000) : 1);
    else esp_rom_delay_us(us);
    breathe();
}

static uint32_t ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const sspi_io io = { txn, delay_us, ms, 0 };

const sspi_io *fpga_io_claim(void)
{
    const gpio_config_t outs = {
        .pin_bit_mask = 1ULL << PIN_CSSPIN | 1ULL << PIN_CCLK | 1ULL << PIN_SI | 1ULL << PIN_SN,
        .mode = GPIO_MODE_OUTPUT,
    };
    const gpio_config_t ins = { .pin_bit_mask = 1ULL << PIN_SO, .mode = GPIO_MODE_INPUT };

    /* Уровни -- до включения выходов: SN не должен мигнуть вниз. */
    gpio_set_level(PIN_SN, 1);
    gpio_set_level(PIN_CSSPIN, 1);
    gpio_set_level(PIN_CCLK, 0);
    gpio_set_level(PIN_SI, 0);
    gpio_config(&outs);
    gpio_config(&ins);
    last_yield = esp_timer_get_time();
    return &io;
}

void fpga_io_release(void)
{
    const gpio_config_t all = {
        .pin_bit_mask = 1ULL << PIN_CSSPIN | 1ULL << PIN_CCLK | 1ULL << PIN_SI |
                        1ULL << PIN_SN | 1ULL << PIN_SO,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_set_level(PIN_SN, 1);
    gpio_config(&all);
}

int fpga_io_extra(char *out, int max) { (void)out; (void)max; return 0; }
