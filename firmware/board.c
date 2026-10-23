/* Узлы ПЛИС помимо привода, см. board.h. */

#include "board.h"
#include "combo.h"
#include "link.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

static int     present;
static uint8_t feat, feat2;
static int     has_pal;            /* блок 5, байт 2, разряд 6: есть C_PALRD */
static board_settings_t cur = BOARD_SETTINGS_DEFAULT;

static int      was_alive;
static int      vvod_was_held;
static uint32_t keys_cmd_ms;        /* когда мы сами жали клавишу */
static volatile int reapply_flag;

int board_settings_valid(const board_settings_t *s)
{
    return !(s->dev_off & ~DEV_MASK) && s->fdc_rdy <= 2 && s->fdc_pc <= 0x1F
        && s->vga60 <= 1 && s->vi53_off <= 1 && s->boot <= BOOT_VVOD
        && s->fdd_cable <= 1 && s->autostart <= 1;
}

/* Раскладка блоков 4 и 5 -- версия 1 в старшей тетраде байта 0x6E. Старый
 * битстрим отвечает на неизвестный код состоянием, а у состояния разряд 7
 * всегда единица (ST_ALIVE), то есть тетрада не меньше 8. Висящая линия даёт
 * 0xFF, прижатая -- 0x00: тоже мимо. */
int board_probe(void)
{
    uint8_t b6 = combo_diag5(6), b7 = combo_diag5(7);

    present = (b6 >> 4) == 1;
    feat  = present ? b7 : 0;
    feat2 = present ? (uint8_t)(b6 & 0x0F) : 0;
    has_pal = present && (feat & BF_VIDEO) && (combo_diag5(2) & 0x40);
    return present ? 0 : -1;
}

int     board_present(void)   { return present; }
uint8_t board_features(void)  { return feat; }
uint8_t board_features2(void) { return feat2; }

/* ТОЛЬКО ПРИ ОПОЗНАННОМ БИТСТРИМЕ. Битстрим, не знающий команд с аргументом,
 * принял бы аргумент за следующую команду: маска 0x30 стала бы запросом шины,
 * 0x32 -- сбросом привода. Блоки 4 и 5 появились позже всех этих команд,
 * поэтому их наличие и служит пропуском. */
static void apply(const board_settings_t *s)
{
    if (!present) return;
    combo_cmd_arg(C_DEV, s->dev_off & DEV_MASK);
    /* Разряд 2 байта -- шлейф ПК. Битстрим, который его не знает, берёт только
     * разряды 1..0, так что посылать можно всем. */
    combo_cmd_arg(C_FDC_RDY, (uint8_t)(s->fdc_rdy | (s->fdd_cable ? 4 : 0)));
    combo_cmd_arg(C_FDC_PC, s->fdc_pc);
    combo_cmd_arg(C_VGA, s->vga60 & 1);
    combo_cmd(s->vi53_off ? C_VI53_OFF : C_VI53_ON);
    /* Разряд 1 -- автозапуск загруженного (BOOT26 и ПЗУ). Битстрим до него
     * берёт только разряд 0 и после BOOT26 жмёт СБР всегда. */
    if (feat2 & BF2_KBD)
        combo_cmd_arg(C_BOOT, (uint8_t)((s->boot == BOOT_VVOD) | (s->autostart ? 2 : 0)));
}

void board_init(void)
{
    board_settings_t s;

    if (board_settings_load(&s) == 0 && board_settings_valid(&s)) cur = s;
    board_probe();
    apply(&cur);
    was_alive = combo_alive();
    vvod_was_held = board_vvod_held();
}

void board_get(board_settings_t *out) { *out = cur; }

int board_set(const board_settings_t *s)
{
    if (!board_settings_valid(s)) return -1;
    cur = *s;
    apply(&cur);
    board_settings_save(&cur);
    return 0;
}

void board_reapply(void)
{
    if (board_probe() == 0) apply(&cur);
}

void board_reapply_later(void) { reapply_flag = 1; }

int board_keys(int what)
{
    if (!(feat & BF_KEYS)) return -1;
    combo_cmd_arg(C_KEYS, (uint8_t)(what & 3));
    keys_cmd_ms = link_ms();
    return 0;
}

int board_vvod_held(void)
{
    return (feat & BF_KEYS) && (combo_diag4(0) & 0x10);
}

/* Между проверкой и командой удержание может истечь само, и тогда команда 3
 * нажмёт ВВОД заново. Машина в этот миг только что вышла из сброса и ничего не
 * успела -- она просто стартует на 2 с позже. Закрыть щель целиком можно лишь
 * в ПЛИС («продлить, только если нажат»), а цена ошибки -- одна задержка. */
