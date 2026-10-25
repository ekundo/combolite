/* Wi-Fi модем: AT-команды и telnet. Описание -- modem.h. */

#include "modem.h"
#include "bbslist.h"
#include "fetch.h"
#include "netcompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DIAL_TIMEOUT_MS   20000u
#define GUARD_MS          1000u        /* тишина вокруг +++ */

/* ---------------------------------------------------------- трубы */
/* Каждая труба -- кольцо с одним писателем и одним читателем: на плате
 * modem_put и modem_get зовёт задача канала, а modem_poll -- задача модема.
 * Индексы растут без конца, размер -- степень двойки. */
#define RING 4096                       /* справка и список BBS -- больше 1 КБ разом */
typedef struct {
    uint8_t           b[RING];
    volatile uint32_t w, r;
} ring_t;

static ring_t from_vec;                 /* машина -> модем */
static ring_t to_vec;                   /* модем -> машина */
static uint8_t  net_out[512];           /* модем -> сеть, ещё не ушло */
static int      net_out_n;

static int ring_put(ring_t *q, uint8_t b)
{
    if (q->w - q->r >= RING) return 0;
    q->b[q->w % RING] = b;
    __sync_synchronize();
    q->w++;
    return 1;
}

static int ring_get(ring_t *q, uint8_t *b)
{
    if (q->w == q->r) return 0;
    *b = q->b[q->r % RING];
    __sync_synchronize();
    q->r++;
    return 1;
}

static int ring_room(const ring_t *q) { return RING - (int)(q->w - q->r); }

void modem_put(uint8_t b) { ring_put(&from_vec, b); }
int  modem_get(uint8_t *b) { return ring_get(&to_vec, b); }

static void say(const char *s)
{
    while (*s) ring_put(&to_vec, (uint8_t)*s++);
}

static void result(const char *word)
{
    say("\r\n"); say(word); say("\r\n");
}

/* ---------------------------------------------------------- состояние */
enum { M_CMD, M_DIAL, M_ONLINE };

static int      mode = M_CMD;
static int      online_cmd;             /* соединение есть, но мы в командах (+++) */
static int      echo = 1;
static int      sock = -1;
static uint32_t dial_ms;
static char     line[80];               /* строка команды */
static int      line_n;
static int      cmd_esc;                /* ESC-последовательность в строке команды: мимо */
static char     last_cmd[80];           /* для A/ */
static char     last_dial[80];          /* для ATDL */
/* Вид соединения -- по первому байту от сервера: telnet начинает с IAC, IRC и
 * прочий сырой TCP -- с текста. Сервер молчит CONN_WAIT_MS -- сырой: IRC ждёт
 * NICK и USER, telnet-BBS торгуется сразу. Пока вид не ясен, байты от машины
 * ждут: telnet и сырой TCP шлют их по-разному (IAC IAC, CR NUL). */
#define CONN_WAIT_MS 1000u
enum { C_UNKNOWN, C_TELNET, C_RAW, C_GOPHER };
static int      conn;
static uint32_t conn_ms;

/* +++ в соединении */
static uint32_t last_in_ms;             /* последний байт от машины */
static int      plus_n;
static uint32_t plus_ms;

/* Мост «Gopher поверх HTTPS»: соединение без сокета, байты -- из fetch.h. */
static int      gopher;                 /* 0 -- нет, 1 -- копим строку выбора, 2 -- качаем */
static char     g_host[64];
static char     g_sel[256];
static int      g_sel_n;

static int line_up(void) { return sock >= 0 || gopher; }

int modem_online(void) { return line_up() && mode == M_ONLINE; }
int modem_line(void) { return line_up(); }

static void hang_up(void)
{
    if (gopher) fetch_close();
    gopher = 0;
    if (sock >= 0) net_close(sock);
    sock = -1;
    net_out_n = 0;
    online_cmd = 0;
    mode = M_CMD;
}

void modem_init(void)
{
    hang_up();
    echo = 1;
    line_n = 0;
    cmd_esc = 0;
    plus_n = 0;
}

/* ---------------------------------------------------------- telnet */
#define T_IAC   255
#define T_DONT  254
#define T_DO    253
#define T_WONT  252
#define T_WILL  251
#define T_SB    250
#define T_SE    240
#define O_BINARY 0
#define O_ECHO   1
#define O_SGA    3
#define O_TTYPE  24
#define O_NAWS   31

