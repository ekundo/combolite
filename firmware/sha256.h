/* SHA-256 и HMAC-SHA256. Свои, а не из mbedTLS.
 *
 * ПОЧЕМУ СВОИ. Двести строк без зависимостей: разбору и проверке доступа не
 * нужен mbedTLS. Расход — доли миллисекунды на запрос, на фоне 190 мкс
 * обращения к приводу это ничто.
 *
 * Реализация выверена по контрольным примерам из RFC 6234 и RFC 4231. Своя
 * криптография тем и опасна, что молча считает не то.
 *
 * ПРИСТАВКА combo_ ОБЯЗАТЕЛЬНА, и это не вкусовщина. Без неё наш hmac_sha256
 * сталкивается с одноимённой функцией из wpa_supplicant -- того самого, что
 * тянется вместе с WiFi, -- и сборка падает на линковке с «multiple
 * definition». Имена вроде sha256_init в большом проекте заняты почти
 * наверняка, так что своё должно быть своим.
 */
#ifndef COMBO_SHA256_H
#define COMBO_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_SIZE  32
#define SHA256_BLOCK 64

typedef struct {
    uint32_t h[8];
    uint64_t len;                   /* всего байт */
    uint8_t  buf[SHA256_BLOCK];
    size_t   have;
} sha256_t;

void combo_sha256_init(sha256_t *c);
void combo_sha256_update(sha256_t *c, const void *data, size_t n);
void combo_sha256_final(sha256_t *c, uint8_t out[SHA256_SIZE]);
void combo_sha256(const void *data, size_t n, uint8_t out[SHA256_SIZE]);

void combo_hmac_sha256(const void *key, size_t klen,
                 const void *msg, size_t mlen, uint8_t out[SHA256_SIZE]);

/* Потоковый HMAC — нужен там, где сообщение собирается из кусков. */
typedef struct { sha256_t in; uint8_t opad[SHA256_BLOCK]; } hmac_t;
void combo_hmac_init(hmac_t *h, const void *key, size_t klen);
void combo_hmac_update(hmac_t *h, const void *data, size_t n);
void combo_hmac_final(hmac_t *h, uint8_t out[SHA256_SIZE]);

#endif
