/* Канал «Вектор» <-> ESP: модем или кадры настроек. Протокол -- vchan.h. */

#include "vchan.h"
#include "board.h"
#include "combo.h"
#include "modem.h"

#include <string.h>

/* ---------------------------------------------------------------- разбор */
enum { R_SYNC, R_SEQ, R_OP, R_LEN, R_DATA, R_SUM };

static int      st = R_SYNC;
static uint8_t  seq, op, len, pos, sum;
static uint8_t  data[VCHAN_MAX];
static uint32_t last_ms;

/* Ответы копятся здесь, пока в очереди ПЛИС нет места. Двух кадров хватает:
 * «Вектор» ждёт ответа на каждый запрос, прежде чем слать следующий. */
#define TXQ 128
static uint8_t txq[TXQ];
static int     tx_head, tx_len;

/* Режим канала: модем, пока сразу после очистки не пришла строка VCHAN_CL. */
static const char cl_word[] = VCHAN_CL;
static int      frames;                 /* 1 -- кадры настроек */
static int      fresh = 1;              /* очистка была, VCHAN_CL ещё может прийти */
static int      held;                   /* сколько знаков VCHAN_CL уже совпало */

void vchan_reset(void) { st = R_SYNC; frames = 0; fresh = 1; held = 0; }
int  vchan_frames(void) { return frames; }

static void tx_put(uint8_t b)
{
    if (tx_len >= TXQ) return;          /* переполнение: «Вектор» не забирает */
    txq[(tx_head + tx_len) % TXQ] = b;
    tx_len++;
}

static void reply(uint8_t status, const uint8_t *body, int n)
{
    uint8_t s, l = (uint8_t)(n + 1), o = (uint8_t)(op | 0x80);
    int i;

    s = (uint8_t)(seq + o + l + status);
    tx_put(VCHAN_SYNC); tx_put(seq); tx_put(o); tx_put(l); tx_put(status);
    for (i = 0; i < n; i++) { tx_put(body[i]); s = (uint8_t)(s + body[i]); }
    tx_put(s);
}

static void fields(const board_settings_t *s, uint8_t f[VF_COUNT])
{
    f[VF_DEV_OFF]   = s->dev_off;
    f[VF_FDC_RDY]   = s->fdc_rdy;
    f[VF_FDC_PC]    = s->fdc_pc;
    f[VF_VGA60]     = s->vga60;
    f[VF_VI53_OFF]  = s->vi53_off;
    f[VF_BOOT]      = s->boot;
    f[VF_FDD_CABLE] = s->fdd_cable;
    f[VF_AUTOSTART] = s->autostart;
}

static void set_field(board_settings_t *s, int id, uint8_t v)
{
    switch (id) {
    case VF_DEV_OFF:   s->dev_off   = v; break;
    case VF_FDC_RDY:   s->fdc_rdy   = v; break;
    case VF_FDC_PC:    s->fdc_pc    = v; break;
    case VF_VGA60:     s->vga60     = v; break;
    case VF_VI53_OFF:  s->vi53_off  = v; break;
    case VF_BOOT:      s->boot      = v; break;
    case VF_FDD_CABLE: s->fdd_cable = v; break;
    default:           s->autostart = v; break;
    }
}

static void handle(void)
{
    board_settings_t s;
    uint8_t f[VF_COUNT], info[5], diag[1 + SCR_PAL_BYTES + 1];
    int i, status = VS_OK;

    if (op == VOP_DIAG) {
        /* Что TERM.COM вернёт ОС на выходе: регистр квазидиска видит ПЛИС
         * (0x38), палитру -- журнал кадра. Без палитры (старый битстрим) --
         * чёрный и белый, как у консоли. */
        if (len) { reply(VS_LEN, 0, 0); return; }
        diag[0] = combo_diag(0);
        if (board_palette(diag + 1) != 0)
            for (i = 0; i < 16; i++) diag[1 + i] = (i & 6) ? 0xF6 : 0x00;
        diag[17] = (uint8_t)(modem_online() != 0);  /* прошлая программа не положила трубку */
        reply(VS_OK, diag, 18);
        return;
    }
    board_get(&s);
    switch (op) {
    case VOP_INFO:
        if (len) { reply(VS_LEN, 0, 0); return; }
        info[0] = VCHAN_PROTO; info[1] = VF_COUNT;
        info[2] = board_features(); info[3] = board_features2();
        info[4] = (uint8_t)(board_present() != 0);
        reply(VS_OK, info, 5);
        return;
    case VOP_GET:
        if (len) { reply(VS_LEN, 0, 0); return; }
        break;
    case VOP_SET:
        if (len == 0 || (len & 1)) { status = VS_LEN; break; }
        for (i = 0; i < len; i += 2)
            if (data[i] >= VF_COUNT) { status = VS_FIELD; break; }
        if (status != VS_OK) break;
        for (i = 0; i < len; i += 2) set_field(&s, data[i], data[i + 1]);
        if (board_set(&s) != 0) status = VS_VALUE;
        board_get(&s);
        break;
    default:
        reply(VS_OP, 0, 0);
        return;
    }
    fields(&s, f);
    reply((uint8_t)status, f, VF_COUNT);
}