static uint8_t us_on[32], him_on[32];    /* по разряду на опцию */
static int echo_esc;                    /* местное эхо: 0 -- текст, 1 -- после ESC, 2 -- внутри ESC [ */
static int  bit(const uint8_t *m, int o) { return (m[o >> 3] >> (o & 7)) & 1; }
static void setb(uint8_t *m, int o, int v)
{
    if (v) m[o >> 3] |= (uint8_t)(1 << (o & 7));
    else   m[o >> 3] &= (uint8_t)~(1 << (o & 7));
}

static void net_raw(uint8_t b)
{
    if (net_out_n < (int)sizeof net_out) net_out[net_out_n++] = b;
}

static void cmd3(uint8_t verb, uint8_t opt)
{
    net_raw(T_IAC); net_raw(verb); net_raw(opt);
}

static void send_naws(void)
{
    static const uint8_t sb[] = { T_IAC, T_SB, O_NAWS, 0, MODEM_COLS, 0, MODEM_ROWS,
                                  T_IAC, T_SE };
    size_t i;
    for (i = 0; i < sizeof sb; i++) net_raw(sb[i]);
}

static void send_ttype(void)
{
    static const char name[] = "ANSI";
    size_t i;
    net_raw(T_IAC); net_raw(T_SB); net_raw(O_TTYPE); net_raw(0);   /* IS */
    for (i = 0; i < sizeof name - 1; i++) net_raw((uint8_t)name[i]);
    net_raw(T_IAC); net_raw(T_SE);
}

/* BINARY -- в обе стороны: без него сервер может портить CR в файлах (XMODEM). */
static int we_can(int o)  { return o == O_SGA || o == O_TTYPE || o == O_NAWS || o == O_BINARY; }
static int he_may(int o)  { return o == O_ECHO || o == O_SGA || o == O_BINARY; }

/* Ответ меняет состояние и уходит только при смене: так вежливые стороны
 * не зацикливаются на подтверждениях (RFC 854, раздел о согласовании). */
static void tn_verb(uint8_t verb, uint8_t o)
{
    switch (verb) {
    case T_DO:
        if (we_can(o)) {
            if (!bit(us_on, o)) { setb(us_on, o, 1); cmd3(T_WILL, o); }
            if (o == O_NAWS) send_naws();
        } else cmd3(T_WONT, o);
        break;
    case T_DONT:
        if (bit(us_on, o)) { setb(us_on, o, 0); cmd3(T_WONT, o); }
        break;
    case T_WILL:
        if (he_may(o)) {
            if (!bit(him_on, o)) { setb(him_on, o, 1); cmd3(T_DO, o); }
        } else cmd3(T_DONT, o);
        break;
    default:   /* WONT */
        if (bit(him_on, o)) { setb(him_on, o, 0); cmd3(T_DONT, o); }
        break;
    }
}

static void binary(void);

enum { TS_DATA, TS_IAC, TS_VERB, TS_SB, TS_SB_IAC, TS_CR };
static int     ts = TS_DATA;
static uint8_t ts_verb, sb_opt;
static int     sb_n, sb_send;

static void tn_reset(void)
{
    conn = C_UNKNOWN;
    echo_esc = 0;
    memset(us_on, 0, sizeof us_on);
    memset(him_on, 0, sizeof him_on);
    ts = TS_DATA;
}

