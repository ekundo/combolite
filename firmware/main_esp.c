/* Точка входа на плате.
 *
 * Порядок важен: сперва шина -- ровно настолько, чтобы отпустить машину,
 * которую ПЛИС держит ВВОДОМ с включения (early_drive_reset), -- потом сеть,
 * потом всё остальное. Если привода нет или он не отвечает, лучше это увидеть
 * в журнале по WiFi, чем гадать у молчащей платы.
 */

#include "ata.h"
#include "board.h"
#include "mouse.h"     /* объявление безвредно и там, где мыши нет */
#include "combo.h"   /* combo_status: сырой байт от ПЛИС в сообщении об отказе */
#include "link.h"
#include "serve.h"
#include "vchan.h"
#include "modem.h"
#include "eslog.h"
#include "netcfg.h"
#include "ota.h"
#include "update.h"
#include "wifi.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "espsec.h"

#include "esp_random.h"
#include "nvs_flash.h"

#include <stdio.h>

static const char *TAG = "combo";

/* Булев Kconfig, выставленный в n, макроса НЕ определяет вовсе — не в ноль, а в
 * ничего. Поэтому `x = CONFIG_ЧТО_ТО` собирается ровно до тех пор, пока опция
 * включена, и разваливается у того, кто её выключит. Ловится это только
 * сборкой с выключенной опцией, а у COMBO_WEB_READ_ONLY как раз default n. */
#ifdef CONFIG_COMBO_READ_ONLY
#  define NBD_READ_ONLY 1
#else
#  define NBD_READ_ONLY 0
#endif

#ifdef CONFIG_COMBO_WEB_READ_ONLY
#  define WEB_READ_ONLY 1
#else
#  define WEB_READ_ONLY 0
#endif

/* ПОКА МАШИНА ДЕРЖИТСЯ ВВОДОМ, ПРИВОД НАШ.
 *
 * С включения ПЛИС сама держит ВВОД 2 с (fpga/hdl/autostart.v): у «Вектора» нет
 * сброса по питанию. Загрузчик машины ждёт готовности винта около 1,5 с и по
 * «занят» решает, что винта нет, -- а сброс привода, который делает прошивка,
 * идёт дольше, а у раскручивающегося винта и секунды. Поэтому сброс здесь, до
 * сети: удержание продлеваем раз в секунду, пока привод не поднимется, и
 * отпускаем машину сами, не дожидаясь конца двух секунд.
 *
 * Только если ВВОД нажат СЕЙЧАС. Перезагрузка ESP посреди работы машины --
 * обновление по воздуху -- застаёт клавиши отпущенными, и тогда остаётся
 * прежний путь: сброс после сети, с захватом шины по тишине.
 *
 * Не дольше HOLD_MAX_MS. Без привода сброс тянется до своего тайм-аута в 31 с,
 * и машина без винта стояла бы всё это время; дальше привод досчитывает в
 * фоне, а машина грузится из ПЗУ, как и без платы.
 *
 * Отпускает board_release: по настройке «загрузчик» машина пойдёт либо в ПЗУ,
 * либо сразу в подставной загрузчик. */
#define HOLD_MAX_MS 10000u

static int early_drive_reset(void)
{
    uint32_t t0, last;
    int loader;

    if (!board_vvod_held()) return 0;
    board_vvod_extend();
    if (ata_reset_start() != ATA_OK) { board_release(); return 0; }

    t0 = last = link_ms();
    while (ata_drive_state() == ATA_DRIVE_RESET &&
           (uint32_t)(link_ms() - t0) < HOLD_MAX_MS) {
        ata_tick();
        if ((uint32_t)(link_ms() - last) >= 1000) {
            board_vvod_extend();
            last = link_ms();
        }
        link_delay_us(1000);
    }
    loader = board_release() == 1;
    ESP_LOGI(TAG, "drive reset while the machine was held: %s in %u ms, %s",
             ata_drive_state() == ATA_DRIVE_READY ? "ready"
             : ata_drive_state() == ATA_DRIVE_RESET ? "still busy" : "dead",
             (unsigned)(link_ms() - t0),
             loader ? "booting the substitute loader" : "booting from ROM");
    return 1;
}

/* Почему плата перезапустилась -- первой строкой журнала. Журнал живёт в
 * памяти и после сброса начинается заново, так что без этой строки падение,
 * сторож и просадка питания выглядят одинаково: просто «плата заново». */
