/* Станция WiFi на ESP-IDF.
 *
 * Ничего сверх нужного: одна сеть, бесконечные попытки переподключения,
 * ожидание адреса.
 */

#include "roam.h"
#include "wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

#define BIT_GOT_IP  BIT0

/* --------------------------------------------------------------- переход
 *
 * Плата стоит на месте, а эфир — нет. Сильнейшую точку она выбирает при
 * подключении, но соседи включаются и выключаются, и через час можно
 * обнаружить себя на -91 дБм при живом соседе в -50. Работать при таком
 * сигнале нельзя: пинг остаётся приличным, а полоса падает так, что запрос на
 * 32 КБ идёт секунду.
 *
 * САМО ПРАВИЛО лежит в roam.c: это арифметика, и радио ей не нужно. Здесь
 * остаётся только радио — узнать сигнал, обойти эфир, разорвать связь. */
#define ROAM_CHECK_S  30

/* Сколько ждать, пока сеть переведёт нас сама, прежде чем взяться за дело
   руками. Десяти секунд хватает с запасом: точка отвечает на запрос за
   доли секунды, а сам переход — это одна смена ассоциации. */
#define BTM_WAIT_S    10

static const char *TAG = "combo-wifi";
static EventGroupHandle_t events;
static uint32_t got_ip;
static int attempts;
/* Скан бывает двух родов — по нашей просьбе из морды и сам собой при поиске
   точки получше, — а радио одно. Замок разводит их по очереди: два скана
   одновременно драйвер не примет. */
static SemaphoreHandle_t scan_lock;
/* Сетевой интерфейс собственной точки. Заводится только в режиме настройки —
   в обычной работе точку мы не поднимаем. */
static esp_netif_t *ap_netif;

/* Сторож эфира: определён ниже, рядом с обзором — им обоим нужен один и тот же
   поиск точек, и держать их врозь значило бы разносить одно дело по файлу. */
static void roamer(void *arg);

/* Режим настройки: объявлен здесь, потому что о нём должен знать обработчик
   событий, а живёт он ниже, рядом с самой настройкой. */
static int setup_on;

/* ------------------------------------------------- роуминг силами сети
 *
 * 802.11k и v есть не у каждой точки, а в ESP-IDF они ещё и отключаемы: при
 * снятом CONFIG_ESP_WIFI_11KV_SUPPORT функций esp_wnm_* попросту НЕТ, и сборка
 * валится на линковке.
 *
 * Поэтому весь разговор с сетью идёт через две обёртки. Без поддержки в сборке
 * они честно отвечают «сеть нас не переведёт», и работает прежний путь: обзор
 * эфира и переподключение своими силами. Тот же ответ приходит и от точки,
 * которая 11v не объявляет, — то есть отказ выглядит одинаково, откуда бы он
 * ни пришёл, и ветка отката в коде одна. */
#ifdef CONFIG_ESP_WIFI_11KV_SUPPORT
#include "esp_wnm.h"

static int net_moves_us(void)
{
    return esp_wnm_is_btm_supported_connection() ? 1 : 0;
}

static int ask_net_to_move(void)
{
    return esp_wnm_send_bss_transition_mgmt_query(REASON_RSSI, NULL, 0) == 0;
}
#else
static int net_moves_us(void)   { return 0; }
static int ask_net_to_move(void) { return 0; }
#endif

/* Причина отключения словами.
 *
 * Названия из esp_wifi_types.h есть и в самой ESP-IDF, но отдельной функции
 * «номер в строку» там нет, а помнить наизусть, что 15 это неверный пароль,
 * человеку незачем. Перечислены не все три десятка кодов, а те, что встречаются
 * на столе: остальные придут числом, и по нему найдутся в заголовке. */