/* Байт из сети. Команды telnet вырезаются, CR NUL становится CR. */
static void tn_in(uint8_t b)
{
    if (conn == C_UNKNOWN) {
        conn = b == T_IAC ? C_TELNET : C_RAW;
        if (conn == C_TELNET) binary();
    }
    if (conn == C_RAW) { ring_put(&to_vec, b); return; }
    switch (ts) {
    case TS_DATA:
        if (b == T_IAC) ts = TS_IAC;
        else { ring_put(&to_vec, b); if (b == '\r' && !bit(him_on, O_BINARY)) ts = TS_CR; }
        break;
    case TS_CR:
        ts = TS_DATA;
        if (b == 0 && !bit(him_on, O_BINARY)) break;     /* в BINARY NUL после CR -- данные */
        tn_in(b);
        break;
    case TS_IAC:
        if (b == T_IAC) { ring_put(&to_vec, b); ts = TS_DATA; }
        else if (b >= T_WILL) { ts_verb = b; ts = TS_VERB; }
        else if (b == T_SB) { ts = TS_SB; sb_n = 0; sb_send = 0; }
        else ts = TS_DATA;            /* NOP, GA и прочие одиночные */
        break;
    case TS_VERB:
        tn_verb(ts_verb, b);
        ts = TS_DATA;
        break;
    case TS_SB:
        if (b == T_IAC) { ts = TS_SB_IAC; break; }
        if (sb_n == 0) sb_opt = b;
        else if (sb_n == 1 && sb_opt == O_TTYPE && b == 1) sb_send = 1;   /* SEND */
        sb_n++;
        break;
    default:   /* TS_SB_IAC */
        if (b == T_SE) {
            if (sb_send) send_ttype();
            ts = TS_DATA;
        } else ts = TS_SB;            /* IAC IAC внутри SB -- данные */
        break;
    }
}

/* Попросить BINARY в обе стороны, как только видно, что это telnet: файлы
 * (XMODEM) идут через модем как есть, а модем о них не знает. Биты ставятся
 * сразу: ответ согласия тогда не повторяет просьбу, а отказ их снимет. */
static void binary(void)
{
    if (!bit(us_on, O_BINARY)) { setb(us_on, O_BINARY, 1); cmd3(T_WILL, O_BINARY); }
    if (!bit(him_on, O_BINARY)) { setb(him_on, O_BINARY, 1); cmd3(T_DO, O_BINARY); }
}

/* Местное эхо, как у telnet-клиента: пока сервер не обещал эхо (WILL ECHO),
 * набранное машине показывает сам модем -- иначе на MUD вроде bylins.su,
 * которые эха не шлют, не видно, что набираешь. Сервер включил своё (перед
 * паролем, чтобы его скрыть) -- модем молчит. ВК -- переводом строки, ЗБ --
 * стиранием знака; последовательности ESC (стрелки, F1-F5) не показываются. */
static void local_echo(uint8_t b)
{
    if (echo_esc == 1) { echo_esc = (b == '[' || b == 'O') ? 2 : 0; return; }
    if (echo_esc == 2) { if (b >= 0x40 && b <= 0x7E) echo_esc = 0; return; }
    if (b == 27) { echo_esc = 1; return; }
    if (b == '\r') { ring_put(&to_vec, '\r'); ring_put(&to_vec, '\n'); return; }
    if (b == 8 || b == 127) { ring_put(&to_vec, 8); ring_put(&to_vec, ' '); ring_put(&to_vec, 8); return; }
    if (b >= 32) ring_put(&to_vec, b);
}

/* Байт от машины в сеть. */
static void gopher_char(uint8_t b);

static void tn_out(uint8_t b)
{
    if (conn == C_GOPHER) { gopher_char(b); return; }
    if (conn == C_RAW) { net_raw(b); return; }
    if (conn == C_TELNET && !bit(him_on, O_ECHO)) local_echo(b);
    if (b == T_IAC) { net_raw(T_IAC); net_raw(T_IAC); return; }
    net_raw(b);
    if (b == '\r' && !bit(us_on, O_BINARY)) net_raw(0);
}

/* ---------------------------------------------------------- мост Gopher
 * GitHub Pages отдаёт только HTTPS, а GOPHER.COM ходит сырым TCP на порт 70.
 * Набор хоста *.github.io на порт 70 соединяет не с сетью, а с мостом:
 * CONNECT сразу, строка выбора -- до CR или LF (запрос поиска после ТАБ --
 * мимо: там статика), файл -- https://хост/путь (пустой путь или с «/» на
 * конце -- index.gph), его байты -- как ответ сервера Gopher, конец --
 * NO CARRIER, как при закрытии TCP. Не вышло -- меню из одной строки-ошибки.
 * Скачивание -- fetch.h: на плате esp_http_client, на маке curl. */
static int gopher_host(const char *host, const char *port)
{
    static const char tail[] = ".github.io";
    size_t hl = strlen(host), tl = sizeof tail - 1;
    return !strcmp(port, "70") && hl > tl && !strcasecmp(host + hl - tl, tail);
}

