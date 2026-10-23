/* Обновление из сети. Пояснения — в update.h. */
#include "sdkconfig.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

#include "fpga.h"
#include "manifest.h"
#include "ota.h"
#include "pubkey.h"
#include "sha256.h"
#include "update.h"

static const char *TAG = "combo-upd";

/* Откуда читаем манифест. Стабильная ссылка на файл в репозитории, а не
   релиз: релиз заводится ради образов, а текстовый файл незачем перевыпускать.

   У сборки под ESP32-S3 СВОЙ МАНИФЕСТ, и в нём же образ ПЛИС. Прошивка под
   другой кристалл ESP32 на плату не встанет, а общий манифест с одной строкой
   app= отдал бы плате чужую сборку: её отвергла бы лишь проверка образа в
   конце заливки, когда морда уже стёрта и переписана. */
#define MANIFEST_BASE "https://raw.githubusercontent.com/ekundo/combolite-firmware/main/"
#if CONFIG_IDF_TARGET_ESP32S3
#define MANIFEST_URL MANIFEST_BASE "manifest-rev4.txt"
#else
#define MANIFEST_URL MANIFEST_BASE "manifest.txt"
#endif

#define CHECK_EVERY_S  (24 * 60 * 60)
#define FIRST_AFTER_S  60          /* дать плате подняться и не мешать старту */
#define MANIFEST_MAX   2048

static update_state_t state;

static void say_error(const char *why)
{
    snprintf(state.error, sizeof state.error, "%s", why);
    state.checked = 1;
    state.available = state.app = state.fpga = 0;
    ESP_LOGW(TAG, "update check failed: %s", why);
}

/* Открыть и дойти до тела, следуя за перенаправлениями.
 *
 * esp_http_client идёт за ними САМ только в esp_http_client_perform, который
 * читает ответ целиком в память. Нам нужен поток — мегабайт в память не
 * положить, — а в ручном режиме open/fetch_headers перенаправление приходится
 * доводить руками. GitHub без этого не отдаёт ничего: ссылка на образ в релизе
 * всегда уводит на objects.githubusercontent.com, и без перехода скачивание
 * падает за четыре секунды без единой ошибки в журнале: честный ответ 302
 * выглядит отказом. Возвращает 0, когда добрались до 200. */
static int open_following(esp_http_client_handle_t c)
{
    int hop;

    for (hop = 0; hop < 5; hop++) {
        int code;

        if (esp_http_client_open(c, 0) != ESP_OK) return -1;
        esp_http_client_fetch_headers(c);
        code = esp_http_client_get_status_code(c);

        ESP_LOGI(TAG, "hop %d: status %d", hop, code);
        if (code == 200) return 0;
        if (code != 301 && code != 302 && code != 303 &&
            code != 307 && code != 308)
            return code == 404 ? -2 : -1;

        /* set_redirection берёт Location из последнего ответа; соединение перед
           этим надо закрыть, иначе следующий open откроет его поверх старого. */
        esp_http_client_close(c);
        if (esp_http_client_set_redirection(c) != ESP_OK) return -1;
    }
    return -1;
}

/* Скачать в память. Возвращает длину или отрицательное.
 *
 * Сертификат ПРОВЕРЯЕТСЯ — набором корневых из ESP-IDF. Пропустить проверку в
 * esp_http_client нечем: он принимает либо свой сертификат, либо набор, либо
 * общее хранилище, а без всякого отвечает «No server verification option set»
 * и связь не открывает.
 *
 * Устаревание набора не страшно: если он протухнет, перестанет работать только
 * САМОПРОВЕРКА обновлений, а плата продолжит работать, и обновить её
 * по-прежнему можно через combo.py. Кирпичом она не станет.
 *
 * Подпись остаётся вторым рубежом и главным: она отвечает за то, ЧТО именно
 * приехало, а сертификат — только за то, с кем мы говорили.
 *
 * Ограничение по длине обязательно: на том конце может оказаться что угодно,
 * включая бесконечный поток. */
