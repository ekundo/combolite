/* USB-мышь в розетке платы: хост USB ESP32-S3 -> пакет C_MOUSE -> ПЛИС, а
 * дальше машина читает порт D4h (интерфейс KTSerg, fpga/hdl/mouse.v).
 *
 * ЗАГРУЗОЧНЫЙ ПРОТОКОЛ HID, А НЕ ОТЧЁТЫ ПО ОПИСАНИЮ. Для мыши он задан
 * стандартом раз и навсегда: кнопки, dx, dy, у большинства ещё колесо. Разбор
 * описателя отчётов дал бы то же самое для обычной мыши, а для необычной --
 * лишнюю сотню строк. Поэтому берём только интерфейс с подклассом «загрузочный»
 * (1) и протоколом «мышь» (2) и переводим его в загрузочный протокол. У такого
 * отчёта нет номера, и байты стоят на своих местах.
 *
 * КОЛЕСО -- ТОЛЬКО В ПРОТОКОЛЕ ОТЧЁТОВ. Загрузочный протокол знает три байта,
 * и колесо четвёртым туда кладут не все: мышь первой rev4 слала ровно три
 * (25.10.2026). Поэтому сначала читаем описатель отчётов (hidmouse.c): нашлись
 * в нём X и Y -- включаем протокол отчётов и берём кнопки, сдвиги и колесо с
 * найденных мест. Не нашлись или описатель не прочитался -- загрузочный
 * протокол, как раньше, колесо четвёртым байтом, если он есть.
 *
 * Сумму движений ведёт ПЛИС: каждый пакет прибавляется к накопителю, машина
 * забирает накопленное, когда читает порт. Поэтому отчёт уходит сразу, без
 * своего накопления, и терять его нельзя -- отсюда отправка прямо из задачи
 * USB под замком шины (link_espidf.c). */

#include "mouse.h"
#include "board.h"
#include "combo.h"
#include "hidmouse.h"

#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

static const char *TAG = "mouse";

#define HID_SET_IDLE     0x0A
#define HID_SET_PROTOCOL 0x0B
#define HID_SUBCLASS_BOOT 1
#define HID_PROTO_MOUSE   2

static usb_host_client_handle_t client;
static usb_device_handle_t dev;             /* открытая мышь, 0 -- нет */
static usb_transfer_t *in_xfer, *ctl_xfer;
static uint8_t intf, ep;
static SemaphoreHandle_t ctl_done;

/* События клиента приходят в его же задаче, внутри handle_events, -- там
 * нельзя ждать управляющей посылки (её ответ придёт туда же). Поэтому здесь
 * только отметки, а работа -- в цикле задачи. */
static uint8_t new_addr;
static usb_device_handle_t gone;
static uint8_t last_btn;                    /* кнопки последнего отчёта */
static hidm_t layout;                       /* раскладка отчёта по описателю */
static int by_desc;                         /* 1 -- протокол отчётов и layout */

#define DESC_MAX 512                        /* описатель отчётов мыши -- десятки байт */

static void client_cb(const usb_host_client_event_msg_t *m, void *arg)
{
    (void)arg;
    if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) new_addr = m->new_dev.address;
    else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) gone = m->dev_gone.dev_hdl;
}

static void ctl_cb(usb_transfer_t *t)
{
    (void)t;
    xSemaphoreGive(ctl_done);
}

static void in_cb(usb_transfer_t *t)
{
    static int said_len, said_wheel;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes >= 3) {
        const uint8_t *r = t->data_buffer;
        uint8_t btn;
        int dx, dy, wh;
        /* Для журнала: длина отчёта -- при каждой смене, колесо -- один раз,
         * первое ненулевое. */
        if ((int)t->actual_num_bytes != said_len) {
            said_len = (int)t->actual_num_bytes;
            ESP_LOGI(TAG, "report: %d bytes", said_len);
        }
        if (by_desc) {
            if (hidm_decode(&layout, r, (int)t->actual_num_bytes, &btn, &dx, &dy, &wh) != 0)
                goto again;                 /* чужой отчёт того же устройства */
        } else {
            /* Кнопки -- три младших разряда: у интерфейса машины их три. */
            btn = r[0] & 0x07;
            dx = (int8_t)r[1]; dy = (int8_t)r[2];
            wh = t->actual_num_bytes >= 4 ? (int8_t)r[3] : 0;
        }
        if (!said_wheel && wh) {
            said_wheel = 1;
            ESP_LOGI(TAG, "wheel seen: %d", wh);
        }
        last_btn = btn;
        if (board_features() & BF_MOUSE)
            combo_mouse(last_btn, (int8_t)dx, (int8_t)dy, (int8_t)wh);
    }
again:
    /* Отменённую и ушедшую с устройством не перезапускаем: мышь вынули. */
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        t->status == USB_TRANSFER_STATUS_CANCELED || !dev)
        return;
    if (usb_host_transfer_submit(t) != ESP_OK)
        ESP_LOGW(TAG, "interrupt transfer not resubmitted");
}