static void gopher_end(const char *error)
{
    if (error) {
        say("3"); say(error); say("\t\terror.host\t1\r\n.\r\n");
    }
    hang_up();
    result("NO CARRIER");
}

/* Строка выбора пришла: путь -> адрес. В адресе остаются только буквы, цифры
 * и -._~/%, прочее -- процентами: так он годится и в оболочку (curl). */
static void gopher_start(void)
{
    static const char hex[] = "0123456789ABCDEF";
    char url[512];
    size_t n;
    int i;

    n = (size_t)snprintf(url, sizeof url, "https://%s", g_host);
    if (g_sel[0] != '/') url[n++] = '/';
    for (i = 0; i < g_sel_n && g_sel[i] != '\t' && n < sizeof url - 16; i++) {
        uint8_t c = (uint8_t)g_sel[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            strchr("-._~/%", c)) url[n++] = (char)c;
        else { url[n++] = '%'; url[n++] = hex[c >> 4]; url[n++] = hex[c & 15]; }
    }
    if (url[n - 1] == '/') { memcpy(url + n, "index.gph", 9); n += 9; }
    url[n] = 0;
    if (fetch_open(url) != 0) { gopher_end("Bridge busy, try again"); return; }
    gopher = 2;
}

static void gopher_char(uint8_t b)
{
    if (gopher != 1) return;            /* строка выбора уже ушла: остальное мимо */
    if (b == '\r' || b == '\n') { gopher_start(); return; }
    if (g_sel_n < (int)sizeof g_sel - 1) g_sel[g_sel_n++] = (char)b;
}

/* Скачанное -- машине, сколько влезет. */
static void gopher_pump(void)
{
    uint8_t buf[256];
    int n = ring_room(&to_vec) - 16, i;

    if (n > (int)sizeof buf) n = (int)sizeof buf;
    if (n <= 0) return;
    n = fetch_read(buf, n);
    if (n > 0) { for (i = 0; i < n; i++) ring_put(&to_vec, buf[i]); return; }
    if (n == FETCH_DONE) gopher_end(0);
    else if (n == FETCH_FAIL) gopher_end(fetch_status() ? "Not found" : "Cannot reach the site");
}

/* ---------------------------------------------------------- набор */
static void dial(const char *arg, uint32_t now_ms)
{
    char host[64], port[8] = "23";
    const char *c;
    struct addrinfo hints, *ai = 0;
    int rc;

    while (*arg == ' ') arg++;
    if (arg != last_dial) snprintf(last_dial, sizeof last_dial, "%s", arg);
    c = strrchr(arg, ':');
    if (c) {
        size_t hl = (size_t)(c - arg);
        if (hl == 0 || hl >= sizeof host || strlen(c + 1) >= sizeof port) { result("ERROR"); return; }
        memcpy(host, arg, hl); host[hl] = 0;
        strcpy(port, c + 1);
    } else {
        if (!*arg || strlen(arg) >= sizeof host) { result("ERROR"); return; }
        strcpy(host, arg);
    }
    if (gopher_host(host, port)) {      /* мост: сеть -- за fetch.h */
        snprintf(g_host, sizeof g_host, "%s", host);
        g_sel_n = 0;
        gopher = 1;
        tn_reset();
        conn = C_GOPHER;
        mode = M_ONLINE;
        last_in_ms = now_ms;
        result("CONNECT");
        return;
    }
    net_start();
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0 || !ai) { result("NO ANSWER"); return; }
    sock = net_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) { freeaddrinfo(ai); result("ERROR"); return; }
    net_nonblock(sock);
    rc = connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc != 0 && !net_again()) { hang_up(); result("NO ANSWER"); return; }
    tn_reset();
    dial_ms = now_ms;
    mode = M_DIAL;
}

/* ---------------------------------------------------------- справка и BBS */
static const char help[] =
    "\r\nAT commands (the line ends with Return):\r\n"
    "  ATDT host[:port]   dial, port 23 by default\r\n"
    "  ATBBS [word]       BBS list from the Telnet BBS Guide: search by name,\r\n"
    "                     address, software or place\r\n"
    "  ATDS n             dial BBS number n from the list\r\n"
    "  ATDL               dial the last address again\r\n"
    "  +++                back to commands, the line stays (1 s pause around)\r\n"
    "  ATO                back to the line       ATH   hang up\r\n"
    "  ATE0, ATE1         echo off, on           ATI   modem info\r\n"
    "  ATZ                reset                  A/    repeat the last command\r\n"
    "  AT?                this help";