static int fetch(const char *url, char *out, size_t max)
{
    esp_http_client_config_t cfg = { 0 };
    esp_http_client_handle_t c;
    int got = 0, n;

    cfg.url = url;
    cfg.timeout_ms = 15000;
    /* БУФЕРЫ БОЛЬШЕ УМОЛЧАНИЯ, и это не запас на всякий случай. GitHub
       отвечает на ссылку в релизе перенаправлением на objects.githubusercontent.com
       с подписанным адресом — он длиной под тысячу знаков. В умолчательные 512
       байт такой заголовок не влезает, и клиент отвечает «Out of buffer»
       ровно там, где по коду всё правильно: скачивание падает за четыре
       секунды без единой понятной строки в журнале. */
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    /* GitHub уводит на другой хост, за файлом надо идти следом. */
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 5;

    c = esp_http_client_init(&cfg);
    if (!c) return -1;
    {
        int rc = open_following(c);
        if (rc < 0) { esp_http_client_cleanup(c); return rc; }
    }
    while ((size_t)got < max) {
        n = esp_http_client_read(c, out + got, (int)(max - (size_t)got));
        if (n <= 0) break;
        got += n;
    }
    esp_http_client_cleanup(c);
    return got;
}

/* Проверить подпись манифеста нашим открытым ключом. 0 — сошлась. */
static int signature_ok(const char *text, const manifest_t *m)
{
    mbedtls_ecdsa_context ctx;
    uint8_t hash[32];
    int rc;

    mbedtls_ecdsa_init(&ctx);

    /* Ключ — точка кривой в несжатом виде, прямо из pubkey.h: возиться на
       плате с разбором PEM ради шестидесяти пяти байт незачем. */
    rc = mbedtls_ecp_group_load(&ctx.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1);
    if (rc == 0)
        rc = mbedtls_ecp_point_read_binary(&ctx.MBEDTLS_PRIVATE(grp),
                                           &ctx.MBEDTLS_PRIVATE(Q),
                                           COMBO_PUBKEY, sizeof COMBO_PUBKEY);
    if (rc == 0) {
        /* Хешируем ровно то, что подписано: от начала манифеста до строки
           «sig=». Границу посчитал разбор — см. manifest.c, там же объяснено,
           почему её нельзя считать на этой стороне. */
        combo_sha256(text, m->signed_len, hash);
        rc = mbedtls_ecdsa_read_signature(&ctx, hash, sizeof hash,
                                          m->sig, m->sig_len);
    }

    mbedtls_ecdsa_free(&ctx);
    return rc == 0 ? 0 : -1;
}

/* Прочитать манифест и разобрать его. 0 — годен, поля в *m. */
static int read_manifest(char *buf, size_t max, manifest_t *m)
{
    int n = fetch(MANIFEST_URL, buf, max);

    if (n == -2) { say_error("manifest not published yet"); return -1; }
    if (n <= 0)  { say_error("cannot reach the manifest"); return -1; }
    if ((size_t)n >= max) { say_error("manifest too large"); return -1; }

    if (manifest_parse(buf, (size_t)n, m) < 0) {
        say_error(manifest_error());
        return -1;
    }
    if (signature_ok(buf, m) < 0) {
        say_error("manifest signature does not match");
        return -1;
    }
    return 0;
}

/* Образ ПЛИС из манифеста не тот, что прошит. Не знаем, что прошито, --
   тоже «не тот»: лишний раз предложить безвредно, промолчать -- нет. */
static int fpga_differs(const man_part_t *p)
{
    uint8_t sha[32];
    return fpga_flashed(sha) < 0 || memcmp(sha, p->sha, sizeof sha) != 0;
}

