/* Приём обновления. См. ota.h. */
#include "sdkconfig.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_spiffs.h"

#include "ota.h"

static const char *TAG_ERR;          /* беда, о которой скажем наружу */
static esp_ota_handle_t app_h;       /* приём прошивки */
static const esp_partition_t *part;  /* куда пишем */
static size_t want, done;
static int kind = -1;

static int fail(const char *why)
{
    TAG_ERR = why;
    if (kind == OTA_APP && app_h) { esp_ota_abort(app_h); app_h = 0; }
    kind = -1;
    return -1;
}

int ota_begin(int what, size_t len)
{
    TAG_ERR = 0;
    if (kind >= 0) return fail("another update is already in progress");
    if (!len) return fail("empty body");

    if (what == OTA_APP) {
        part = esp_ota_get_next_update_partition(NULL);
        if (!part) return fail("no spare partition for firmware");
        if (len > part->size) return fail("firmware larger than the partition");
        if (esp_ota_begin(part, len, &app_h) != ESP_OK)
            return fail("cannot open the spare partition");
    } else {
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
        if (!part) return fail("no www partition");
        if (len > part->size) return fail("image larger than the partition");
        /* Отмонтировать НАДО: SPIFFS держит свой кэш, и запись мимо него
           оставила бы файловую систему при старом представлении о диске.
           Морда на это время недоступна — она и лежит в этом разделе. */
        esp_vfs_spiffs_unregister("www");
        if (esp_partition_erase_range(part, 0, part->size) != ESP_OK)
            return fail("cannot erase the www partition");
    }

    want = len;
    done = 0;
    kind = what;
    return 0;
}

int ota_write(const uint8_t *buf, size_t n)
{
    if (kind < 0) return -1;
    if (done + n > want) return fail("more data than promised");

    if (kind == OTA_APP) {
        if (esp_ota_write(app_h, buf, n) != ESP_OK) return fail("write failed");
    } else {
        if (esp_partition_write(part, done, buf, n) != ESP_OK)
            return fail("write failed");
    }
    done += n;
    return 0;
}

int ota_finish(int ok)
{
    int what = kind;

    if (what < 0) return -1;
    if (ok && done != want) ok = 0, TAG_ERR = "fewer bytes than promised";

    if (what == OTA_APP) {
        esp_err_t e = ok ? esp_ota_end(app_h) : ESP_FAIL;
        app_h = 0;
        kind = -1;
        if (!ok) { if (!TAG_ERR) TAG_ERR = "signature did not match"; return -1; }
        if (e != ESP_OK) return fail("the image did not pass its own checks");
        if (esp_ota_set_boot_partition(part) != ESP_OK)
            return fail("cannot switch to the new firmware");
        return 0;
    }

    kind = -1;
    /* Смонтировать обратно в любом случае: при неудаче морда испорчена, но
       раздел должен быть доступен — иначе и попытку повторить будет неоткуда. */
    {
        esp_vfs_spiffs_conf_t fs = { .base_path = "/www", .partition_label = "www",
                                     .max_files = 4, .format_if_mount_failed = false };
        esp_vfs_spiffs_register(&fs);
    }
    if (!ok) { if (!TAG_ERR) TAG_ERR = "signature did not match"; return -1; }
    return 0;
}

const char *ota_error(void)
{
    return TAG_ERR ? TAG_ERR : "no error";
}

const char *ota_version(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "unknown";
}

const char *ota_running(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "unknown";
}

void ota_mark_good(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *p = esp_ota_get_running_partition();

    if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;   /* давно работает */
    esp_ota_mark_app_valid_cancel_rollback();
}

void ota_reboot(void)
{
    /* Полсекунды на то, чтобы ответ ушёл в сеть: он уже отдан ядру, но пакет
       ещё не подтверждён, а перезагрузка обрывает всё разом. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