int board_vvod_extend(void)
{
    if (!(feat & BF_KEYS)) return -1;
    if (!board_vvod_held()) return 0;
    combo_cmd_arg(C_KEYS, 3);
    keys_cmd_ms = link_ms();
    return 1;
}

int board_loader(int what)
{
    if (!(feat & BF_LOADER)) return -1;
    combo_cmd_arg(C_LOADER, (uint8_t)(what & 3));
    return 0;
}

int board_release(void)
{
    if (cur.boot != BOOT_ROM && board_loader(LOADER_BOOT) == 0) {
        keys_cmd_ms = link_ms();
        return 1;
    }
    return board_keys(KEY_RELEASE);
}

/* ПОРЯДОК СТРОК -- ЧЕРЕЗ ВОСЕМЬ. Буфер строки в ПЛИС один, и заявленная строка
 * ловится, когда выборка до неё дойдёт (scr_buf.v): строка у неё идёт за
 * ~70 мкс, кадр -- за 17-20 мс. Пока ESP читает 131 байт на 4 МГц, выборка
 * уходит на четыре-пять строк, и заявка следующей подряд опоздала бы на целый
 * кадр: 256 кадров, пять секунд. Строка через восемь ещё впереди, так что за
 * кадр проходит каждая восьмая, а весь экран -- за восемь кадров. */
#define SCR_STRIDE  8
/* Не пришла за ~100 мс, пять кадров, -- выборка стоит. Срок в опросах, а не в
 * миллисекундах: опрос с паузой -- около 35 мкс. */
#define SCR_POLLS   3000

int board_screen(uint8_t *frame)
{
    uint8_t buf[3 + SCR_ROW_BYTES], ready, row;
    int pass, y;

    if (!(feat & BF_VIDEO)) return -1;
    for (pass = 0; pass < SCR_STRIDE; pass++)
        for (y = pass; y < SCR_ROWS; y += SCR_STRIDE) {
            int polls = 0;
            combo_cmd_arg(C_SCRROW, (uint8_t)y);
            for (;;) {
                combo_scr_peek(&ready, &row);
                if (ready == 0xA5 && row == y) break;
                if (++polls > SCR_POLLS) return -2;
                link_delay_us(20);
            }
            combo_scr_read(buf);
            if (buf[0] != 0xA5 || buf[1] != y) return -2;
            memcpy(frame + (size_t)y * SCR_ROW_BYTES, buf + 3, SCR_ROW_BYTES);
        }
    return 0;
}

int board_palette(uint8_t pal[SCR_PAL_BYTES])
{
    if (!has_pal) return -1;
    combo_pal_read(pal);
    return 0;
}

int board_pal_log(uint8_t buf[PAL_LOG_BYTES])
{
    if (!has_pal) return -1;
    combo_pal_log(buf);
    return 0;
}

void board_diag(uint8_t d4[8], uint8_t d5[8])
{
    int i;
    for (i = 0; i < 8; i++) {
        d4[i] = combo_diag4((uint8_t)i);
        d5[i] = combo_diag5((uint8_t)i);
    }
}

void board_diag6(uint8_t d6[8])
{
    int i;
    for (i = 0; i < 8; i++)
        d6[i] = (feat2 & BF2_KBD) ? combo_diag6((uint8_t)i) : 0;
}

/* ПЛИС ожила после молчания -- значит её перезалили или она загрузилась
 * заново, и настройки в ней снова умолчательные. Второй признак -- ВВОД,
 * нажатый не нами: так ПЛИС делает только с включения (fpga/hdl/autostart.v). Он
 * ловит перезаливку в фоновом режиме, когда ПЛИС молчит считанные миллисекунды
 * и шаг раз в секунду этого не заметил бы. */
void board_tick(void)
{
    int alive = combo_alive(), held = 0, restarted = 0;

    if (alive && present) {
        held = board_vvod_held();
        if (held && !vvod_was_held && (uint32_t)(link_ms() - keys_cmd_ms) > 3000)
            restarted = 1;
    }
    if (alive && (!was_alive || restarted || reapply_flag)) {
        reapply_flag = 0;
        board_reapply();
    }
    was_alive = alive;
    vvod_was_held = held;
}

/* --------------------------------------------------------------- разбор */

static const char *tf(int v) { return v ? "true" : "false"; }

/* Сброс с клавиатуры, блок 6 (fpga/hdl/rom_sense.v): ПЗУ или ОЗУ ответило на
 * первые чтения после сброса. first -- первые пять байт последнего сброса;
 * после своего ВВОД это и есть подпись ПЗУ. */