int update_check(void)
{
    static char buf[MANIFEST_MAX];
    manifest_t m;
    const esp_app_desc_t *me = esp_app_get_description();

    if (read_manifest(buf, sizeof buf, &m) < 0) return -1;

    state.error[0] = '\0';
    state.checked = 1;
    snprintf(state.version, sizeof state.version, "%s", m.version);
    state.app  = manifest_differs(&m, me ? me->version : "");
    state.fpga = m.fpga.have && fpga_can_flash() && fpga_differs(&m.fpga);
    state.available = state.app || state.fpga;
    ESP_LOGI(TAG, "manifest says %s, we run %s%s%s", m.version,
             me ? me->version : "?", state.app ? " — firmware update available" : "",
             state.fpga ? " — FPGA update available" : "");
    return 0;
}

/* Куда класть пришедшее: раздел флеша или память под образ ПЛИС. */
typedef int (*sink_fn)(uint32_t off, const uint8_t *p, size_t n);

static int sink_ota(uint32_t off, const uint8_t *p, size_t n)
{
    (void)off;
    return ota_write(p, n);
}

/* fpga_part хочет заголовок целиком в первой части, а сеть отдаёт куски как
   придётся -- первый может оказаться короче сорока байт. Заголовок копим
   здесь. */
static int sink_fpga(uint32_t off, const uint8_t *p, size_t n)
{
    static uint8_t hdr[FPGA_HDR];
    const char *msg = "";
    size_t k;

    if (off < FPGA_HDR) {
        k = FPGA_HDR - off < n ? FPGA_HDR - off : n;
        memcpy(hdr + off, p, k);
        p += k; n -= k; off += (uint32_t)k;
        if (off < FPGA_HDR) return 0;
        if (fpga_part(0, hdr, FPGA_HDR, &msg) != 0) goto refused;
    }
    if (n && fpga_part(off, p, n, &msg) != 0) goto refused;
    return 0;
refused:
    ESP_LOGW(TAG, "FPGA image refused: %s", msg);
    return -1;
}

/* Скачать один образ, сверяя размер и хеш с манифестом. 0 -- пришёл весь и
   хеш тот. */
static int stream(const man_part_t *p, sink_fn sink)
{
    esp_http_client_config_t cfg = { 0 };
    esp_http_client_handle_t c;
    static uint8_t chunk[2048];
    sha256_t sum;
    uint8_t hash[32];
    uint32_t got = 0;
    int ok = 1, n;

    cfg.url = p->url;
    cfg.timeout_ms = 20000;
    /* БУФЕРЫ БОЛЬШЕ УМОЛЧАНИЯ, и это не запас на всякий случай. GitHub
       отвечает на ссылку в релизе перенаправлением на objects.githubusercontent.com
       с подписанным адресом — он длиной под тысячу знаков. В умолчательные 512
       байт такой заголовок не влезает, и клиент отвечает «Out of buffer»
       ровно там, где по коду всё правильно: скачивание падает за четыре
       секунды без единой понятной строки в журнале. */
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 5;

    c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (open_following(c) < 0) {
        ESP_LOGW(TAG, "cannot open %s", p->url);
        esp_http_client_cleanup(c);
        return -1;
    }

    combo_sha256_init(&sum);
    while (got < p->size) {
        n = esp_http_client_read(c, (char *)chunk, (int)sizeof chunk);
        if (n <= 0) break;
        if (got + (uint32_t)n > p->size) { ok = 0; break; }
        combo_sha256_update(&sum, chunk, (size_t)n);
        if (sink(got, chunk, (size_t)n) < 0) { ok = 0; break; }
        got += (uint32_t)n;
    }
    esp_http_client_cleanup(c);
    combo_sha256_final(&sum, hash);

    /* ХЕШ СВЕРЯЕТСЯ ДО ТОГО, как раздел объявляется годным. Он взят из
       манифеста, а тот подписан, — значит подмена образа по дороге ловится
       здесь, даже если подпись самого манифеста была настоящей. */
    if (got != p->size || memcmp(hash, p->sha, sizeof hash)) {
        ESP_LOGW(TAG, "got %u of %u bytes, hash %s", (unsigned)got,
                 (unsigned)p->size,
                 memcmp(hash, p->sha, sizeof hash) ? "differs" : "matches");
        ok = 0;
    }
    return ok ? 0 : -1;
}

