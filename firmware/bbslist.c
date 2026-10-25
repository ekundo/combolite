/* Список BBS из Telnet BBS Guide. Описание -- bbslist.h. */

#include "bbslist.h"
#include "inflate.h"
#include "netcompat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char     src_host[64] = "www.telnetbbsguide.com";
static int      src_port = 80;
static uint8_t *zcsv;                   /* сжатый bbslist.csv */
static size_t   zlen;
static char     issue[16];
static int      count;
static uint32_t got_ms;
static int      have;
static const char *err = "";

void bbs_source(const char *host, int port)
{
    snprintf(src_host, sizeof src_host, "%s", host);
    src_port = port;
    have = 0;
}

const char *bbs_error(void) { return err; }
const char *bbs_issue(void) { return issue; }

/* ---------------------------------------------------------- HTTP */
/* GET по HTTP/1.0 (без chunked), тело -- кусками в body(); body вернул не 0 --
 * хватит. 0 -- ответ 200 и тело прочитано (или остановлено), -1 -- нет. */
static int http_get(const char *path, int (*body)(const uint8_t *p, int n, void *arg), void *arg)
{
    struct addrinfo hints, *ai = 0;
    char port[8], req[256], hdr[1024];
    int s, n, hn = 0, in_body = 0, rc = -1;
    uint8_t buf[1024];

    snprintf(port, sizeof port, "%d", src_port);
    net_start();
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(src_host, port, &hints, &ai) != 0 || !ai) { err = "no such host"; return -1; }
    s = net_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s < 0) { freeaddrinfo(ai); err = "no socket"; return -1; }
    net_timeouts(s, 10);
    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
        freeaddrinfo(ai); net_close(s); err = "no connection"; return -1;
    }
    freeaddrinfo(ai);
    n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ComboLite\r\n\r\n",
                 path, src_host);
    if (net_send(s, req, n) != n) { net_close(s); err = "send failed"; return -1; }
    err = "bad reply";
    while ((n = net_recv(s, buf, (int)sizeof buf)) > 0) {
        int i = 0;
        if (!in_body) {
            for (; i < n && !in_body; i++) {
                if (hn < (int)sizeof hdr - 1) hdr[hn++] = (char)buf[i];
                if (hn >= 4 && !memcmp(hdr + hn - 4, "\r\n\r\n", 4)) {
                    hdr[hn] = 0;
                    if (strncmp(hdr, "HTTP/1.", 7) || strncmp(hdr + 9, "200", 3)) { net_close(s); return -1; }
                    in_body = 1;
                }
            }
            if (!in_body) continue;
        }
        if (i < n && body(buf + i, n - i, arg)) { rc = 0; break; }
    }
    if (n == 0 && in_body) rc = 0;
    net_close(s);
    if (rc == 0) err = "";
    else if (in_body) err = "connection broken";
    return rc;
}

/* Имя месячного архива на странице: "ibbs" + 4 цифры + ".zip". */
typedef struct { char tail[16]; int tn; char name[16]; } page_t;

static int page_body(const uint8_t *p, int n, void *arg)
{
    page_t *pg = arg;
    int i, k;
    for (i = 0; i < n; i++) {
        if (pg->tn == (int)sizeof pg->tail) { memmove(pg->tail, pg->tail + 1, sizeof pg->tail - 1); pg->tn--; }
        pg->tail[pg->tn++] = (char)p[i];
        if (pg->tn >= 12 && !memcmp(pg->tail + pg->tn - 12, "ibbs", 4)
            && !memcmp(pg->tail + pg->tn - 4, ".zip", 4)) {
            const char *d = pg->tail + pg->tn - 8;
            for (k = 0; k < 4 && d[k] >= '0' && d[k] <= '9'; k++) ;
            if (k == 4) { memcpy(pg->name, pg->tail + pg->tn - 12, 8); pg->name[8] = 0; return 1; }
        }
    }
    return 0;
}

/* Локальные заголовки ZIP по порядку, до bbslist.csv: 30 байт заголовка, имя,
 * поле extra, данные. */
typedef struct {
    uint8_t  h[30 + 64];
    int      hn, need;              /* заголовок с именем: сколько есть и сколько нужно */
    uint32_t skip;                  /* пропустить: extra и данные чужого файла */
    uint32_t pre;                   /* extra перед данными нашего */
    uint32_t csize;
    uint8_t *data;
    uint32_t got;
    int      done, bad;
} zip_t;

static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }
static unsigned le16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }

static void zip_header(zip_t *z)
{
    unsigned nlen = le16(z->h + 26), xlen = le16(z->h + 28);
    int want = nlen == 11 && !strncasecmp((char *)z->h + 30, "bbslist.csv", 11);
    z->hn = 0;
    z->csize = le32(z->h + 18);
    if (!want) { z->skip = xlen + z->csize; return; }
    if (le16(z->h + 8) != 8 || z->csize == 0 || z->csize > 400000) { z->bad = 1; return; }
    z->data = malloc(z->csize);
    if (!z->data) { z->bad = 1; return; }
    z->pre = xlen;
}

