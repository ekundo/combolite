#include "jtag.h"

/* Путь по автомату TAP записан здесь ОДИН раз, и все обращения им пользуются.
 * Иначе пришлось бы повторять «два такта туда, один сюда» в каждой команде, и
 * первая же опечатка выглядела бы как молчащий кристалл. */

static int pulse(const jtag_io *io, int tms, int tdi)
{
    return io->pulse(io->ctx, tms, tdi);
}

void jtag_reset(const jtag_io *io)
{
    int i;
    for (i = 0; i < 5; i++) pulse(io, 1, 0);   /* -> Test-Logic-Reset */
    pulse(io, 0, 0);                            /* -> Run-Test/Idle */
}

/* Сдвиг одного регистра. Отличие IR от DR ровно в одном такте по дороге:
 * Select-DR-Scan -> Select-IR-Scan. Поэтому обе команды — одна функция с
 * признаком, а не две похожие, которые разъедутся при первой же правке. */
static void shift(const jtag_io *io, int is_ir,
                  const uint8_t *out, uint8_t *in, int nbits)
{
    int i;

    if (nbits <= 0) return;

    pulse(io, 1, 0);                    /* Run-Test/Idle -> Select-DR-Scan */
    if (is_ir) pulse(io, 1, 0);         /*             -> Select-IR-Scan */
    pulse(io, 0, 0);                    /*             -> Capture-?R */
    pulse(io, 0, 0);                    /*             -> Shift-?R */

    for (i = 0; i < nbits; i++) {
        int bit = out ? (out[i >> 3] >> (i & 7)) & 1 : 0;
        /* ПОСЛЕДНИЙ бит уходит вместе с TMS=1: он и сдвигается, и уводит в
           Exit1. Отдельного такта на выход нет — лишний такт сдвинул бы
           регистр на разряд, и данные приехали бы смещёнными. */
        int tdo = pulse(io, i == nbits - 1, bit);
        if (in) {
            if (tdo) in[i >> 3] |=  (uint8_t)(1u << (i & 7));
            else     in[i >> 3] &= (uint8_t)~(1u << (i & 7));
        }
    }

    pulse(io, 1, 0);                    /* Exit1-?R -> Update-?R */
    pulse(io, 0, 0);                    /*          -> Run-Test/Idle */
}

void jtag_ir(const jtag_io *io, uint8_t ir)
{
    shift(io, 1, &ir, 0, 8);
}

void jtag_dr(const jtag_io *io, const uint8_t *out, uint8_t *in, int nbits)
{
    shift(io, 0, out, in, nbits);
}

/* Таблица кодов. Держится здесь, а не у вызывающего: ее читают и морда по
 * сети, и самопроверка, и разошлись бы они молча. Откуда взяты числа и почему
 * имя — перечень, а не один прибор, написано в jtag.h. */
const char *jtag_device_name(uint32_t idcode)
{
    static const struct { uint32_t id; const char *name; } known[] = {
        { JTAG_IDCODE_LCMXO2_2000HC, "LCMXO2-2000HC / LCMXO2-1200UHC" },
        { JTAG_IDCODE_LCMXO2_4000HC, "LCMXO2-4000HC / LCMXO2-2000UHC" },
        { JTAG_IDCODE_LCMXO2_7000HE, "LCMXO2-7000HE / LCMXO2-7000ZE"  },
        { JTAG_IDCODE_LCMXO2_7000HC, "LCMXO2-7000HC / LCMXO2-4000UHC" },
    };
    unsigned i;

    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (known[i].id == idcode)
            return known[i].name;
    return 0;
}

uint32_t jtag_idcode(const jtag_io *io)
{
    uint8_t buf[4] = {0, 0, 0, 0};

    jtag_reset(io);
    jtag_dr(io, 0, buf, 32);

    return (uint32_t)buf[0]        | (uint32_t)buf[1] << 8
         | (uint32_t)buf[2] << 16  | (uint32_t)buf[3] << 24;
}
