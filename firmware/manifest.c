/* Разбор манифеста. Пояснения — в manifest.h. */
#include "manifest.h"

#include <string.h>

static const char *ERR;

#define HEAD "combolite-manifest-v1"

static int fail(const char *why)
{
    ERR = why;
    return -1;
}

/* Строка до перевода. Возвращает её длину, начало кладёт в *at. Конец текста
   считается концом строки: манифест без завершающего перевода тоже разберём. */
static size_t line_at(const char *text, size_t len, size_t pos, const char **at)
{
    size_t end = pos;
    *at = text + pos;
    while (end < len && text[end] != '\n') end++;
    return end - pos;
}

static int hex_byte(const char *p, uint8_t *out)
{
    int i, v = 0;
    for (i = 0; i < 2; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else return -1;
    }
    *out = (uint8_t)v;
    return 0;
}

static int unhex(const char *p, size_t chars, uint8_t *out, size_t max)
{
    size_t i;
    if (chars % 2 || chars / 2 > max) return -1;
    for (i = 0; i < chars; i += 2)
        if (hex_byte(p + i, out + i / 2) < 0) return -1;
    return (int)(chars / 2);
}

/* «<sha256hex> <size> <url>» — три поля через один пробел. */
static int part(const char *s, size_t len, man_part_t *out)
{
    size_t i, at;
    uint32_t size = 0;

    if (len < MAN_SHA_HEX + 4) return fail("part line too short");
    if (unhex(s, MAN_SHA_HEX, out->sha, sizeof out->sha) != 32)
        return fail("bad hash");
    if (s[MAN_SHA_HEX] != ' ') return fail("no space after hash");

    for (i = MAN_SHA_HEX + 1; i < len && s[i] != ' '; i++) {
        if (s[i] < '0' || s[i] > '9') return fail("bad size");
        /* Больше четырёх гигабайт не бывает ни у одного раздела, а
           переполнение здесь дало бы правдоподобное маленькое число. */
        if (size > 0xFFFFFFFFu / 10) return fail("size too large");
        size = size * 10 + (uint32_t)(s[i] - '0');
    }
    if (i >= len) return fail("no url");
    if (!size) return fail("zero size");
    out->size = size;

    at = ++i;
    if (len - at >= MAN_URL_MAX) return fail("url too long");
    /* Только печатное и без пробелов: ссылка идёт в HTTP-запрос как есть. */
    for (i = at; i < len; i++)
        if (s[i] <= ' ' || (unsigned char)s[i] > '~') return fail("bad url");
    memcpy(out->url, s + at, len - at);
    out->url[len - at] = '\0';
    out->have = 1;
    return 0;
}

int manifest_parse(const char *text, size_t len, manifest_t *out)
{
    size_t pos = 0, n;
    const char *s;

    ERR = 0;
    memset(out, 0, sizeof *out);

    /* ЗАГОЛОВОК ОБЯЗАТЕЛЕН, и не для красоты: он отличает наш манифест от
       страницы с ошибкой, которую отдаст тот же сервер, если файла нет. Без
       него разбор HTML-страницы шёл бы до первой непонятной строки. */
    n = line_at(text, len, pos, &s);
    if (n != sizeof HEAD - 1 || memcmp(s, HEAD, n)) return fail("not a manifest");
    pos += n + 1;

    while (pos < len) {
        n = line_at(text, len, pos, &s);
        pos += n + 1;
        if (!n) continue;

        if (n > 8 && !memcmp(s, "version=", 8)) {
            if (n - 8 >= MAN_VER_MAX) return fail("version too long");
            memcpy(out->version, s + 8, n - 8);
            out->version[n - 8] = '\0';
        }
        else if (n > 4 && !memcmp(s, "app=", 4)) {
            if (part(s + 4, n - 4, &out->app) < 0) return -1;
        }
        else if (n > 4 && !memcmp(s, "www=", 4)) {
            if (part(s + 4, n - 4, &out->www) < 0) return -1;
        }
        else if (n > 5 && !memcmp(s, "fpga=", 5)) {
            if (part(s + 5, n - 5, &out->fpga) < 0) return -1;
        }
        else if (n > 4 && !memcmp(s, "sig=", 4)) {
            int k;
            /* СТРОКА ПОДПИСИ ОБЯЗАНА БЫТЬ ЗАВЕРШЕНА переводом.
             *
             * Скачивание обрывается где угодно, в том числе посреди подписи, —
             * и обрезок получался «годным»: версия есть, образ есть, подпись
             * короче настоящей, но по виду подпись. Дальше он не прошёл бы
             * проверку ключом, но объяснение звучало бы «подпись не сходится»,
             * а не «файл докачался не весь», и искать причину пришлось бы не
             * там. Обрыв должен называться обрывом. */
            if ((size_t)(s - text) + n >= len) return fail("truncated manifest");
            k = unhex(s + 4, n - 4, out->sig, sizeof out->sig);
            if (k <= 0) return fail("bad signature");
            out->sig_len = (size_t)k;
            /* Подпись покрывает всё ДО своей строки. Считаем это здесь, а не
               на стороне вызывающего: ошибиться на единицу в такой границе
               проще простого, а проверка потом молча не сойдётся. */
            out->signed_len = (size_t)(s - text);
            break;
        }
        /* Неизвестные строки пропускаем молча: так манифест можно будет
           дополнить, не ломая старые платы. Подпись их всё равно покрывает. */
    }

    if (!out->version[0]) return fail("no version");
    if (!out->app.have)   return fail("no firmware image");
    if (!out->sig_len)    return fail("not signed");
    return 0;
}

int manifest_differs(const manifest_t *m, const char *running)
{
    return strcmp(m->version, running ? running : "") != 0;
}

const char *manifest_error(void)
{
    return ERR ? ERR : "no error";
}
