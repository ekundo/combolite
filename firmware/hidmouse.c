/* Разбор описателя отчётов USB-мыши. Описание -- hidmouse.h.
 *
 * Описатель -- поток коротких элементов: байт приставки (тег, тип, длина 0, 1,
 * 2 или 4) и данные. Длинные элементы (FEh) мышам не нужны и пропускаются.
 * Глобальные элементы (страница, размер, число, номер отчёта) живут до смены,
 * локальные (Usage, Usage Min/Max) -- до ближайшего главного. Главный Input
 * отводит report_count полей по report_size разрядов; им по порядку достаются
 * назначения из локальных, а когда список кончился -- последнее (HID 1.11,
 * 6.2.2.8). Push/Pop мышам не встречаются, но разобраны: иначе после Pop
 * размер был бы чужой. */

#include "hidmouse.h"

#include <string.h>

#define PAGE_DESKTOP 0x01
#define PAGE_BUTTON  0x09
#define U_X          0x30
#define U_Y          0x31
#define U_WHEEL      0x38

#define MAX_USAGE 16
#define MAX_IDS   16
#define MAX_STACK 4

typedef struct { uint16_t page; uint8_t id; uint32_t size, count; } glob_t;

/* Поле, найденное в отчёте: с какого разряда и сколько. */
typedef struct { int found; uint8_t id; uint16_t off; uint8_t size; } field_t;

static uint32_t item_u(const uint8_t *p, int len)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < len; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

int hidm_parse(const uint8_t *d, int n, hidm_t *m)
{
    glob_t g = { 0, 0, 0, 0 }, stack[MAX_STACK];
    int sp = 0;
    uint32_t usage[MAX_USAGE];      /* страница << 16 | назначение */
    int nu = 0;
    uint32_t umin = 0, umax = 0;
    int have_range = 0;
    uint8_t ids[MAX_IDS];
    uint16_t bits[MAX_IDS];         /* занято разрядов в отчёте с этим номером */
    int nid = 0;
    field_t x = { 0, 0, 0, 0 }, y = x, w = x, b = x;
    int b_n = 0;
    int i = 0;

    memset(m, 0, sizeof *m);
    while (i < n) {
        uint8_t pre = d[i];
        int len = pre & 3, type = (pre >> 2) & 3, tag = pre >> 4, k;
        uint32_t v;
        if (len == 3) len = 4;
        if (pre == 0xFE) {                      /* длинный элемент */
            if (i + 1 >= n) break;
            i += 3 + d[i + 1];
            continue;
        }
        if (i + 1 + len > n) break;
        v = item_u(d + i + 1, len);
        i += 1 + len;

        if (type == 1) {                        /* глобальный */
            switch (tag) {
            case 0x0: g.page = (uint16_t)v; break;
            case 0x7: g.size = v; break;
            case 0x8: g.id = (uint8_t)v; break;
            case 0x9: g.count = v; break;
            case 0xA: if (sp < MAX_STACK) stack[sp++] = g; break;
            case 0xB: if (sp > 0) g = stack[--sp]; break;
            default: break;
            }
        } else if (type == 2) {                 /* локальный */
            /* Назначение в один-два байта -- на текущей странице, в четыре --
               со своей страницей в старшем слове. */
            uint32_t u = len == 4 ? v : ((uint32_t)g.page << 16 | v);
            switch (tag) {
            case 0x0: if (nu < MAX_USAGE) usage[nu++] = u; break;
            case 0x1: umin = u; have_range = 1; break;
            case 0x2: umax = u; have_range = 1; break;
            default: break;
            }
        } else if (type == 0) {                 /* главный */
            if (tag == 0x8) {                   /* Input */
                int slot;
                for (slot = 0; slot < nid && ids[slot] != g.id; slot++) ;
                if (slot == nid) {
                    if (nid == MAX_IDS) return -1;
                    ids[nid] = g.id; bits[nid] = 0; nid++;
                }
                if (!(v & 1) && g.size && g.size <= 32) {   /* не константа */
                    for (k = 0; k < (int)g.count; k++) {
                        uint32_t u;
                        uint16_t off = (uint16_t)(bits[slot] + k * g.size);
                        if (k < nu) u = usage[k];
                        else if (have_range && umin + (uint32_t)(k - nu) <= umax)
                            u = umin + (uint32_t)(k - nu);
                        else if (nu) u = usage[nu - 1];
                        else continue;
                        if ((u >> 16) == PAGE_BUTTON && g.size == 1) {
                            if (!b.found) { b.found = 1; b.id = g.id; b.off = off; b.size = 1; }
                            if (b.id == g.id && off == b.off + b_n) b_n++;
                        } else if ((u >> 16) == PAGE_DESKTOP) {
                            field_t *f = (u & 0xFFFF) == U_X ? &x : (u & 0xFFFF) == U_Y ? &y
                                       : (u & 0xFFFF) == U_WHEEL ? &w : 0;
                            if (f && !f->found) {
                                f->found = 1; f->id = g.id; f->off = off; f->size = (uint8_t)g.size;
                            }
                        }
                    }
                }
                bits[slot] = (uint16_t)(bits[slot] + g.count * g.size);
            }
            /* Input, Output, Feature, Collection, End Collection -- все
               главные сбрасывают локальные. */
            nu = 0; umin = umax = 0; have_range = 0;
        }
    }
    if (!x.found || !y.found || x.id != y.id || x.size < 2 || x.size > 16 || y.size < 2 || y.size > 16)
        return -1;
    m->id = x.id;
    m->x_off = x.off; m->x_size = x.size;
    m->y_off = y.off; m->y_size = y.size;
    if (b.found && b.id == x.id) { m->btn_off = b.off; m->btn_n = (uint8_t)(b_n > 3 ? 3 : b_n); }
    if (w.found && w.id == x.id && w.size >= 2 && w.size <= 16) { m->w_off = w.off; m->w_size = w.size; }
    return 0;
}

/* Поле со знаком из отчёта: разряды младшим вперёд, как в HID. */
static int field(const uint8_t *r, int nbits, uint16_t off, uint8_t size)
{
    int32_t v = 0;
    int i;
    if (off + size > nbits) return 0;
    for (i = 0; i < size; i++) {
        int bit = off + i;
        if (r[bit >> 3] >> (bit & 7) & 1) v |= (int32_t)1 << i;
    }
    if (v & ((int32_t)1 << (size - 1))) v -= (int32_t)1 << size;
    return (int)v;
}

static int clamp(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

int hidm_decode(const hidm_t *m, const uint8_t *r, int n,
                uint8_t *btn, int *dx, int *dy, int *wheel)
{
    int nbits, i;
    if (m->id) {
        if (n < 1 || r[0] != m->id) return -1;
        r++; n--;
    }
    nbits = n * 8;
    if (m->x_off + m->x_size > nbits || m->y_off + m->y_size > nbits) return -1;
    *btn = 0;
    for (i = 0; i < m->btn_n; i++) {
        int bit = m->btn_off + i;
        if (bit < nbits && (r[bit >> 3] >> (bit & 7) & 1)) *btn |= (uint8_t)(1 << i);
    }
    *dx = clamp(field(r, nbits, m->x_off, m->x_size));
    *dy = clamp(field(r, nbits, m->y_off, m->y_size));
    *wheel = m->w_size ? clamp(field(r, nbits, m->w_off, m->w_size)) : 0;
    return 0;
}
