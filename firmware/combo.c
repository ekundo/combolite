/* Разговор с fpga/hdl/ide_remote.v. */

#include "combo.h"
#include "link.h"

/* Ответ отстаёт на байт: это устройство SPI, байт N выходит, пока входит
 * N+1. Поэтому первый возврат после команды — состояние ПРЕДЫДУЩЕЙ посылки,
 * и полезный ответ забирается со сдвигом. */

uint8_t combo_status(void)
{
    uint8_t s;
    link_select();
    link_xfer(C_STATUS);
    s = link_xfer(0x00);
    link_deselect();
    return s;
}

/* Маска — по числу байт в блоке. Ошибка в ней не видна: лишние номера молча
 * заворачиваются на начало блока, и наверху это выглядит как настоящие
 * показания, например счётчик записей читается как счётчик наблюдателя. */
static uint8_t diag_byte(uint8_t base, uint8_t sel)
{
    uint8_t v;
    link_select();
    link_xfer((uint8_t)(base + (sel & 7)));
    v = link_xfer(0x00);
    link_deselect();
    return v;
}

uint8_t combo_diag(uint8_t sel)  { return diag_byte(C_DIAG,  sel); }
uint8_t combo_diag2(uint8_t sel) { return diag_byte(C_DIAG2, sel); }
uint8_t combo_diag3(uint8_t sel) { return diag_byte(C_DIAG3, sel); }
uint8_t combo_diag4(uint8_t sel) { return diag_byte(C_DIAG4, sel); }
uint8_t combo_diag5(uint8_t sel) { return diag_byte(C_DIAG5, sel); }
uint8_t combo_diag6(uint8_t sel) { return diag_byte(C_DIAG6, sel); }
uint8_t combo_diag7(uint8_t sel) { return diag_byte(C_DIAG7, sel); }

void combo_pal_read(uint8_t pal[17])
{
    int i;
    link_select();
    link_xfer(C_PALRD);
    for (i = 0; i < 17; i++) pal[i] = link_xfer(0x00);
    link_deselect();
}

void combo_pal_log(uint8_t buf[2048])
{
    int i;
    link_select();
    link_xfer(C_PALRD);
    for (i = 0; i < 2048; i++) buf[i] = link_xfer(0x00);
    link_deselect();
}

void combo_diag_clr(void)
{
    link_select();
    link_xfer(C_DIAG_CLR);
    link_deselect();
}

/* Заморозка защёлки адреса на время обращения — опыт, разбор в fpga/hdl/vu_bus.v.
 * Это УРОВЕНЬ в ПЛИС, а не импульс: держится до обратной команды. По сбросу
 * кристалла и по перезаливке битстрима возвращается в ноль, то есть плата
 * всегда поднимается в прежнем, проверенном поведении. */
void combo_latch(int on)
{
    link_select();
    link_xfer(on ? C_LATCH_ON : C_LATCH_OFF);
    link_deselect();
}

/* Гашение такта защёлки на ПРЕДСКАЗАННОМ видеоспаде ~RAS — опыт, разбор в
 * fpga/hdl/vid_phase.v. В отличие от заморозки выше защёлка продолжает тактоваться
 * от процессорных спадов, то есть строка остаётся верной, а не устаревшей.
 * Тоже уровень, тоже ноль по сбросу кристалла. */
void combo_vid(int on)
{
    link_select();
    link_xfer(on ? C_VID_ON : C_VID_OFF);
    link_deselect();
}

/* Кому принадлежит строб машины: предыдущему спаду ~RAS или этому. По времени
 * неопределимо — строб лежит посередине между спадами, — поэтому обе версии
 * переключаются вживую, а решает замер stb_after/stb_before. */
void combo_vid_pol(int b)
{
    link_select();
    link_xfer(b ? C_VID_POLB : C_VID_POLA);
    link_deselect();
}

/* Квалификация выбора кристалла ОЗУ стробом процессора — опыт, разбор в
 * fpga/hdl/kvaz_sram.v. Тоже уровень, тоже ноль по сбросу кристалла. */
void combo_cegate(int on)
{
    link_select();
    link_xfer(on ? C_CE_ON : C_CE_OFF);
    link_deselect();
}

/* Чем квалифицировать выбор кристалла. Строб для этого не годится: его фаза
 * относительно ~RAS плавает в 14 раз, и квалификация по нему ухудшила стековые
 * пары вдвое. ~CAS метит строго подмножество циклов и приходит раньше. */
void combo_ceby(int by_cas)
{
    link_select();
    link_xfer(by_cas ? C_CE_CAS : C_CE_STB);
    link_deselect();
}

int combo_alive(void)
{
    return (combo_status() & ST_ALIVE) != 0;
}

int combo_wait(uint8_t mask, int want, uint32_t timeout_us, uint8_t *last)
{
    uint32_t waited = 0;
    for (;;) {
        uint8_t s = combo_status();
        if (last) *last = s;
        if (!!(s & mask) == !!want) return 0;
        if (waited >= timeout_us) return -1;
        link_delay_us(10);
        waited += 10;
    }
}

