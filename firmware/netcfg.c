/* Сохранённая сеть в NVS. Пояснения — в netcfg.h. */
#include "netcfg.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "nvs_flash.h"
#endif

/* Тот же ящик, что у секрета платы: обе вещи про одну плату, и разносить их по
   разным пространствам значило бы плодить имена без нужды. */
#define NS    "combo"
#define K_SSID "ssid"
#define K_PASS "pass"
/* Признак «сеть выбирал человек». Отдельно от имени, потому что пустое имя —
   это тоже его выбор: «не подключайся никуда, жди указаний». Без этого
   признака пустое имя было бы неотличимо от ненастроенной платы. */
#define K_SET  "netset"

int netcfg_plan(int state, int builtin)
{
    /* Забытая сеть — решение человека, и зашитая его не отменяет. Правило
       «нет сохранённой — возьми зашитую» обязано отличать «ещё не решали» от
       «решили никуда не подключаться». */
    if (state == NETCFG_FORGOTTEN) return NETPLAN_SETUP;
    if (state == NETCFG_HAVE)      return NETPLAN_SAVED;
    return builtin ? NETPLAN_BUILTIN : NETPLAN_SETUP;
}

int netcfg_show_secret(int state)
{
    return state == NETCFG_NONE;
}

#ifdef ESP_PLATFORM

int netcfg_load(char *ssid, size_t ssid_max, char *pass, size_t pass_max)
{
    nvs_handle_t h;
    size_t n;

    uint8_t set = 0;

    ssid[0] = pass[0] = '\0';
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return NETCFG_NONE;

    n = ssid_max;
    if (nvs_get_str(h, K_SSID, ssid, &n) != ESP_OK) ssid[0] = '\0';
    n = pass_max;
    if (nvs_get_str(h, K_PASS, pass, &n) != ESP_OK) pass[0] = '\0';
    if (nvs_get_u8(h, K_SET, &set) != ESP_OK) set = 0;

    nvs_close(h);
    if (!set) return NETCFG_NONE;                 /* плату ещё не настраивали */
    return ssid[0] ? NETCFG_HAVE : NETCFG_FORGOTTEN;
}

int netcfg_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);

    if (e != ESP_OK) return -1;
    e = nvs_set_str(h, K_SSID, ssid ? ssid : "");
    if (e == ESP_OK) e = nvs_set_str(h, K_PASS, pass ? pass : "");
    /* Признак ставится и при забывании: пустое имя со знаком «решено» и есть
       «в настройку». */
    if (e == ESP_OK) e = nvs_set_u8(h, K_SET, 1);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

int netcfg_forget(void)
{
    return netcfg_save("", "");
}

#else   /* без ESP-IDF хранить негде */

int netcfg_load(char *ssid, size_t ssid_max, char *pass, size_t pass_max)
{
    (void)ssid_max; (void)pass_max;
    ssid[0] = pass[0] = '\0';
    return NETCFG_NONE;
}

int netcfg_save(const char *ssid, const char *pass)
{
    (void)ssid; (void)pass;
    return -1;
}

int netcfg_forget(void) { return -1; }

#endif
