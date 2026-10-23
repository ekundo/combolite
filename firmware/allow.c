/* Список разрешённых адресов. Пояснения — в allow.h. */
#include "allow.h"

#include <string.h>

/* Разбор одного "a.b.c.d" или "a.b.c.d/len". 0 — годится. */
static int one(const char *s, const char *end, uint32_t *net, uint32_t *mask)
{
    uint32_t ip = 0, oct = 0, bits = 32;
    int digits = 0, parts = 0, slash = 0;
    const char *p;

    for (p = s; p < end; p++) {
        if (*p >= '0' && *p <= '9') {
            oct = oct * 10 + (uint32_t)(*p - '0');
            if (oct > 255) return -1;
            digits++;
            if (digits > 3) return -1;
        }
        else if (*p == '.' && !slash) {
            if (!digits || parts == 3) return -1;
            ip = (ip << 8) | oct;
            oct = 0; digits = 0; parts++;
        }
        else if (*p == '/' && !slash) {
            if (!digits || parts != 3) return -1;
            ip = (ip << 8) | oct;
            oct = 0; digits = 0; parts++;
            slash = 1;
        }
        else return -1;
    }
    if (!digits) return -1;

    if (slash) {
        bits = oct;
        if (bits > 32) return -1;
    }
    else {
        if (parts != 3) return -1;
        ip = (ip << 8) | oct;
    }

    *mask = bits ? (uint32_t)(0xFFFFFFFFu << (32 - bits)) : 0u;
    *net  = ip & *mask;
    return 0;
}

int allow_parse(allow_t *a, const char *spec)
{
    const char *p;

    a->n = 0;
    if (!spec || !*spec) return 0;

    /* Цикл нарочно не по `*p`: иначе звено ПОСЛЕ последней запятой не
     * разбиралось бы вовсе, и "1.2.3.4," проходило бы как верное. */
    for (p = spec; ; ) {
        const char *e = p;
        while (*e && *e != ',') e++;
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        {
            const char *t = e;
            while (t > p && (t[-1] == ' ' || t[-1] == '\t')) t--;
            if (t > p) {
                if (a->n == ALLOW_MAX) { a->n = 0; return -1; }
                if (one(p, t, &a->net[a->n], &a->mask[a->n]) < 0) { a->n = 0; return -1; }
                a->n++;
            }
            else { a->n = 0; return -1; }   /* пустое звено: "1.2.3.4,,5.6.7.8" */
        }
        if (!*e) break;
        p = e + 1;
    }
    return a->n ? 0 : -1;
}

int allow_ok(const allow_t *a, uint32_t addr)
{
    int i;
    if (a->n == 0) return 1;
    for (i = 0; i < a->n; i++)
        if ((addr & a->mask[i]) == a->net[i]) return 1;
    return 0;
}
