/* Доступ к API: вызов-ответ с подписью. Пояснения — в auth.h. */
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void auth_hex(const uint8_t *p, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2]     = d[p[i] >> 4];
        out[i * 2 + 1] = d[p[i] & 15];
    }
    out[n * 2] = '\0';
}

/* 0 — разобрано. Строго n байт, только строчные и заглавные шестнадцатеричные. */
static int unhex(const char *s, size_t n, uint8_t *out)
{
    size_t i;
    for (i = 0; i < n * 2; i++) {
        char c = s[i];
        int v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        if (i & 1) out[i / 2] |= (uint8_t)v;
        else       out[i / 2]  = (uint8_t)(v << 4);
    }
    return s[n * 2] == '\0' ? 0 : -1;
}

/* Сравнение за постоянное время: обычное memcmp выходит на первом несовпадении,
 * и подпись подбиралась бы побайтно по времени ответа. */
static int same(const uint8_t *a, const uint8_t *b, size_t n)
{
    unsigned diff = 0;
    size_t i;
    for (i = 0; i < n; i++) diff |= (unsigned)(a[i] ^ b[i]);
    return diff == 0;
}

void auth_init(auth_t *a, const char *secret, const uint8_t seed[SHA256_SIZE])
{
    memset(a, 0, sizeof *a);
    if (secret) snprintf(a->secret, sizeof a->secret, "%s", secret);
    memcpy(a->mkey, seed, SHA256_SIZE);
}

int auth_on(const auth_t *a) { return a->secret[0] != '\0'; }

/* Вызов самопроверяемый: время открытым текстом, за ним подпись от него же.
 * Хранить его не нужно — при возврате плата пересчитает подпись заново. */
static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void make_nonce(const auth_t *a, uint32_t t, uint32_t ctr,
                       uint8_t out[AUTH_NONCE_BYTES])
{
    uint8_t mac[SHA256_SIZE], head[8];

    be32(head, t);
    be32(head + 4, ctr);
    combo_hmac_sha256(a->mkey, SHA256_SIZE, head, sizeof head, mac);
    memcpy(out, head, sizeof head);
    memcpy(out + sizeof head, mac, AUTH_NONCE_BYTES - sizeof head);
}

void auth_challenge(auth_t *a, uint32_t now_s, char *out)
{
    uint8_t n[AUTH_NONCE_BYTES];
    make_nonce(a, now_s, ++a->issued, n);
    auth_hex(n, sizeof n, out);
}

/* Сообщение под подписью собирается строкой — ровно так же его собирает морда. */
static void sign_bytes(const char *secret, const char *nonce_hex, const char *seq,
                       const char *method, const char *path,
                       const uint8_t body_hash[SHA256_SIZE], uint8_t out[SHA256_SIZE])
{
    uint8_t key[SHA256_SIZE];
    char hh[SHA256_SIZE * 2 + 1];
    uint8_t nonce[AUTH_NONCE_BYTES];
    hmac_t h;

    /* Ключ сессии из секрета и вызова. По проводу не идёт ни то, ни другое. */
    unhex(nonce_hex, AUTH_NONCE_BYTES, nonce);
    combo_hmac_sha256(secret, strlen(secret), nonce, sizeof nonce, key);

    auth_hex(body_hash, SHA256_SIZE, hh);
    combo_hmac_init(&h, key, sizeof key);
    combo_hmac_update(&h, nonce_hex, strlen(nonce_hex));
    combo_hmac_update(&h, "\n", 1);
    combo_hmac_update(&h, seq, strlen(seq));
    combo_hmac_update(&h, "\n", 1);
    combo_hmac_update(&h, method, strlen(method));
    combo_hmac_update(&h, "\n", 1);
    combo_hmac_update(&h, path, strlen(path));
    combo_hmac_update(&h, "\n", 1);
    combo_hmac_update(&h, hh, strlen(hh));
    combo_hmac_final(&h, out);
}

void auth_sign(const char *secret, const char *nonce_hex, uint32_t seq,
               const char *method, const char *path,
               const uint8_t body_hash[SHA256_SIZE], char *out_hex)
{
    uint8_t mac[SHA256_SIZE];
    char s[16];
    snprintf(s, sizeof s, "%lu", (unsigned long)seq);
    sign_bytes(secret, nonce_hex, s, method, path, body_hash, mac);
    auth_hex(mac, SHA256_SIZE, out_hex);
}

static auth_slot_t *find_slot(auth_t *a, const uint8_t *nonce)
{
    int i;
    for (i = 0; i < AUTH_SESSIONS; i++)
        if (a->s[i].used && same(a->s[i].nonce, nonce, AUTH_NONCE_BYTES))
            return &a->s[i];
    return 0;
}