static const char *wifi_why(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:          return "authentication expired";
    case WIFI_REASON_AUTH_LEAVE:           return "the access point closed the session";
    case WIFI_REASON_ASSOC_EXPIRE:         return "association expired";
    case WIFI_REASON_NOT_AUTHED:           return "not authenticated";
    case WIFI_REASON_NOT_ASSOCED:          return "not associated";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "wrong password (handshake timed out)";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:    return "handshake timed out";
    case WIFI_REASON_NO_AP_FOUND:          return "no such network in range";
    case WIFI_REASON_AUTH_FAIL:            return "authentication refused: wrong password";
    case WIFI_REASON_ASSOC_FAIL:           return "association refused";
    case WIFI_REASON_BEACON_TIMEOUT:       return "lost the access point (beacon timeout)";
    case WIFI_REASON_CONNECTION_FAIL:      return "connection failed";
    default:                               return "see esp_wifi_types.h";
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* В режиме настройки станция нужна только для обзора эфира. Пусти её
           подключаться — она будет перебирать каналы в поисках сети, которой
           нет, и точка, живущая на том же радио, начнёт терять клиентов. */
        if (!setup_on) esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Пробуем без конца: диск должен вернуться в сеть сам после того, как
         * роутер перезагрузили, а не ждать, пока кто-то придёт к машине.
         *
         * СПАТЬ ЗДЕСЬ НЕЛЬЗЯ. Обработчик выполняется в задаче цикла событий, и
         * vTaskDelay заморозил бы её целиком — вместе с событием о полученном
         * адресе. Пауза не нужна и по существу: сама попытка подключения с
         * поиском сети занимает около секунды.
         *
         * ПРИЧИНУ ПЕЧАТАЕМ, И С ПЕРВОГО РАЗА. Плата стоит внутри машины, журнал
         * — единственный способ понять, почему её нет в сети, и разница между
         * «пароль не тот» и «сети не видно» решает всё. Событие эту разницу
         * знает: reason. */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        int why = d ? d->reason : 0;

        if (setup_on) return;               /* см. выше: в настройке не лезем */

        if (attempts++ == 0 || attempts % 10 == 0)
            ESP_LOGW(TAG, "disconnected: %s (reason %d), attempt %d",
                     wifi_why(why), why, attempts);
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        got_ip = e->ip_info.ip.addr;
        attempts = 0;
        ESP_LOGI(TAG, "address " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(events, BIT_GOT_IP);
    }
}

/* ПОДНЯТЬ РАДИО — отдельно от подключения, и это не вкусовщина.
 *
 * Плата, которую попросили забыть сеть, подключаться не должна — ей сразу в
 * настройку, — но радио ей всё равно нужно. Без него первое же обращение к
 * радио возвращает «неверное состояние», плата печатает «cannot start setup
 * mode» и замирает: ни в сети, ни своей точки.
 *
 * ОБА интерфейса заводятся здесь, до запуска радио. Создать интерфейс точки
 * потом, когда станция уже работает, нельзя: esp_netif_create_default_wifi_ap
 * внутри себя проверяет состояние и валит всё приложение через
 * ESP_ERROR_CHECK — плата уходит в вечную перезагрузку. Лишний интерфейс,
 * пока мы в сети, не стоит ничего. */
static int wifi_up(void)
{
    static int done;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();

    if (done) return 0;

    events = xEventGroupCreate();
    if (!events) return -1;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    done = 1;
    return 0;
}