#define BBS_SHOW 15

static void bbs_line(const bbs_t *b, void *arg)
{
    char hp[80], tail[80], out[200];
    int room;
    (void)arg;
    snprintf(hp, sizeof hp, "%s:%d", b->host, b->port);
    room = 79 - (4 + 1 + 24 + 1 + (int)strlen(hp) + 1);
    if (room < 0) room = 0;
    snprintf(tail, sizeof tail, "%s%s%s", b->software, *b->software && *b->location ? ", " : "", b->location);
    snprintf(out, sizeof out, "\r\n%4d %-24.24s %s %.*s", b->num, b->name, hp, room, tail);
    say(out);
}

static void cmd_bbs(const char *word, uint32_t now_ms)
{
    char buf[120];
    int n;
    while (*word == ' ') word++;
    say("\r\nThe Telnet BBS Guide, www.telnetbbsguide.com ...");
    n = bbs_fetch(now_ms);
    if (n < 0) { say("\r\nNo list: "); say(bbs_error()); result("ERROR"); return; }
    if (!*word) {
        snprintf(buf, sizeof buf, "\r\n%d BBS, issue %s. ATBBS word -- search, ATDS n -- dial.", n, bbs_issue());
        say(buf);
        result("OK");
        return;
    }
    n = bbs_search(word, BBS_SHOW, bbs_line, 0);
    if (!n) say("\r\nNothing found.");
    else if (n > BBS_SHOW) {
        snprintf(buf, sizeof buf, "\r\n... and %d more, a longer word narrows the search.", n - BBS_SHOW);
        say(buf);
    }
    result("OK");
}

static void cmd_dial_stored(const char *p, uint32_t now_ms)
{
    char host[64], arg[80];
    int port;
    if (bbs_fetch(now_ms) < 0) { say("\r\nNo list: "); say(bbs_error()); result("ERROR"); return; }
    if (!bbs_get(atoi(p), host, sizeof host, &port)) { result("ERROR"); return; }
    snprintf(arg, sizeof arg, "%s:%d", host, port);
    say("\r\nDialing "); say(arg);
    dial(arg, now_ms);
}

/* ---------------------------------------------------------- команды */
static void command(uint32_t now_ms)
{
    char *p = line;

    line[line_n] = 0;
    line_n = 0;
    while (*p == ' ') p++;
    if (!*p) return;
    if ((p[0] != 'A' && p[0] != 'a') || (p[1] != 'T' && p[1] != 't')) { result("ERROR"); return; }
    snprintf(last_cmd, sizeof last_cmd, "%s", p);
    p += 2;
    while (*p) {
        char c = *p++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        switch (c) {
        case ' ': break;
        case 'Z': hang_up(); echo = 1; break;
        case 'E': echo = (*p == '1'); if (*p == '0' || *p == '1') p++; break;
        case 'H': if (*p == '0') p++; hang_up(); break;
        case 'I': if (*p >= '0' && *p <= '9') p++;
                  say("\r\n" MODEM_NAME); break;
        case 'O': if (!line_up()) { result("NO CARRIER"); return; }
                  online_cmd = 0; mode = M_ONLINE; result("CONNECT"); return;
        case 'D': if (line_up()) { hang_up(); say("\r\nHung up the old line"); }   /* новый набор -- старое прочь */
                  if (*p == 'S' || *p == 's') { cmd_dial_stored(p + 1, now_ms); return; }
                  if (*p == 'L' || *p == 'l') {
                      if (!last_dial[0]) { result("ERROR"); return; }
                      say("\r\nDialing "); say(last_dial);
                      dial(last_dial, now_ms); return;
                  }
                  if (*p == 'T' || *p == 't' || *p == 'P' || *p == 'p') p++;
                  dial(p, now_ms); return;
        case '?': say(help); result("OK"); return;
        case 'B': if (!strncasecmp(p, "BS", 2)) { cmd_bbs(p + 2, now_ms); return; }
                  result("ERROR"); return;
        default:  result("ERROR"); return;
        }
    }
    result("OK");
}