static int kbd_json(char *out, int max)
{
    static const char *const kinds[4] = { "unknown", "own", "not_vvod", "vvod" };
    uint8_t d6[8];

    board_diag6(d6);
    return snprintf(out, (size_t)max,
        ",\"kbd\":{\"rom_known\":%s,\"kind\":\"%s\",\"own_vvod\":%s,"
        "\"pending\":%s,\"reading\":%s,\"rom_live\":%s,\"same\":%u,\"events\":%u,"
        "\"vvod\":%u,\"first\":\"%02x%02x%02x%02x%02x\"},"
        "\"diag6\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"",
        tf(d6[0] & 0x80), kinds[d6[0] & 3], tf(d6[0] & 0x20),
        tf(d6[0] & 0x40), tf(d6[0] & 0x10), tf(d6[0] & 0x04), d6[1], d6[2] & 15, d6[2] >> 4,
        d6[3], d6[4], d6[5], d6[6], d6[7],
        d6[0], d6[1], d6[2], d6[3], d6[4], d6[5], d6[6], d6[7]);
}

/* ВГ93, блок 7 (fpga/hdl/fdc.v, dbg): последняя команда, состояние, дорожка и
 * сектор, и счётчики по кругу -- байты данных из 0x18, найденные метки,
 * импульсы данных / 256, индексы. Битстрим без этого блока отвечает байтом
 * состояния: тогда все восемь байт одинаковы и не меньше 0x80. */
static int fdc_json(char *out, int max)
{
    uint8_t d7[8];
    int i;

    for (i = 0; i < 8; i++) d7[i] = combo_diag7((uint8_t)i);
    return snprintf(out, (size_t)max,
        ",\"fdc\":{\"cmd\":%u,\"status\":%u,\"track\":%u,\"sector\":%u,"
        "\"data_reads\":%u,\"marks\":%u,\"pulses_256\":%u,\"index\":%u},"
        "\"diag7\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"",
        d7[0], d7[1], d7[2], d7[3], d7[4], d7[5], d7[6], d7[7],
        d7[0], d7[1], d7[2], d7[3], d7[4], d7[5], d7[6], d7[7]);
}

/* Сколько дописать, чтобы не выйти за max: snprintf отдаёт длину, которая
 * получилась бы, и следующий кусок надо писать уже не туда. */
static int room(int n, int max) { return n < max ? max - n : 0; }