int wifi_start(const char *ssid, const char *pass, uint32_t timeout_ms,
               uint8_t ip[4])
{
    wifi_config_t cfg;
    EventBits_t bits;

    if (wifi_up() < 0) return -1;

    memset(&cfg, 0, sizeof cfg);
    strncpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof cfg.sta.password - 1);

    /* Искать сеть по ВСЕМ каналам и выбирать самую сильную точку.
     *
     * По умолчанию ESP-IDF сканирует быстрым способом и цепляется за ПЕРВУЮ
     * подходящую — а когда точек с одним именем несколько (repeater, mesh,
     * вторая точка в квартире), первой попадается любая. Плата лежала рядом с
     * роутером и держалась за дальнюю: -88 дБм при живом соседе в -43.
     *
     * Снаружи это выглядит не как слабая связь, а как странно медленный
     * сервер. Пинг при -88 остаётся приличным — пакет ICMP маленький, — зато
     * полоса падает до 32 КБ/с, и запрос на 32 КБ идёт секунду вместо
     * тридцати миллисекунд. Мерить в таком состоянии что бы то ни было
     * бесполезно. */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    /* Сказать сети, что мы умеем 802.11k и v, — если она их поддерживает.
     *
     * 11k (rm) — спросить у точки список соседей вместо того, чтобы обходить
     * все каналы самим. 11v (btm) — попросить сеть перевести нас на другую
     * точку: она знает про эфир больше нашего и умеет сделать это мягко, без
     * разрыва, который у нас рвёт открытые соединения.
     *
     * Флаги безобидны там, где сети это не умеют: точка просто не объявит
     * поддержку, esp_wnm_is_btm_supported_connection ответит «нет», и работает
     * прежний путь — обзор и переподключение своими силами. Именно поэтому
     * никакой настройки «включить роуминг» нет: спросить дешевле, чем
     * заставлять человека знать про свою точку такое. */
    cfg.sta.rm_enabled  = 1;
    cfg.sta.btm_enabled = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Сон радио выключается ПОСЛЕ esp_wifi_start, и порядок здесь решает всё.
     *
     * Старт ставит режим по настройке сборки, а она по умолчанию
     * WIFI_PS_MIN_MODEM, так что вызов перед стартом не действует: по коду сна
     * нет, а на деле станция просыпается раз в три маяка. В журнале это видно
     * строкой драйвера «li: 3, scale listen interval ... to 307200 us», а
     * снаружи — пингом, который вместо миллисекунд даёт 300, 460, 990: ровно
     * кратные тому же интервалу. Обход каталогов от этого распухает на
     * порядок, и выглядит это как слабый сигнал.
     *
     * Экономить тут не на чем: плата питается от машины, а сервер обязан
     * отвечать сразу. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    bits = xEventGroupWaitBits(events, BIT_GOT_IP, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms));
    if (!(bits & BIT_GOT_IP)) {
        ESP_LOGE(TAG, "no address within %u ms", (unsigned)timeout_ms);
        return -1;
    }
    if (ip) memcpy(ip, &got_ip, 4);

    /* Сторож эфира. Отдельной задачей, а не в цикле сервера: обзор занимает
       радио на пару секунд, и сервер всё это время всё равно отвечал бы, а вот
       засунь мы это в его виток — морда замирала бы на ровном месте. */
    scan_lock = xSemaphoreCreateMutex();
    if (scan_lock) xTaskCreate(roamer, "roam", 3072, NULL, 3, NULL);

    /* Сказать, что вышло НА САМОМ ДЕЛЕ, а не что мы просили: заявленный в
     * коде режим и действующий могут разойтись (см. выше). */
    {
        wifi_ps_type_t ps = WIFI_PS_NONE;
        wifi_ap_record_t ap;

        esp_wifi_get_ps(&ps);
        if (ps != WIFI_PS_NONE)
            ESP_LOGW(TAG, "radio power save is ON (mode %d): replies will lag",
                     (int)ps);
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            ESP_LOGI(TAG, "AP %02x:%02x:%02x:%02x:%02x:%02x, channel %d, %d dBm",
                     ap.bssid[0], ap.bssid[1], ap.bssid[2],
                     ap.bssid[3], ap.bssid[4], ap.bssid[5],
                     ap.primary, ap.rssi);
    }
    return 0;
}

int wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

/* Как ЗОВЁТСЯ наша сеть — та, в которой плата сейчас, а не та, что знал
 * сборщик.
 *
 * Фильтровать обзор и поиск точки получше по CONFIG_COMBO_WIFI_SSID нельзя:
 * у платы, настроенной через морду, сеть другая, а у собранной без зашитой
 * сети имя это ПУСТОЕ. Тогда обзор отвечал бы «no access point with this name
 * is in range», сидя в работающей сети, а сторож эфира не находил бы ни одной
 * точки и не мог перейти никуда. Второе не видно вовсе, пока не приглядишься,
 * — и это хуже первого.
 *
 * Спрашиваем у драйвера: он знает, с кем связан. Пусто — значит связи нет, и
 * тогда искать по имени нечего. */