/* Классовая посылка без данных; ответ ждём, прокачивая события сами. */
static esp_err_t hid_request(uint8_t req, uint16_t value)
{
    usb_setup_packet_t *s = (usb_setup_packet_t *)ctl_xfer->data_buffer;
    TickType_t t0 = xTaskGetTickCount();

    s->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
                       USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    s->bRequest = req;
    s->wValue   = value;
    s->wIndex   = intf;
    s->wLength  = 0;
    ctl_xfer->num_bytes        = sizeof(usb_setup_packet_t);
    ctl_xfer->device_handle    = dev;
    ctl_xfer->bEndpointAddress = 0;
    ctl_xfer->callback         = ctl_cb;
    xSemaphoreTake(ctl_done, 0);
    if (usb_host_transfer_submit_control(client, ctl_xfer) != ESP_OK) return ESP_FAIL;
    while (xSemaphoreTake(ctl_done, 0) != pdTRUE) {
        if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(500)) return ESP_ERR_TIMEOUT;
        usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
    }
    return ctl_xfer->status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
}

/* Описатель отчётов интерфейса (HID 1.11, 7.1.1) -> layout. 0 -- разобран.
 * Посылка своя, на DESC_MAX: общая ctl_xfer рассчитана на запросы без данных. */
static int read_layout(void)
{
    const usb_device_desc_t *dd;
    usb_transfer_t *x;
    usb_setup_packet_t *s;
    TickType_t t0 = xTaskGetTickCount();
    int mps0 = 8, n, rc = -1;

    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK) mps0 = dd->bMaxPacketSize0;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + usb_round_up_to_mps(DESC_MAX, mps0),
                                0, &x) != ESP_OK)
        return -1;
    s = (usb_setup_packet_t *)x->data_buffer;
    s->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                       USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    s->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    s->wValue   = 0x22 << 8;                /* описатель отчётов, первый */
    s->wIndex   = intf;
    s->wLength  = DESC_MAX;
    x->num_bytes        = sizeof(usb_setup_packet_t) + usb_round_up_to_mps(DESC_MAX, mps0);
    x->device_handle    = dev;
    x->bEndpointAddress = 0;
    x->callback         = ctl_cb;
    xSemaphoreTake(ctl_done, 0);
    if (usb_host_transfer_submit_control(client, x) == ESP_OK) {
        while (xSemaphoreTake(ctl_done, 0) != pdTRUE) {
            if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(500)) goto out;   /* x в полёте -- не освобождать */
            usb_host_client_handle_events(client, pdMS_TO_TICKS(10));
        }
        n = (int)x->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
        if (x->status == USB_TRANSFER_STATUS_COMPLETED && n > 0)
            rc = hidm_parse(x->data_buffer + sizeof(usb_setup_packet_t), n, &layout);
        ESP_LOGI(TAG, "report descriptor: %d bytes, %s", n, rc == 0 ? "mouse found" : "no X and Y");
    }
    usb_host_transfer_free(x);
out:
    return rc;
}

/* Найти загрузочный интерфейс мыши и его входную конечную точку. */
static int find_mouse(const usb_config_desc_t *cfg, uint16_t *mps)
{
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    int off = 0, i;

    while ((d = usb_parse_next_descriptor_of_type(d, cfg->wTotalLength,
                                                  USB_B_DESCRIPTOR_TYPE_INTERFACE, &off))) {
        const usb_intf_desc_t *in = (const usb_intf_desc_t *)d;
        if (in->bInterfaceClass != USB_CLASS_HID || in->bAlternateSetting != 0 ||
            in->bInterfaceSubClass != HID_SUBCLASS_BOOT ||
            in->bInterfaceProtocol != HID_PROTO_MOUSE)
            continue;
        for (i = 0; i < in->bNumEndpoints; i++) {
            int eoff = off;
            const usb_ep_desc_t *e =
                usb_parse_endpoint_descriptor_by_index(in, i, cfg->wTotalLength, &eoff);
            if (e && USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_INTR &&
                USB_EP_DESC_GET_EP_DIR(e)) {
                intf = in->bInterfaceNumber;
                ep   = e->bEndpointAddress;
                *mps = USB_EP_DESC_GET_MPS(e);
                return 0;
            }
        }
    }
    return -1;
}