static void log_reset_reason(void)
{
    static const char *const names[] = {
        [ESP_RST_UNKNOWN] = "unknown", [ESP_RST_POWERON] = "power-on",
        [ESP_RST_EXT] = "external pin", [ESP_RST_SW] = "software (esp_restart)",
        [ESP_RST_PANIC] = "panic", [ESP_RST_INT_WDT] = "interrupt watchdog",
        [ESP_RST_TASK_WDT] = "task watchdog", [ESP_RST_WDT] = "other watchdog",
        [ESP_RST_DEEPSLEEP] = "deep sleep", [ESP_RST_BROWNOUT] = "brownout",
        [ESP_RST_SDIO] = "SDIO",
    };
    esp_reset_reason_t r = esp_reset_reason();
    const char *n = ((unsigned)r < sizeof names / sizeof names[0] && names[r]) ? names[r] : "?";
    if (r == ESP_RST_POWERON || r == ESP_RST_SW)
        ESP_LOGI(TAG, "reset reason: %s (%d)", n, (int)r);
    else
        ESP_LOGW(TAG, "reset reason: %s (%d)", n, (int)r);
}

void app_main(void)
{
    /* ОБЕ СТРУКТУРЫ ОБНУЛЯЮТСЯ, и это не бережливость.
     *
     * http_cfg_t лежит на стеке, а поля model и serial здесь заполняются не
     * всегда. В api_info стоит защита «cfg->model ? cfg->model : ""» — от НУЛЯ
     * она спасает, а от мусора со стека нет: первый же запрос /api/info уронил
     * бы плату в LoadProhibited внутри strlen. Морда при этом выглядит так,
     * будто отвергает вход: плата перезагружается, ключ сессии меняется, и её
     * выбрасывает на логин по кругу. */
    net_cfg_t cfg = { 0 };
    http_cfg_t web = { 0 };
    static ata_dev_t drive;
    int have_drive;
    static auth_t auth;
    static allow_t http_allow, nbd_allow;
    static char secret[65];
    static const char *setup_secret;
    int setup = 0, link_ok, early = 0;
    uint8_t seed[SHA256_SIZE];
    uint8_t ip[4];
    esp_vfs_spiffs_conf_t fs = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 4,
        /* Не форматировать при неудаче: пустая морда молча — хуже, чем
         * внятная ругань в журнале о непрошитом разделе. */
        .format_if_mount_failed = false,
    };
    esp_err_t e;

    /* Копить журнал начинаем первым делом: самое нужное в нём — строки о
       подключении к сети, а они печатаются через несколько миллисекунд. */
    eslog_start();
    log_reset_reason();

    e = nvs_flash_init();

    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    /* ШИНА ДО СЕТИ -- ради машины, которую ПЛИС держит ВВОДОМ: см.
     * early_drive_reset. Не вышло поднять SPI -- скажем об этом после сети,
     * когда журнал можно будет прочитать. */
    link_ok = link_open() == 0;
    if (link_ok) {
        /* СНИМАЕМ ЗАПРОС ШИНЫ, ОСТАВШИЙСЯ ОТ ПРОШЛОЙ ЖИЗНИ. Запрос живёт в ПЛИС, а
         * не в нас: команда C_REQ поднимает его, C_REL снимает. Перезагрузись ESP
         * посреди владения — скажем, обновившись по воздуху, — и запрос останется
         * поднятым. Арбитр, отобрав шину по времени, взводит locked и до СНЯТИЯ
         * запроса больше её не даёт (fpga/hdl/ide_arb.v, S_CPU): новый C_REQ ничего не
         * меняет, потому что запрос и так стоит.
         *
         * Тогда сброс привода ждёт шину все 31 секунды тайм-аута и сдаётся с
         * «kept BSY», опознание — с «arbiter did not grant the bus», и привод
         * остаётся мёртвым, хотя жив. Один C_REL в начале снимает весь этот
         * класс отказов. */
        combo_release();
        board_init();
        if (board_present())
            ESP_LOGI(TAG, "FPGA nodes 0x%02x/0x%02x", board_features(),
                     board_features2());
        early = early_drive_reset();
#ifdef CONFIG_COMBO_USB_MOUSE
        mouse_start();
#endif
        /* Канал с «Вектором» (CL.COM, TERM.COM): опрос сам ждёт битстрима
         * с каналом. Модем и экран терминала -- до него: канал их зовёт. */
        modem_start();
        vchan_start();
    }

    /* КУДА ПОДКЛЮЧАТЬСЯ, решается здесь, и порядок важен.
     *
     * Сохранённая сеть перекрывает зашитую при сборке: зашитая — это то, что
     * знал сборщик, сохранённая — то, что сказал хозяин платы, и последнее
     * слово за ним. Не вышло подключиться — плата поднимает свою точку и ждёт
     * указаний, вместо того чтобы вечно перебирать попытки в тишине: у платы в
     * машине сказать о беде иначе некому. */
    {
        static char ssid[33], pass[65];
        int state = netcfg_load(ssid, sizeof ssid, pass, sizeof pass);
        int plan  = netcfg_plan(state, sizeof CONFIG_COMBO_WIFI_SSID > 1);

        if (plan == NETPLAN_BUILTIN) {
            snprintf(ssid, sizeof ssid, "%s", CONFIG_COMBO_WIFI_SSID);
            snprintf(pass, sizeof pass, "%s", CONFIG_COMBO_WIFI_PASS);
        }

        if (plan == NETPLAN_SETUP || wifi_start(ssid, pass, 30000, ip) < 0) {
            static char ap[33];
            if (wifi_setup_start(ap, sizeof ap) < 0) {
                ESP_LOGE(TAG, "cannot start setup mode, nothing to do");
                return;
            }
            setup = 1;
            /* Секрет показываем, лишь пока сеть НИ РАЗУ не настраивали: это
               первая настройка, плата ещё ничья, и взять его человеку больше
               негде. У настроенной платы, потерявшей сеть, здесь ноль — иначе
               точка, поднятая в чужом доме, раздавала бы ключ от диска. */
            setup_secret = netcfg_show_secret(state) ? secret : 0;
        }
    }

    if (!link_ok) {
        ESP_LOGE(TAG, "SPI init failed");
        return;
    }

    e = esp_vfs_spiffs_register(&fs);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "static files not mounted (%s): the API will work, the web UI will not", esp_err_to_name(e));

    cfg.port      = CONFIG_COMBO_NBD_PORT;
    cfg.read_only = NBD_READ_ONLY;
    cfg.sectors   = 0;                  /* взять из опознания привода */
    cfg.once      = 0;
    cfg.token     = CONFIG_COMBO_TOKEN;
    cfg.allow     = &nbd_allow;

    /* Доступ к API. Ключ подписи вызовов свой при каждом запуске — значит
     * перезагрузка платы обнуляет все сессии, и это правильно. */
    espsec_load(secret, sizeof secret);
    esp_fill_random(seed, sizeof seed);
    auth_init(&auth, secret, seed);
    espsec_console();

    /* Опечатка в списке адресов не должна тихо открыть плату всем. */
    if (allow_parse(&http_allow, CONFIG_COMBO_ALLOW_HTTP) < 0 ||
        allow_parse(&nbd_allow,  CONFIG_COMBO_ALLOW_NBD)  < 0) {
        ESP_LOGE(TAG, "malformed address list in the build config");
        return;
    }

    /* Привода нет — морду всё равно поднимаем.
     *
     * Всё, что нужно человеку, здесь уже есть: ata_drive_state() отдаёт
     * «мёртв», /api/info кладёт это в поле drive, морда рисует красным, и там
     * же есть кнопка сброса привода. Молчи плата по сети, единственным
     * способом понять, что случилось, был бы кабель и журнал — при том, что
     * морда сделана ровно для этого.
     *
     * Случай не выдуманный: отвалившийся шлейф IDE выглядит именно так. Да и
     * плата без привода вовсе — обычное дело, пока её проверяют на столе.
     *
     * NBD при этом не поднимаем: экспортировать нечего, а пустой экспорт
     * клиент примет за диск нулевого размера. srv_run это умеет — он берёт
     * конфигурацию NBD отдельным доводом и работает без неё. */
    /* В режиме настройки привод НЕ ТРОГАЕМ. Плата в этот миг стоит в чужой
       сети или в чужих руках, а опознание привода дёргает шину «Вектора» —
       делать это, пока не решено, чья это плата и куда она подключается, ни к
       чему. Заодно настройка идёт быстрее: опознание занимает секунды. */
    /* СБРОС ПЕРЕД ОПОЗНАНИЕМ. По ATA привод после подачи питания положено
     * сбросить. Без сброса CompactFlash на живой плате отвечает на регистры,
     * но данных по IDENTIFY не отдаёт (DRQ не поднимается) — карте нужен
     * сброс, тем более что её вставляют и вынимают на ходу. */
    /* Сброс уже был, пока машина держалась ВВОДОМ, -- тогда только дождаться
     * его конца, если привод ещё раскручивается. Тот сброс делается и в
     * режиме настройки: машина в этот миг стоит в сбросе, и трогать привод
     * ей не мешает. */
    if (!setup && !early) {
        int rst = ata_reset();
        if (rst != ATA_OK)
            ESP_LOGW(TAG, "drive reset: %s", ata_strerror(rst));
    }
    while (early && ata_drive_state() == ATA_DRIVE_RESET) {
        ata_tick();
        link_delay_us(1000);
    }
    int resolve_rc = setup ? 0 : net_resolve(&cfg, &drive);
    have_drive = !setup && resolve_rc == 0;
    if (!have_drive && !setup) {
        /* Два разных отказа, и путать их дорого. ATA_E_LINK значит, что не
         * пришёл признак живости от ПЛИС: старший бит её статуса всегда 1,
         * см. ST_ALIVE. Тогда виноват тракт SPI или незагруженная ПЛИС, а
         * привод тут вообще не при чём. Всё остальное — ответ ПЛИС о том, что
         * привод молчит. */
        if (resolve_rc == ATA_E_LINK) {
            /* Сырой байт статуса стоит того, чтобы его напечатать: разряд 7 у
             * ПЛИС всегда единица, поэтому 0x00 и 0xFF означают разное. 0xFF —
             * линия MISO никем не управляется и висит; 0x00 — её кто-то держит
             * внизу; что-то среднее — обмен идёт, но разъезжается по фазе или
             * частоте. Читаем дважды: одинаковые значения отличают устойчивую
             * картину от случайной. */
            uint8_t st1 = combo_status(), st2 = combo_status();
            ESP_LOGE(TAG, "no answer from the FPGA over SPI: status 0x%02x, "
                          "0x%02x (bit 7 must be 1); check that the FPGA is "
                          "configured (DONE high)", st1, st2);
        }
        else
            ESP_LOGE(TAG, "drive not responding (%s): status 0x%02x, error "
                          "0x%02x; the web UI still comes up, look at the "
                          "drive badge there and try a reset",
                     ata_strerror(resolve_rc), ata_last_status(),
                     ata_last_error());
        /* Если сектор ответа всё же прочитался, печатаем что в нём: читаемое
         * имя модели значит, что шина данных в порядке. */
        if (drive.model[0] || drive.serial[0])
            ESP_LOGW(TAG, "but IDENTIFY data came through: model \"%s\", "
                          "serial \"%s\", %u sectors",
                     drive.model, drive.serial, (unsigned)drive.sectors);
    }

    if (have_drive && !ata_can_flush())
        ESP_LOGI(TAG, "drive has no FLUSH CACHE (ATA-4 and later): writes land "
                      "on the medium as they go, so there is nothing to flush");

    web.port      = CONFIG_COMBO_HTTP_PORT;
    /* НЕ cfg.read_only: то про экспорт по NBD, где опасна смонтированная
     * файловая система. Запись из морды защищена окном разрешения, а этот
     * запрет — крайняя мера и по умолчанию снят. */
    web.read_only = WEB_READ_ONLY;
    web.sectors   = cfg.sectors;
    web.root      = "/www";
    web.rssi      = wifi_rssi;
    web.survey    = wifi_survey_text;
    web.log       = eslog_text;
    /* Обновление из сети — только когда плата В СЕТИ: в режиме настройки идти
       за манифестом некуда, и предлагать это человеку незачем. */
    if (!setup) {
        web.update       = update_now;
        web.update_check = update_check;
        web.update_apply = update_apply;
        update_watch();
    }
    web.setup     = setup;
    web.nets      = wifi_networks;
    web.join      = netcfg_save;
    web.setup_secret = setup_secret;
    /* Пусто, когда привода нет: морда покажет прочерк, и это честно. */
    web.model     = have_drive ? drive.model  : "";
    web.serial    = have_drive ? drive.serial : "";
    web.write_window_s = CONFIG_COMBO_WRITE_WINDOW;
    web.auth      = &auth;
    web.allow     = &http_allow;

    if (!auth_on(&auth))
        ESP_LOGW(TAG, "NO SECRET: the API is open to everyone on this network");
    if (!cfg.token || !*cfg.token)
        ESP_LOGW(TAG, "NO NBD EXPORT NAME: the block device is open to everyone");

    if (setup)
        ESP_LOGW(TAG, "setup mode: open http://192.168.4.1:%u/ over our own AP",
                 (unsigned)web.port);
    else
        ESP_LOGI(TAG, "web UI:       http://%u.%u.%u.%u:%u/",
                 ip[0], ip[1], ip[2], ip[3], (unsigned)web.port);
    /* Сам токен в журнал не пишем: журнал читают через монитор и копируют в
     * переписку не глядя. */
    if (have_drive)
        ESP_LOGI(TAG, "block device: nbd-client %u.%u.%u.%u %u /dev/nbd0%s",
                 ip[0], ip[1], ip[2], ip[3], (unsigned)cfg.port,
                 (cfg.token && *cfg.token) ? " -N <token>" : "");

    /* Дошли сюда — значит прошивка живая: сеть поднята, файловая система
       смонтирована, сервер настроен. Сказать об этом надо ДО srv_run: он не
       возвращается, и после него сказать будет некому. */
    ota_mark_good();

    srv_run(&web, have_drive ? &cfg : 0, 0);    /* не возвращается */
    ESP_LOGE(TAG, "server stopped");
}