static const char *our_ssid(char *buf, size_t max)
{
    wifi_ap_record_t ap;

    buf[0] = '\0';
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        snprintf(buf, max, "%s", (const char *)ap.ssid);
    return buf;
}

/* Взять список точек с нашим именем сети. Возвращает сколько нашлось или -1.
   Радио на это время занято; звать только под scan_lock. */
static int look(wifi_ap_record_t *found, uint16_t max)
{
    wifi_scan_config_t sc = { 0 };
    char name[33];

    /* Имени нет — ищем всё подряд: пусть лучше человек увидит весь эфир, чем
       пустоту с непонятным «такой сети не видно». */
    our_ssid(name, sizeof name);
    sc.ssid = name[0] ? (uint8_t *)name : NULL;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;
    if (esp_wifi_scan_get_ap_records(&max, found) != ESP_OK) return -1;
    return (int)max;
}

/* Раз в полминуты смотреть на сигнал, и только если он плох — искать замену.
 *
 * Проверка сигнала бесплатна, а вот обзор занимает радио на пару секунд, и
 * делать его при хорошей связи значило бы мешать работе ради ничего. */
static void roamer(void *arg)
{
    static wifi_ap_record_t found[16];
    /* Так, чтобы первый же случай считался «давно не переходили»: секунд с
       прошлого перехода выходит заведомо больше паузы. */
    int64_t last_roam = -(int64_t)ROAM_PAUSE_S * 2000000;

    (void)arg;
    for (;;) {
        wifi_ap_record_t cur;
        int n, i, best = -1;
        /* Переводит ли нас сеть сама. От этого зависят и пороги, и то, каким
           путём переходить: мягкий переход дёшев, и терпеть плохую точку ради
           него незачем. */
        int soft = net_moves_us();

        vTaskDelay(pdMS_TO_TICKS(ROAM_CHECK_S * 1000));

        if (esp_wifi_sta_get_ap_info(&cur) != ESP_OK) continue;
        /* Сигнал стоит ничего, обзор — пару секунд занятого радио. Поэтому
           сперва дешёвая проверка, и только потом дорогая. */
        if (cur.rssi >= (soft ? ROAM_POOR_SOFT : ROAM_POOR)) continue;
        if (xSemaphoreTake(scan_lock, 0) != pdTRUE) continue;

        n = look(found, sizeof found / sizeof found[0]);
        xSemaphoreGive(scan_lock);
        if (n <= 0) continue;

        for (i = 0; i < n; i++)
            if (best < 0 || found[i].rssi > found[best].rssi) best = i;
        if (best < 0) continue;
        if (!roam_decide(cur.rssi, found[best].rssi,
                         !memcmp(found[best].bssid, cur.bssid, 6),
                         (esp_timer_get_time() - last_roam) / 1000000, soft))
            continue;

        ESP_LOGW(TAG, "moving to a stronger AP: %d dBm here, %d dBm there",
                 cur.rssi, found[best].rssi);
        last_roam = esp_timer_get_time();

        /* СНАЧАЛА ПРОСИМ СЕТЬ. Она знает про эфир больше нашего, а главное —
           переводит мягко: ассоциация меняется без разрыва, и открытые
           соединения переживают переход. Наш собственный путь этого не умеет
           вовсе, он рвёт связь и собирает её заново.
        
           Ждём ответа делом, а не словом: смотрим, сменилась ли точка. Сеть
           вправе и отказать — например, если считает нынешнюю точку лучшей, —
           и тогда переходим сами. */
        if (soft) {
            int i;

            ESP_LOGI(TAG, "asking the network to move us (802.11v)");
            if (ask_net_to_move()) {
                for (i = 0; i < BTM_WAIT_S; i++) {
                    wifi_ap_record_t now;

                    vTaskDelay(pdMS_TO_TICKS(1000));
                    if (esp_wifi_sta_get_ap_info(&now) != ESP_OK) continue;
                    if (memcmp(now.bssid, cur.bssid, 6)) break;
                }
                if (i < BTM_WAIT_S) {
                    ESP_LOGI(TAG, "the network moved us");
                    continue;
                }
                ESP_LOGW(TAG, "the network did not move us; doing it ourselves");
            }
        }

        /* Разрываем связь, а точку выбирать не указываем: при
           WIFI_CONNECT_AP_BY_SIGNAL драйвер сам возьмёт сильнейшую, и это тот
           же путь, которым плата подключается при включении. Одна дорога
           вместо двух — меньше мест, где может разойтись. */
        esp_wifi_disconnect();
    }
}

