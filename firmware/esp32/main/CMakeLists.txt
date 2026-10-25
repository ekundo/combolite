# Прошивка ESP32-S3. К ПЛИС модуль ходит по SSPI; выводы 26..32 у S3 заняты
# флешем модуля, поэтому JTAG проводами нет и XVC отвечает честным отказом
# (jtag_fake.c). Мышь -- через USB OTG.
idf_component_register(
    SRCS "../../main_esp.c"
         "../../wifi.c"
         "../../eslog.c"
         "../../ota.c"
         "../../netcfg.c"
         "../../manifest.c"
         "../../update.c"
         "../../roam.c"
         "../../net.c"
         "../../http.c"
         "../../serve.c"
         "../../ata.c"
         "../../ata_fake.c"
         "../../combo.c"
         "../../board.c"
         "../../vchan.c"
         "../../vchan_esp.c"
         "../../modem.c"
         "../../fetch_esp.c"
         "../../bbslist.c"
         "../../inflate.c"
         "../../modem_esp.c"
         "../../link_espidf.c"
         "../../sha256.c"
         "../../allow.c"
         "../../auth.c"
         "../../espsec.c"
         "../../jtag.c"
         "../../xvc.c"
         "../../sspi.c"
         "../../fpga.c"
         "../../jtag_fake.c"
         "../../mouse_esp.c"
         "../../hidmouse.c"
         "../../sspi_esp.c"
    INCLUDE_DIRS "../.."
    # esp_timer тут не для красоты: link_espidf.c берёт из него монотонные
    # микросекунды, и без объявления его заголовок просто не находится.
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash lwip spiffs console
             esp_timer wpa_supplicant app_update spiffs esp_http_client mbedtls
             usb)

# Каталог www/ уезжает в раздел SPIFFS образом, собранным при сборке проекта.
spiffs_create_partition_image(www ../../../www FLASH_IN_PROJECT)