static auth_slot_t *take_slot(auth_t *a)
{
    int i, best = 0;
    for (i = 0; i < AUTH_SESSIONS; i++) {
        if (!a->s[i].used) { best = i; break; }
        /* Вытесняем самую давнюю. Вычитанием, а не сравнением: счётчик
         * заворачивается, и прямое `<` однажды выбрало бы не ту. */
        if (a->s[best].used &&
            (uint32_t)(a->clock - a->s[i].touched) >
            (uint32_t)(a->clock - a->s[best].touched)) best = i;
    }
    return &a->s[best];
}

int auth_check(auth_t *a, uint32_t now_s,
               const char *nonce_hex, const char *seq_str, const char *mac_hex,
               const char *method, const char *path,
               const uint8_t body_hash[SHA256_SIZE], int need_slot)
{
    uint8_t nonce[AUTH_NONCE_BYTES], want[AUTH_NONCE_BYTES];
    uint8_t got[SHA256_SIZE], mac[SHA256_SIZE];
    uint32_t t, ctr, seq;
    auth_slot_t *sl;
    char *endp;

    if (!auth_on(a)) return AUTH_OFF;

    /* Задержка после череды неудач. Секрет в 128 бит перебором не берут, но
     * если его задали руками и слабым — это единственная преграда. */
    if (a->fails >= AUTH_FAIL_LIMIT) {
        if ((uint32_t)(now_s - a->hold_until_s) >= 0x80000000u) return AUTH_HELD;
        a->fails = 0;
    }

    a->clock++;

    if (!nonce_hex || !seq_str || !mac_hex) return AUTH_MISSING;
    if (unhex(nonce_hex, AUTH_NONCE_BYTES, nonce) < 0) goto bad_nonce;
    if (unhex(mac_hex, SHA256_SIZE, got) < 0)          goto bad_nonce;

    seq = (uint32_t)strtoul(seq_str, &endp, 10);
    if (endp == seq_str || *endp) goto bad_nonce;

    /* Вызов наш? Подпись пересчитывается, хранить его не требуется. */
    t = ((uint32_t)nonce[0] << 24) | ((uint32_t)nonce[1] << 16)
      | ((uint32_t)nonce[2] << 8) | (uint32_t)nonce[3];
    ctr = ((uint32_t)nonce[4] << 24) | ((uint32_t)nonce[5] << 16)
        | ((uint32_t)nonce[6] << 8) | (uint32_t)nonce[7];
    make_nonce(a, t, ctr, want);
    if (!same(nonce, want, AUTH_NONCE_BYTES)) goto bad_nonce;

    /* Свеж? Вычитанием: время платы заворачивается на 49 сутках. */
    if ((uint32_t)(now_s - t) > AUTH_TTL_S) { a->fails++; a->hold_until_s = now_s + AUTH_FAIL_HOLD_S; return AUTH_STALE; }

    sign_bytes(a->secret, nonce_hex, seq_str, method, path, body_hash, mac);
    if (!same(got, mac, SHA256_SIZE)) {
        a->fails++;
        a->hold_until_s = now_s + AUTH_FAIL_HOLD_S;
        return AUTH_BAD_MAC;
    }

    /* Подпись верна — только теперь трогаем таблицу. Не зная секрета, занять
     * запись нельзя, поэтому истощить её посторонний не может. */
    sl = find_slot(a, nonce);
    if (sl) {
        uint32_t d;
        if (seq > sl->seq) {                /* обогнал всех — двигаем окно */
            d = seq - sl->seq;
            sl->seen = d >= AUTH_WINDOW ? 1u : (sl->seen << d) | 1u;
            sl->seq = seq;
        }
        else {                              /* отстал: в окне ли и не был ли */
            d = sl->seq - seq;
            if (d >= AUTH_WINDOW)            { a->fails = 0; return AUTH_REPLAY; }
            if ((sl->seen >> d) & 1u)        { a->fails = 0; return AUTH_REPLAY; }
            sl->seen |= (uint64_t)1u << d;
        }
    }
    else {
        if (need_slot) { a->fails = 0; return AUTH_NO_SLOT; }
        sl = take_slot(a);
        sl->used = 1;
        memcpy(sl->nonce, nonce, AUTH_NONCE_BYTES);
        sl->seq = seq;
        sl->seen = 1u;
    }
    sl->touched = a->clock;
    a->fails = 0;
    return AUTH_OK;

bad_nonce:
    a->fails++;
    a->hold_until_s = now_s + AUTH_FAIL_HOLD_S;
    return AUTH_BAD_NONCE;
}

const char *auth_strerror(int rc)
{
    switch (rc) {
    case AUTH_OK:        return "ok";
    case AUTH_OFF:       return "authentication disabled";
    case AUTH_MISSING:   return "signature headers missing";
    case AUTH_BAD_NONCE: return "challenge not issued by this board";
    case AUTH_STALE:     return "challenge expired, get a new one";
    case AUTH_BAD_MAC:   return "signature does not match";
    case AUTH_REPLAY:    return "sequence number did not grow";
    case AUTH_NO_SLOT:   return "no live session for this operation";
    case AUTH_HELD:      return "too many failures, wait";
    default:             return "unknown error";
    }
}