int wifi_survey_text(char *out, size_t max)
{
    /* Обзор идёт по всем каналам и занимает пару секунд, а сервер на это время
       занят: он однопоточный, и другие запросы подождут. Для ручной команды это
       честная цена, и потому обзор НЕ делается сам собой при загрузке морды. */
    static wifi_ap_record_t found[16];
    wifi_ap_record_t cur;
    size_t at = 0;
    int n, i;

    /* Ждём не дольше пяти секунд: если radio занято поиском точки получше, это
       кончится само, а висеть в ответе на запрос морды нельзя. */
    if (!scan_lock || xSemaphoreTake(scan_lock, pdMS_TO_TICKS(5000)) != pdTRUE)
        return snprintf(out, max, "the radio is busy, try again\n");
    n = look(found, sizeof found / sizeof found[0]);
    xSemaphoreGive(scan_lock);
    if (n < 0) return snprintf(out, max, "scan failed\n");

    if (esp_wifi_sta_get_ap_info(&cur) != ESP_OK) memset(&cur, 0, sizeof cur);

    for (i = 0; i < n && at + 64 < max; i++) {
        int here = !memcmp(found[i].bssid, cur.bssid, 6);
        at += (size_t)snprintf(out + at, max - at,
                   "%s %02x:%02x:%02x:%02x:%02x:%02x  channel %2d  %4d dBm%s\n",
                   here ? "->" : "  ",
                   found[i].bssid[0], found[i].bssid[1], found[i].bssid[2],
                   found[i].bssid[3], found[i].bssid[4], found[i].bssid[5],
                   found[i].primary, found[i].rssi, here ? "  (connected)" : "");
    }
    /* Пусто бывает по двум разным причинам, и путать их нельзя: либо сети с
       таким именем в эфире нет, либо мы вообще ни к кому не подключены и имя
       спрашивать не у кого. Один ответ на оба случая в сети, где плата
       преспокойно работает, читался бы как поломка. */
    if (!n) {
        char name[33];
        our_ssid(name, sizeof name);
        at += (size_t)snprintf(out + at, max - at, name[0]
                   ? "no access point named \"%s\" is in range\n"
                   : "not connected to any network\n", name);
    }

    /* Умеет ли сеть переводить нас сама. Строка стоит здесь, а не в /api/info,
       потому что отвечает на вопрос ровно этого места: почему плата сидит не
       на той точке и что она может с этим сделать. */
    if (at + 96 < max)
        at += (size_t)snprintf(out + at, max - at, "\nroaming: %s\n",
                   net_moves_us()
                   ? "the network moves us itself (802.11v)"
                   : "not offered here — we reconnect on our own");
    return (int)at;
}

void wifi_survey(void)
{
    static char text[1024];
    if (wifi_survey_text(text, sizeof text) > 0) printf("%s", text);
}

/* ------------------------------------------------------------ настройка */

int wifi_setup_active(void) { return setup_on; }