int combo_request(void)
{
    link_select();
    link_xfer(C_REQ);
    link_deselect();
    /* Арбитр ждёт тишины на шине «Вектора»; боевой порог 50 мс, поэтому
     * ждём с запасом — процессор может как раз молотить по приводу. */
    return combo_wait(ST_GRANT, 1, 500000, 0);
}

int combo_request_long(void)
{
    link_select();
    link_xfer(C_REQ_LONG);
    link_deselect();
    return combo_wait(ST_GRANT, 1, 500000, 0);
}

void combo_release(void)
{
    link_select();
    link_xfer(C_REL);
    link_deselect();
}

void combo_reset_drive(void)
{
    link_select();
    link_xfer(C_RESET);
    link_deselect();
    /* Линию держит счётчик в ПЛИС, 2500 тактов при 50 МГц. Ждём с запасом,
     * чтобы следующая команда не пришла в перезапускающийся привод. */
    link_delay_us(100);
}

int combo_reg_write(uint8_t reg, uint16_t value)
{
    link_select();
    link_xfer(C_REG_WR | (reg & 7));
    link_xfer((uint8_t)(value & 0xFF));
    link_xfer((uint8_t)(value >> 8));
    link_deselect();
    return combo_wait(ST_CYC, 0, 10000, 0);
}

int combo_reg_read(uint8_t reg, uint16_t *out)
{
    uint8_t lo, hi;

    link_select();
    link_xfer(C_REG_RD | (reg & 7));
    link_deselect();
    if (combo_wait(ST_CYC, 0, 10000, 0) < 0) return -1;

    link_select();
    link_xfer(C_RES);
    lo = link_xfer(0x00);
    hi = link_xfer(0x00);
    link_deselect();

    if (out) *out = (uint16_t)(lo | (hi << 8));
    return 0;
}

void combo_buf_read(uint8_t *dst)
{
    link_select();
    link_xfer(C_BUF_RD);
    link_xfer_block(0, dst, COMBO_SECTOR);
    link_deselect();
}

/* Буфер назван ЯВНО, а не взят из владения. Владение меняется по концу
   переноса, то есть в момент, которого прошивка не выбирает; номер же приезжает
   в самой команде и смениться под ней не может. */
void combo_buf_read_n(int buf, uint8_t *dst)
{
    link_select();
    link_xfer((uint8_t)(C_BUF_RD_N | (buf & 1)));
    link_xfer_block(0, dst, COMBO_SECTOR);
    link_deselect();
}

void combo_buf_write(const uint8_t *src)
{
    link_select();
    link_xfer(C_BUF_WR);
    link_xfer_block(src, 0, COMBO_SECTOR);
    link_deselect();
}

/* Пустить перенос и НЕ ждать. Разделено надвое ради конвейера: пока ПЛИС
 * тянет сектор с привода в свободный буфер, ESP успевает выбрать по SPI
 * предыдущий из второго. Кто из буферов какой — распоряжается ПЛИС, здесь об
 * этом знать нечего (см. шапку fpga/hdl/ide_remote.v). */
void combo_block_start(int write)
{
    link_select();
    link_xfer(write ? C_SEC_WR : C_SEC_RD);
    link_deselect();
}

void combo_block_start_n(int buf)
{
    link_select();
    link_xfer((uint8_t)(C_SEC_RD_N | (buf & 1)));
    link_deselect();
}

/* 256 циклов по 740 нс это меньше 200 мкс; берём вдесятеро. */
int combo_block_wait(void) { return combo_wait(ST_BLK, 0, 2000, 0); }

void combo_seen_clear(void)
{
    link_select();
    link_xfer(C_SEEN_CLR);
    link_deselect();
}

int combo_cpu_touched(void) { return (combo_status() & ST_CPU_SEEN) != 0; }

static int block(int write)
{
    combo_block_start(write);
    return combo_block_wait();
}

int combo_sector_from_drive(void) { return block(0); }
int combo_sector_to_drive(void)   { return block(1); }

void combo_cmd(uint8_t cmd)
{
    link_select();
    link_xfer(cmd);
    link_deselect();
}

/* Аргумент -- в той же выборке. Поднятый между ними CS сбросил бы фазу
 * разбора в ПЛИС, и аргумент ушёл бы как новая команда. */
void combo_cmd_arg(uint8_t cmd, uint8_t arg)
{
    link_select();
    link_xfer(cmd);
    link_xfer(arg);
    link_deselect();
}

void combo_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    link_select();
    link_xfer(C_MOUSE);
    link_xfer(buttons);
    link_xfer((uint8_t)dx);
    link_xfer((uint8_t)dy);
    link_xfer((uint8_t)wheel);
    link_deselect();
}

/* Ответ отстаёт на байт: первый байт карты приходит за командой, как и у
 * диагностики. */
void combo_dirty(uint8_t map[32])
{
    link_select();
    link_xfer(C_DIRTY);
    link_xfer_block(0, map, 32);
    link_deselect();
}

void combo_scr_read(uint8_t out[131])
{
    link_select();
    link_xfer(C_SCRRD);
    link_xfer_block(0, out, 131);
    link_deselect();
}

void combo_scr_peek(uint8_t *ready, uint8_t *row)
{
    link_select();
    link_xfer(C_SCRRD);
    *ready = link_xfer(0x00);
    *row   = link_xfer(0x00);
    link_deselect();
}