static void frame_rx(uint8_t b, uint32_t now_ms)
{
    if (st != R_SYNC && (uint32_t)(now_ms - last_ms) > VCHAN_IDLE_MS) st = R_SYNC;
    last_ms = now_ms;

    switch (st) {
    case R_SYNC: if (b == VCHAN_SYNC) st = R_SEQ; break;
    case R_SEQ:  seq = b; sum = b; st = R_OP; break;
    case R_OP:   op = b; sum = (uint8_t)(sum + b); st = R_LEN; break;
    case R_LEN:
        len = b; sum = (uint8_t)(sum + b); pos = 0;
        if (len > VCHAN_MAX) { st = R_SYNC; break; }   /* не кадр: ищем A5h дальше */
        st = len ? R_DATA : R_SUM;
        break;
    case R_DATA:
        data[pos++] = b; sum = (uint8_t)(sum + b);
        if (pos == len) st = R_SUM;
        break;
    default:
        st = R_SYNC;
        if (b != sum) reply(VS_SUM, 0, 0);
        else          handle();
        break;
    }
}

/* Байт с «Вектора». Сразу после очистки строка VCHAN_CL переводит канал в
 * кадры; пока она совпадает, байты придерживаются, разошлась -- уходят модему
 * все. Так перехват работает и посреди соединения, а модем без CL.COM не
 * видит разницы: «AT*CL» у него -- обычная неизвестная команда. */
void vchan_rx(uint8_t b, uint32_t now_ms)
{
    int i;
    if (frames) { frame_rx(b, now_ms); return; }
    if (fresh) {
        if (b == (uint8_t)cl_word[held]) {
            if (cl_word[++held]) return;
            frames = 1; fresh = 0; held = 0; st = R_SYNC;
            return;
        }
        for (i = 0; i < held; i++) modem_put((uint8_t)cl_word[i]);
        fresh = 0; held = 0;
    }
    modem_put(b);
}

int vchan_tx_pending(void) { return tx_len; }

int vchan_tx_take(uint8_t *out, int max)
{
    int n = 0;
    while (n < max && tx_len) {
        out[n++] = txq[tx_head];
        tx_head = (tx_head + 1) % TXQ;
        tx_len--;
    }
    if (!frames)                        /* модем: его ответы и данные из сети */
        while (n < max && modem_get(&out[n])) n++;
    return n;
}

/* ---------------------------------------------------------------- обмен */
int vchan_poll(uint32_t now_ms)
{
    uint8_t buf[CHAN_DEPTH];
    int n, i, free, flushed, moved;

    n = combo_chan_read(buf, (int)sizeof buf, &free, &flushed);
    if (n < 0) return 0;
    /* Очистка с «Вектора»: начатый кадр и неотданные ответы -- от прежней
     * программы, канал -- снова модем. Байты, взятые этим же опросом, пришли
     * уже после очистки. Соединение модема и его неотданное не трогаются:
     * терминал, вернувшись, продолжит с того же места. */
    if (flushed) { vchan_reset(); tx_len = 0; }
    moved = n > 0;
    for (i = 0; i < n; i++) vchan_rx(buf[i], now_ms);
    if (free > 0) {
        n = vchan_tx_take(buf, free < (int)sizeof buf ? free : (int)sizeof buf);
        if (n) { combo_chan_write(buf, n); moved = 1; }
    }
    return moved ? 2 : 1;
}