int wifi_setup_start(char *name, size_t max)
{
    /* Имя точки с хвостом из своего MAC: плат в комнате может оказаться две, а
       различить их иначе нечем — ни экрана, ни наклейки у них нет. */
    wifi_config_t cfg = { 0 };
    uint8_t mac[6] = { 0 };

    /* MAC берём из efuse, а не у драйвера: до esp_wifi_start драйвер отдаёт
       нули, и точка называлась бы «combolite-0000». В efuse он есть всегда,
       ещё до подъёма радио. */
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, max, "combolite-%02x%02x", mac[4], mac[5]);

    /* Сеть ОТКРЫТА, и это осознанный размен.
     *
     * Пароль пришлось бы откуда-то узнать, а узнать его негде: ни экрана, ни
     * наклейки, а шнурок — ровно то, от чего мы уходим. Значит либо открытая
     * точка, либо плата, которую нельзя настроить.
     *
     * Чем это ограничено. Точка поднимается ТОЛЬКО когда плата не смогла
     * подключиться, то есть в этот миг она и так никому не служит; диска за ней
     * нет — привод в режиме настройки не трогается вовсе; а секрет показывается
     * лишь пока сеть ни разу не настраивали (см. api_setup). Настроенную плату
     * чужой в худшем случае перенастроит на свою сеть — но не прочтёт ни диска,
     * ни секрета. */
    /* Радио могло быть и не поднято: если сеть забыта, подключаться мы не
       пробовали вовсе и сюда пришли сразу. */
    if (wifi_up() < 0 || !ap_netif) return -1;

    snprintf((char *)cfg.ap.ssid, sizeof cfg.ap.ssid, "%s", name);
    cfg.ap.ssid_len = (uint8_t)strlen(name);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_OPEN;

    /* Признак ставим ДО перезапуска радио: обработчик событий по нему узнаёт,
       что переподключаться больше не надо. Иначе станция принялась бы искать
       сеть, которой нет, и мешала бы точке — они делят одно радио. */
    setup_on = 1;

    esp_wifi_stop();
    /* APSTA, а не просто AP: обзор эфира для страницы настройки делает
       станция, и без неё человеку пришлось бы набирать имя сети вслепую. */
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) { setup_on = 0; return -1; }
    if (esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK) { setup_on = 0; return -1; }
    if (esp_wifi_start() != ESP_OK) { setup_on = 0; return -1; }

    ESP_LOGW(TAG, "setup mode: join \"%s\" and open http://192.168.4.1/", name);
    return 0;
}

int wifi_networks(combo_net_t *out, int max)
{
    /* Режим APSTA: станция ищет сети, пока точка раздаёт свою. Иначе человеку
       пришлось бы выбирать сеть вслепую, набирая имя руками. */
    static wifi_ap_record_t found[24];
    wifi_scan_config_t sc = { 0 };
    uint16_t n = sizeof found / sizeof found[0];
    int i, k = 0;

    if (!scan_lock) scan_lock = xSemaphoreCreateMutex();
    if (!scan_lock || xSemaphoreTake(scan_lock, pdMS_TO_TICKS(8000)) != pdTRUE)
        return -1;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK ||
        esp_wifi_scan_get_ap_records(&n, found) != ESP_OK) {
        xSemaphoreGive(scan_lock);
        return -1;
    }
    xSemaphoreGive(scan_lock);

    for (i = 0; i < (int)n && k < max; i++) {
        int j, seen = 0;
        if (!found[i].ssid[0]) continue;          /* скрытые пропускаем: выбрать их нельзя */
        /* Одна сеть на нескольких точках даёт одно имя несколько раз — человеку
           нужен список сетей, а не точек. */
        for (j = 0; j < k; j++)
            if (!strcmp(out[j].ssid, (char *)found[i].ssid)) { seen = 1; break; }
        if (seen) continue;
        snprintf(out[k].ssid, sizeof out[k].ssid, "%s", (char *)found[i].ssid);
        out[k].rssi = found[i].rssi;
        out[k].secure = found[i].authmode != WIFI_AUTH_OPEN;
        k++;
    }
    return k;
}