int board_json(char *out, int max)
{
    uint8_t d4[8], d5[8];
    int n;

    if (!present)
        return snprintf(out, (size_t)max,
            "{\"present\":false,\"settings\":{\"dev_off\":%u,\"fdc_rdy\":%u,"
            "\"fdc_pc\":%u,\"vga60\":%u,\"vi53\":%s,\"boot\":%u,"
            "\"fdd_cable\":%u,\"autostart\":%u}}\n",
            cur.dev_off, cur.fdc_rdy, cur.fdc_pc, cur.vga60, tf(!cur.vi53_off),
            cur.boot, cur.fdd_cable, cur.autostart);

    board_diag(d4, d5);
    n = snprintf(out, (size_t)max,
        "{\"present\":true,\"layout\":%u,"
        "\"features\":{\"fdc\":%s,\"vi53\":%s,\"mouse\":%s,\"keys\":%s,"
        "\"loader\":%s,\"mirror\":%s,\"watch\":%s,\"video\":%s,\"ay\":%s,"
        "\"kvaz2\":%s,\"audio_split\":%s,\"kbd_reset\":%s,\"palette\":%s},"
        "\"settings\":{\"dev_off\":%u,\"fdc_rdy\":%u,\"fdc_pc\":%u,"
        "\"vga60\":%u,\"vi53\":%s,\"boot\":%u,\"fdd_cable\":%u,\"autostart\":%u},"
        "\"keys\":{\"vvod\":%s,\"sbr\":%s},"
        "\"loader\":{\"window\":%s,\"armed\":%s},"
        "\"video\":{\"raster_ok\":%s,\"vid_lock\":%s,\"vga60\":%s,"
        "\"lock_good\":%s,\"late_lines\":%u,\"fetch_aborts\":%u,"
        "\"journal_over\":%u,\"lock_err\":%d,\"slip_v\":%d,\"slip_h\":%d,"
        "\"scroll\":%u,\"cal_s\":%u,\"addr_ref\":%s,\"col_before0\":%u,"
        "\"line\":%u,\"rows\":%u,\"pll_ok\":%s},"
        "\"mirror_miss\":%u,"
        "\"fdd\":{\"ready34\":%s,\"write\":%s,\"protect\":%s,\"track0\":%s,"
        "\"index\":%s,\"motor\":%s,\"ds1\":%s,\"ds0\":%s},"
        "\"diag4\":\"%02x%02x%02x%02x%02x%02x%02x%02x\","
        "\"diag5\":\"%02x%02x%02x%02x%02x%02x%02x%02x\"",
        d5[6] >> 4,
        tf(feat & BF_FDC), tf(feat & BF_VI53), tf(feat & BF_MOUSE),
        tf(feat & BF_KEYS), tf(feat & BF_LOADER), tf(feat & BF_MIRROR),
        tf(feat & BF_WATCH), tf(feat & BF_VIDEO), tf(feat2 & BF2_AY),
        tf(feat2 & BF2_KVAZ2), tf(feat2 & BF2_AUDIO_SPLIT), tf(feat2 & BF2_KBD), tf(has_pal),
        cur.dev_off, cur.fdc_rdy, cur.fdc_pc, cur.vga60, tf(!cur.vi53_off), cur.boot,
        cur.fdd_cable, cur.autostart,
        tf(d4[0] & 0x10), tf(d4[0] & 0x20),
        tf(d4[0] & 0x80), tf(d4[0] & 0x40),
        tf(d4[0] & 0x01), tf(d4[0] & 0x02), tf(d4[0] & 0x04), tf(d4[0] & 0x08),
        d4[1], d4[2], d4[3], (int8_t)d4[4], (int8_t)d4[5], (int8_t)d4[6],
        d5[0], d5[1], tf(d5[2] & 0x80), d5[2] & 0x1F, d5[3], d5[5], tf(d5[2] & 0x20),
        d4[7],
        tf(d5[4] & 0x80), tf(d5[4] & 0x40), tf(d5[4] & 0x20), tf(d5[4] & 0x10),
        tf(d5[4] & 0x08), tf(d5[4] & 0x04), tf(d5[4] & 0x02), tf(d5[4] & 0x01),
        d4[0], d4[1], d4[2], d4[3], d4[4], d4[5], d4[6], d4[7],
        d5[0], d5[1], d5[2], d5[3], d5[4], d5[5], d5[6], d5[7]);
    if (feat2 & BF2_KBD) n += kbd_json(out + (n < max ? n : max), room(n, max));
    if (feat & BF_FDC)   n += fdc_json(out + (n < max ? n : max), room(n, max));
    n += snprintf(out + (n < max ? n : max), (size_t)room(n, max), "}\n");
    return n;
}

/* ------------------------------------------------------------ хранение */

#ifdef ESP_PLATFORM

#define NS  "board"
#define KEY "cfg"
#define VER 4

/* Первый байт -- версия записи: поля прибавятся, и запись старой прошивки
 * должна читаться как «не было», а не разъезжаться по полям. Старые версии
 * берём, недостающие поля -- по умолчанию, иначе обновление сбросило бы хозяину
 * все настройки: версия 1 -- без boot, версия 2 -- без fdd_cable, версия 3 --
 * без autostart (берётся включённым: так плата вела себя до неё). */
int board_settings_load(board_settings_t *out)
{
    const size_t v1 = offsetof(board_settings_t, boot);
    const size_t v2 = offsetof(board_settings_t, fdd_cable);
    const size_t v3 = offsetof(board_settings_t, autostart);
    board_settings_t d = BOARD_SETTINGS_DEFAULT;
    nvs_handle_t h;
    uint8_t b[1 + sizeof *out];
    size_t n = sizeof b;
    int rc = -1;

    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    if (nvs_get_blob(h, KEY, b, &n) == ESP_OK) {
        if (n == sizeof b && b[0] == VER) {
            memcpy(out, b + 1, sizeof *out);
            rc = 0;
        }
        else if ((n == 1 + v1 && b[0] == 1) || (n == 1 + v2 && b[0] == 2)
                 || (n == 1 + v3 && b[0] == 3)) {
            memcpy(&d, b + 1, n - 1);
            *out = d;
            rc = 0;
        }
    }
    nvs_close(h);
    return rc;
}

int board_settings_save(const board_settings_t *s)
{
    nvs_handle_t h;
    uint8_t b[1 + sizeof *s];
    esp_err_t e;

    b[0] = VER;
    memcpy(b + 1, s, sizeof *s);
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    e = nvs_set_blob(h, KEY, b, sizeof b);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

#else

/* Без ESP-IDF -- в памяти: флеша нет. */
static board_settings_t saved;
static int have_saved;

int board_settings_load(board_settings_t *out)
{
    if (!have_saved) return -1;
    *out = saved;
    return 0;
}

int board_settings_save(const board_settings_t *s)
{
    saved = *s;
    have_saved = 1;
    return 0;
}

#endif