static int zip_body(const uint8_t *p, int n, void *arg)
{
    zip_t *z = arg;
    int i = 0;
    while (i < n) {
        uint32_t left = (uint32_t)(n - i), k;
        if (z->skip) {                          /* чужой файл */
            k = left < z->skip ? left : z->skip;
            z->skip -= k; i += (int)k;
            continue;
        }
        if (z->data) {                          /* наш: сперва extra, потом данные */
            if (z->pre) { k = left < z->pre ? left : z->pre; z->pre -= k; i += (int)k; continue; }
            k = left < z->csize - z->got ? left : z->csize - z->got;
            memcpy(z->data + z->got, p + i, k);
            z->got += k; i += (int)k;
            if (z->got == z->csize) { z->done = 1; return 1; }
            continue;
        }
        z->h[z->hn++] = p[i++];
        if (z->hn == 30) {
            unsigned nlen = le16(z->h + 26);
            if (le32(z->h) != 0x04034B50u || (le16(z->h + 6) & 8) || nlen > 64) { z->bad = 1; return 1; }
            z->need = 30 + (int)nlen;
        }
        if (z->hn >= 30 && z->hn == z->need) {
            zip_header(z);
            if (z->bad) return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------- разбор CSV */
/* Строка CSV -> поля (на месте, кавычки и "" -- по правилам). */
static int csv_fields(char *s, char **f, int max)
{
    int n = 0;
    while (n < max) {
        char *o = s;
        f[n++] = s;
        if (*s == '"') {
            char *r = s + 1;
            for (;;) {
                if (!*r) break;
                if (*r == '"') { if (r[1] == '"') { *o++ = '"'; r += 2; continue; } r++; break; }
                *o++ = *r++;
            }
            while (*r && *r != ',') r++;
            if (*r == ',') { *o = 0; s = r + 1; continue; }
            *o = 0;
            break;
        }
        while (*s && *s != ',') s++;
        if (*s == ',') { *s++ = 0; continue; }
        break;
    }
    return n;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
    return s;
}

typedef struct {
    char line[512];
    int  ln, row;                       /* row 0 -- заголовок */
    int  num;
    int (*each)(const bbs_t *b, void *arg);
    void *arg;
} walk_t;

static int walk_put(uint8_t c, void *arg)
{
    walk_t *w = arg;
    char *f[12];
    bbs_t b;
    int n;
    if (c != '\n') {
        if (w->ln < (int)sizeof w->line - 1) w->line[w->ln++] = (char)c;
        return 0;
    }
    w->line[w->ln] = 0;
    w->ln = 0;
    if (w->row++ == 0) return 0;
    n = csv_fields(w->line, f, 12);
    if (n < 10) return 0;
    b.host = trim(f[3]);
    b.port = atoi(trim(f[4]));
    if (!*b.host) return 0;             /* без telnet -- не набрать */
    if (b.port <= 0 || b.port > 65535) b.port = 23;
    b.num = ++w->num;
    b.name = trim(f[0]);
    b.location = trim(f[7]);
    b.software = trim(f[9]);
    return w->each(&b, w->arg);
}

static int walk(int (*each)(const bbs_t *b, void *arg), void *arg)
{
    walk_t *w = malloc(sizeof *w);
    uint8_t *win = malloc(INFLATE_WINDOW);
    int r = -1;
    if (w && win) {
        memset(w, 0, sizeof *w);
        w->each = each; w->arg = arg;
        r = inflate_raw(zcsv, zlen, win, walk_put, w);
        if (r == 0 && w->ln) walk_put('\n', w);        /* последняя строка без перевода */
    } else err = "out of memory";
    free(w); free(win);
    return r;
}

static int count_each(const bbs_t *b, void *arg) { (void)b; (*(int *)arg)++; return 0; }

/* ---------------------------------------------------------- снаружи */
int bbs_fetch(uint32_t now_ms)
{
    page_t pg;
    zip_t z;
    char path[64];
    int n = 0;

    if (have && (uint32_t)(now_ms - got_ms) < BBS_KEEP_MS) return count;
    memset(&pg, 0, sizeof pg);
    if (http_get("/lists/download-list/", page_body, &pg) != 0 && !pg.name[0]) return -1;
    if (!pg.name[0]) { err = "no list on the page"; return -1; }
    memset(&z, 0, sizeof z);
    snprintf(path, sizeof path, "/bbslist/%s.zip", pg.name);
    http_get(path, zip_body, &z);
    if (!z.done) {
        free(z.data);
        if (!*err || z.bad) err = "no bbslist.csv in the archive";
        return have ? count : -1;       /* старый выпуск лучше, чем ничего */
    }
    free(zcsv);
    zcsv = z.data; zlen = z.csize;
    if (walk(count_each, &n) != 0) { err = "broken archive"; have = 0; return -1; }
    snprintf(issue, sizeof issue, "%s", pg.name);
    count = n;
    got_ms = now_ms;
    have = 1;
    err = "";
    return count;
}

static int has_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; *s; s++) if (!strncasecmp(s, w, n)) return 1;
    return 0;
}

typedef struct {
    const char *word;
    int max, found;
    void (*each)(const bbs_t *b, void *arg);
    void *arg;
} find_t;

static int find_each(const bbs_t *b, void *arg)
{
    find_t *f = arg;
    if (has_word(b->name, f->word) || has_word(b->host, f->word)
        || has_word(b->software, f->word) || has_word(b->location, f->word)) {
        if (f->found++ < f->max) f->each(b, f->arg);
    }
    return 0;
}

int bbs_search(const char *word, int max, void (*each)(const bbs_t *b, void *arg), void *arg)
{
    find_t f = { word, max, 0, each, arg };
    if (!have) return 0;
    walk(find_each, &f);
    return f.found;
}

typedef struct { int num; char *host; int hl; int *port; int ok; } get_t;

static int get_each(const bbs_t *b, void *arg)
{
    get_t *g = arg;
    if (b->num != g->num) return 0;
    snprintf(g->host, (size_t)g->hl, "%s", b->host);
    *g->port = b->port;
    g->ok = 1;
    return 1;
}

int bbs_get(int num, char *host, int hostlen, int *port)
{
    get_t g = { num, host, hostlen, port, 0 };
    if (!have || num < 1) return 0;
    walk(get_each, &g);
    return g.ok;
}