static void open_mouse(uint8_t addr)
{
    const usb_config_desc_t *cfg;
    uint16_t mps = 0;

    if (dev) return;                        /* одна мышь; вторую не берём */
    if (usb_host_device_open(client, addr, &dev) != ESP_OK) { dev = 0; return; }
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK ||
        find_mouse(cfg, &mps) != 0) {
        ESP_LOGI(TAG, "device %u is not a boot-protocol mouse, ignored", addr);
        usb_host_device_close(client, dev);
        dev = 0;
        return;
    }
    if (usb_host_interface_claim(client, dev, intf, 0) != ESP_OK) {
        ESP_LOGW(TAG, "cannot claim interface %u", intf);
        usb_host_device_close(client, dev);
        dev = 0;
        return;
    }
    /* SET_IDLE вправе отвергнуть, SET_PROTOCOL у загрузочного подкласса
     * обязан понимать; не понял -- работаем как есть. Протокол отчётов (1) --
     * если раскладка нашлась в описателе, иначе загрузочный (0). */
    by_desc = read_layout() == 0;
    if (by_desc) {
        if (hid_request(HID_SET_PROTOCOL, 1) != ESP_OK)
            ESP_LOGW(TAG, "SET_PROTOCOL(report) refused, reading reports as they come");
        ESP_LOGI(TAG, "report protocol: id %u, buttons %u, X %u bits, Y %u bits, wheel %u bits",
                 layout.id, layout.btn_n, layout.x_size, layout.y_size, layout.w_size);
    } else if (hid_request(HID_SET_PROTOCOL, 0) != ESP_OK)
        ESP_LOGW(TAG, "SET_PROTOCOL(boot) refused, reading reports as they come");
    hid_request(HID_SET_IDLE, 0);

    if (usb_host_transfer_alloc(mps, 0, &in_xfer) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for the interrupt transfer");
        return;
    }
    in_xfer->num_bytes        = mps;
    in_xfer->device_handle    = dev;
    in_xfer->bEndpointAddress = ep;
    in_xfer->callback         = in_cb;
    if (usb_host_transfer_submit(in_xfer) != ESP_OK)
        ESP_LOGW(TAG, "interrupt transfer not submitted");
    ESP_LOGI(TAG, "mouse on address %u, interface %u, endpoint 0x%02x, %u bytes",
             addr, intf, ep, mps);
}

static void close_mouse(void)
{
    usb_device_handle_t d = dev;

    dev = 0;                                /* in_cb больше не перезапускает */
    /* Посылку в полёте снимаем остановкой конечной точки: освобождать её и
     * отпускать интерфейс можно, только когда в полёте ничего нет. */
    usb_host_endpoint_halt(d, ep);
    usb_host_endpoint_flush(d, ep);
    usb_host_endpoint_clear(d, ep);
    usb_host_client_handle_events(client, pdMS_TO_TICKS(20));
    if (in_xfer) { usb_host_transfer_free(in_xfer); in_xfer = 0; }
    usb_host_interface_release(client, d, intf);
    usb_host_device_close(client, d);
    if (board_features() & BF_MOUSE) combo_cmd(C_MOUSE_OFF);
    ESP_LOGI(TAG, "mouse gone");
}

static void host_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

/* ПРИСУТСТВИЕ -- РАЗ В СЕКУНДУ. Мышь в ПЛИС «есть» с первого отчёта и до
 * C_MOUSE_OFF, а перезапуск ПЛИС (перезаливка, включение) это забывает: до
 * первого движения порт D4h не отвечает. Программа, которая проверила мышь
 * на старте в это время, решала, что мыши нет, и переподключение не помогало
 * (помогал только ВВОД+БЛК после того, как мышь сдвинули). Поэтому, пока мышь открыта, раз в секунду -- отчёт без движения с
 * текущими кнопками: зажатую кнопку он не отпускает, а присутствие
 * восстанавливает. Отчёты идут из этой же задачи (in_cb зовётся из
 * handle_events), так что с ними не пересекается. */
static void client_task(void *arg)
{
    TickType_t beat = 0;

    (void)arg;
    for (;;) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(100));
        if (new_addr) {
            uint8_t a = new_addr;
            new_addr = 0;
            last_btn = 0;
            open_mouse(a);
            beat = xTaskGetTickCount() - pdMS_TO_TICKS(1000);   /* сразу */
        }
        if (gone) { if (gone == dev) close_mouse(); gone = 0; }
        if (dev && (TickType_t)(xTaskGetTickCount() - beat) >= pdMS_TO_TICKS(1000)) {
            beat = xTaskGetTickCount();
            if (board_features() & BF_MOUSE) combo_mouse(last_btn, 0, 0, 0);
        }
    }
}

void mouse_start(void)
{
    const usb_host_config_t host = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    usb_host_client_config_t cc = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_cb, .callback_arg = 0 },
    };

    if (usb_host_install(&host) != ESP_OK) { ESP_LOGE(TAG, "USB host not installed"); return; }
    xTaskCreate(host_task, "usbhost", 3072, 0, 3, 0);
    if (usb_host_client_register(&cc, &client) != ESP_OK) {
        ESP_LOGE(TAG, "USB client not registered");
        return;
    }
    ctl_done = xSemaphoreCreateBinary();
    usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 8, 0, &ctl_xfer);
    xTaskCreate(client_task, "mouse", 4096, 0, 2, 0);
}
