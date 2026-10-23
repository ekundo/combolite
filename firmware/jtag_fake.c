/* Заглушка JTAG: у модуля ESP32-S3 на плате нет проводов к JTAG ПЛИС
 * (выводы 26..32 заняты флешем модуля), а ПЛИС прошивается по SSPI (sspi.h).
 *
 * Здесь честный отказ вместо выдумывания правдоподобного ответа: «всё хорошо»
 * там, где железа нет, выглядело бы исправностью.
 */
#include "jtag.h"
#include "xvc.h"

int jtag_probe(uint32_t *id)
{
    if (id) *id = 0;
    return JTAG_ENOHW;
}

int jtag_check_pins(jtag_pins *p)
{
    (void)p;
    return JTAG_ENOHW;
}

int jtag_scan(int *pin, int *up, int *down, int *n)
{
    (void)pin; (void)up; (void)down;
    if (n) *n = 0;
    return JTAG_ENOHW;
}

int jtag_hold(int tck, int tms, int tdi, int secs)
{
    (void)tck; (void)tms; (void)tdi; (void)secs;
    return JTAG_ENOHW;
}

int jtag_step(int tms, int tdi, int clocks, int secs)
{
    (void)tms; (void)tdi; (void)clocks; (void)secs;
    return JTAG_ENOHW;
}

void jtag_speed(int half_us)
{
    (void)half_us;
}

int jtag_bridges(int m[4][4])
{
    (void)m;
    return JTAG_ENOHW;
}

int jtag_xvc_open(uint32_t peer, int secs)
{
    (void)peer; (void)secs;
    return JTAG_ENOHW;
}