/* Скачать один образ прямо в раздел, сверяя хеш. */
static int pull(const man_part_t *p, int what)
{
    int ok;

    if (ota_begin(what, p->size) < 0) {
        ESP_LOGW(TAG, "cannot open the partition: %s", ota_error());
        return -1;
    }
    ok = stream(p, sink_ota) == 0;
    if (ota_finish(ok) < 0) return -1;
    return ok ? 0 : -1;
}

int update_apply(void)
{
    static char buf[MANIFEST_MAX];
    manifest_t m;
    const esp_app_desc_t *me = esp_app_get_description();
    int app, fp;

    state.reboot = 0;
    if (read_manifest(buf, sizeof buf, &m) < 0) return -1;
    fp  = m.fpga.have && fpga_can_flash() && fpga_differs(&m.fpga);
    /* Прошивку ESP ставим, если она другая -- или если ставить больше нечего:
       «переставить ту же» иногда и нужно. */
    app = manifest_differs(&m, me ? me->version : "") || !fp;

    /* ОБРАЗ ПЛИС -- ПЕРВЫМ, В ПАМЯТЬ. Его хеш сверяется до того, как что-то
       тронуто: оборвись скачивание -- не останется ни новой прошивки ESP при
       старой ПЛИС, ни стёртого флеша ПЛИС. А прошивается ПЛИС последней: это
       самое долгое, и это перезагружает «Вектор». */
    if (fp && stream(&m.fpga, sink_fpga) < 0) {
        /* Хеш не тот -- выбросить: иначе /api/fpga/flash прошил бы его. */
        fpga_drop();
        say_error("FPGA image did not download");
        return -1;
    }

    /* СНАЧАЛА МОРДА, ПОТОМ ПРОШИВКА, и порядок тут не безразличен. Морда
       обновляется без перезагрузки, прошивка — с ней. Сделай наоборот, и между
       перезагрузкой и второй заливкой плата поработала бы с новой прошивкой и
       старой мордой; а так несовпадение живёт лишь до перезагрузки, которую
       человек делает сам. */
    if (app && m.www.have && pull(&m.www, OTA_WWW) < 0) {
        say_error("web files did not download");
        return -1;
    }
    if (app && pull(&m.app, OTA_APP) < 0) {
        say_error("firmware did not download");
        return -1;
    }
    state.reboot = app;

    /* ПЛИС: строку настроек кристалла не трогаем никогда (без SSPI_FORCE) --
       другая в образе значит другая сборка, и это решает человек, а не
       раздача. После прошивки ПЛИС грузится заново, «Вектор» вместе с ней;
       настройки платы board.c зальёт сам. */
    if (fp) {
        sspi_report rep;
        const char *msg = "";
        int rc = fpga_flash(SSPI_REFRESH, &rep, &msg);
        if (rc != SSPI_OK) {
            snprintf(state.error, sizeof state.error, "FPGA not flashed: %s%s", msg,
                     app ? "; the new firmware runs after a reboot" : "");
            state.checked = 1;
            ESP_LOGW(TAG, "%s", state.error);
            return -1;
        }
        state.fpga = 0;
    }
    if (app) state.app = 0;
    state.available = state.app || state.fpga;
    ESP_LOGW(TAG, "update installed%s%s", fp ? ": FPGA flashed" : "",
             app ? ", reboot to run the new firmware" : "");
    return 0;
}

const update_state_t *update_now(void)
{
    return &state;
}

static void watcher(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(FIRST_AFTER_S * 1000));
    for (;;) {
        update_check();
        vTaskDelay(pdMS_TO_TICKS(CHECK_EVERY_S * 1000));
    }
}

void update_watch(void)
{
    xTaskCreate(watcher, "update", 6144, NULL, 3, NULL);
}
