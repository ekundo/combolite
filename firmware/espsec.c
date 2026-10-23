/* Секрет платы в NVS. Пояснения — в espsec.h. НА ЖЕЛЕЗЕ НЕ ПРОВЕРЕНО. */
#include "espsec.h"

#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"

#include "wifi.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define NS   "combo"
#define KEY  "secret"
#define BITS 16                         /* 128 бит */

static const char *TAG = "secret";

static void generate(char *out, size_t max)
{
    uint8_t r[BITS];
    esp_fill_random(r, sizeof r);
    if (max < BITS * 2 + 1) { out[0] = '\0'; return; }
    auth_hex(r, sizeof r, out);
}

static int store(const char *s)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return -1;
    e = nvs_set_str(h, KEY, s);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static int fetch(char *out, size_t max)
{
    nvs_handle_t h;
    size_t n = max;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e != ESP_OK) return -1;
    e = nvs_get_str(h, KEY, out, &n);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

int espsec_load(char *out, size_t max)
{
    out[0] = '\0';

    /* Задан в настройке сборки — значит им распоряжается хозяин платы, и
     * трогать NVS незачем. */
    if (sizeof CONFIG_COMBO_SECRET > 1) {
        snprintf(out, max, "%s", CONFIG_COMBO_SECRET);
        return 0;
    }

    if (fetch(out, max) == 0 && out[0]) return 0;

    generate(out, max);
    if (!out[0] || store(out) < 0) {
        ESP_LOGE(TAG, "cannot store the secret in NVS");
        out[0] = '\0';
        return 0;
    }
    /* Единственный раз, когда он попадает в журнал. */
    ESP_LOGW(TAG, "new board secret generated, WRITE IT DOWN: %s", out);
    ESP_LOGW(TAG, "it will not be printed again; use the 'secret' command");
    return 0;
}

/* Обзор эфира. Живёт рядом с командой секрета не по смыслу, а потому что
   консоль поднимается здесь: регистрировать команду можно только после того,
   как она создана. */
static int cmd_wifi(int argc, char **argv)
{
    (void)argc; (void)argv;
    wifi_survey();
    return 0;
}

static int cmd_secret(int argc, char **argv)
{
    char s[65];

    if (argc > 1 && !strcmp(argv[1], "new")) {
        generate(s, sizeof s);
        if (!s[0] || store(s) < 0) { printf("cannot store the secret\n"); return 1; }
        printf("new secret: %s\n", s);
        printf("all sessions are void; sign in again\n");
        return 0;
    }
    if (sizeof CONFIG_COMBO_SECRET > 1) {
        printf("secret comes from the build config, not from NVS\n");
        return 0;
    }
    if (fetch(s, sizeof s) < 0 || !s[0]) { printf("no secret stored\n"); return 1; }
    printf("secret: %s\n", s);
    return 0;
}

void espsec_console(void)
{
    /* Монитор поднимается здесь же: команда без него бесполезна, а
     * восстановление секрета — единственное, ради чего он вообще нужен. */
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    static esp_console_cmd_t survey = {
        .command = "wifi",
        .help    = "list the access points with our network name and show which one we use",
        .hint    = NULL,
        .func    = &cmd_wifi,
        .argtable = NULL
    };
    static esp_console_cmd_t cmd = {
        .command = "secret",
        .help    = "show the board secret; 'secret new' generates another one",
        .hint    = NULL,
        .func    = &cmd_secret,
        .argtable = NULL
    };

    rc.prompt = "combo>";
    if (esp_console_new_repl_uart(&uc, &rc, &repl) != ESP_OK) {
        ESP_LOGW(TAG, "console not started: the secret cannot be recovered over the wire");
        return;
    }
    esp_console_register_help_command();
    esp_console_cmd_register(&cmd);
    esp_console_cmd_register(&survey);
    esp_console_start_repl(repl);
}
