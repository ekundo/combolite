/* Разбор манифеста обновления.
 *
 * ЗАЧЕМ ОТДЕЛЬНО ОТ ВСЕГО. Это разбор чужого текста, пришедшего из интернета,
 * — то самое место, где ошибка в длине строки превращается в дыру. Здесь нет
 * ни сети, ни ключей, ни флеша: на входе байты, на выходе разобранные поля.
 *
 * ПОДПИСЬ ЗДЕСЬ НЕ ПРОВЕРЯЕТСЯ, только выделяется: для неё нужен mbedTLS, и
 * проверяет её update.c. Разбор обязан быть безопасным САМ ПО СЕБЕ — до
 * всякой подписи, потому что разбирать приходится ещё не проверенное.
 */
#ifndef COMBO_MANIFEST_H
#define COMBO_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#define MAN_VER_MAX  48
#define MAN_URL_MAX  192
#define MAN_SIG_MAX  80         /* ECDSA P-256 в DER: не больше 72, с запасом */
#define MAN_SHA_HEX  64

/* Один раздаваемый образ. */
typedef struct {
    int      have;
    uint8_t  sha[32];
    uint32_t size;
    char     url[MAN_URL_MAX];
} man_part_t;

typedef struct {
    char       version[MAN_VER_MAX];
    man_part_t app, www;
    /* Прошивка ПЛИС: образ CJED, как его собирает combo.py fpga из .jed
     * (fpga.h). */
    man_part_t fpga;
    /* Сколько байт от начала покрыто подписью — всё до строки «sig=». */
    size_t     signed_len;
    uint8_t    sig[MAN_SIG_MAX];
    size_t     sig_len;
} manifest_t;

/* Разобрать. 0 — годен, отрицательное — нет; описание в manifest_error(). */
int manifest_parse(const char *text, size_t len, manifest_t *out);

/* Стоит ли обновляться: версия в манифесте отличается от нынешней.
 *
 * Именно ОТЛИЧАЕТСЯ, а не «новее». Сравнивать «git describe» на больше-меньше
 * нечем — это не номера, — а откат на прежнюю версию бывает нужен не реже
 * обновления. Решает всё равно человек. */
int manifest_differs(const manifest_t *m, const char *running);

const char *manifest_error(void);

#endif