/* Знак строки команды. Эхо, если ATE1; ЗБ стирает; A/ -- повторить прошлую
 * команду, как у Hayes. ESC-последовательности (стрелки терминала) -- мимо. */
static void cmd_char(uint8_t b, uint32_t now_ms)
{
    if (cmd_esc == 1) { cmd_esc = (b == '[' || b == 'O') ? 2 : 0; return; }
    if (cmd_esc == 2) { if (b >= 0x40 && b <= 0x7E) cmd_esc = 0; return; }
    if (b == 27) { cmd_esc = 1; return; }
    if (b == '\r') {
        if (echo) say("\r");
        command(now_ms);
        return;
    }
    if (b == 8 || b == 127) {
        if (!line_n) return;
        line_n--;
        if (echo) say("\b \b");
        return;
    }
    if (b < 0x20 || line_n >= (int)sizeof line - 1) return;
    line[line_n++] = (char)b;
    if (echo) ring_put(&to_vec, b);
    if (line_n == 2 && (line[0] == 'A' || line[0] == 'a') && line[1] == '/') {
        line_n = 0;
        if (!last_cmd[0]) { result("ERROR"); return; }
        line_n = (int)strlen(last_cmd);
        memcpy(line, last_cmd, (size_t)line_n);
        command(now_ms);
    }
}

/* ---------------------------------------------------------- опрос */
static void online_char(uint8_t b, uint32_t now_ms)
{
    if (b == '+' && (plus_n > 0 || now_ms - last_in_ms >= GUARD_MS) && plus_n < 3) {
        if (++plus_n == 3) plus_ms = now_ms;
        last_in_ms = now_ms;
        return;
    }
    while (plus_n) { tn_out('+'); plus_n--; }
    tn_out(b);
    last_in_ms = now_ms;
}

void modem_poll(uint32_t now_ms)
{
    uint8_t b, buf[256];
    int n;

    /* вид соединения: сервер молчит -- сырой TCP */
    if (sock >= 0 && mode != M_DIAL && conn == C_UNKNOWN && now_ms - conn_ms >= CONN_WAIT_MS) conn = C_RAW;
    /* от машины; в соединении -- когда ясен его вид */
    while ((mode != M_ONLINE || (conn != C_UNKNOWN && net_out_n < (int)sizeof net_out - 4))
           && ring_get(&from_vec, &b)) {
        if (mode == M_ONLINE) online_char(b, now_ms);
        else if (mode == M_CMD) cmd_char(b, now_ms);
        /* в наборе байты пропадают, как у настоящего модема */
    }
    if (mode == M_ONLINE && plus_n == 3 && now_ms - plus_ms >= GUARD_MS) {
        plus_n = 0;
        online_cmd = 1;
        mode = M_CMD;
        result("OK");
    }

    if (gopher == 2) gopher_pump();
    if (sock < 0) return;

    if (mode == M_DIAL) {
        if (net_writable(sock)) {
            if (net_error(sock)) { hang_up(); result("NO ANSWER"); return; }
            mode = M_ONLINE;
            last_in_ms = now_ms;
            conn_ms = now_ms;
            result("CONNECT");
        } else if (now_ms - dial_ms >= DIAL_TIMEOUT_MS) {
            hang_up(); result("NO ANSWER"); return;
        }
        if (mode == M_DIAL) return;
    }

    /* в сеть */
    if (net_out_n) {
        n = net_send(sock, net_out, net_out_n);
        if (n > 0) {
            memmove(net_out, net_out + n, (size_t)(net_out_n - n));
            net_out_n -= n;
        } else if (n < 0 && !net_again()) {
            hang_up(); result("NO CARRIER"); return;
        }
    }
    /* из сети: не больше, чем влезет к машине (с запасом на ответы модема) */
    n = ring_room(&to_vec) - 16;
    if (n > (int)sizeof buf) n = (int)sizeof buf;
    if (n <= 0) return;
    n = net_recv(sock, buf, n);
    if (n > 0) {
        int i;
        for (i = 0; i < n; i++) tn_in(buf[i]);
    } else if (n == 0 || !net_again()) {
        hang_up(); result("NO CARRIER");
    }
}
