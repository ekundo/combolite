/* Привод, которого нет: секторы считаются на лету.
 *
 * ЗАЧЕМ. Узнать, сколько на плате стоит САМА морда, без ПЛИС и привода. Обход
 * каталогов это 324 запроса по восемь секторов плюс один общий запрос меток, и
 * упирается он не в привод, а в HTTP, TCP и разбор на стороне браузера.
 *
 * Настоящий образ сюда не положить: он 260 МБ, а свободного флеша мегабайт.
 * Зато секторы образа не нужно ХРАНИТЬ — их можно СЧИТАТЬ. Заголовок винта, метка
 * дискеты и каталог строятся из номера сектора арифметикой, данные файлов
 * никого не интересуют.
 *
 * ЧЕГО ЭТОТ ЗАМЕР НЕ ДАЁТ. Настоящий привод добавит 190 мкс на сектор и обмен по
 * SPI поверх; арбитр добавит ожидание тишины «Вектора». То есть здесь меряется
 * ПОТОЛОК: быстрее морда на этой плате не будет никогда. Если уж он неприемлем,
 * то с приводом и подавно.
 *
 * Включается ключом сборки COMBO_FAKE_DRIVE и в боевой прошивке не участвует:
 * весь файл под #ifdef, а Kconfig держит опцию выключенной.
 */
#include "sdkconfig.h"

#ifdef CONFIG_COMBO_FAKE_DRIVE

#include "ata.h"

#include <stdio.h>
#include <string.h>

/* Числа настоящего винта: 32 сектора на дорожку, 16 головок, 994 цилиндра,
   324 дискеты. Морда считает по ним раскладку, и подменять их круглыми
   значило бы мерить не ту работу. */
#define F_SPT       32
#define F_HEADS     16
#define F_CYLS      994
#define F_FLOPPIES  324
#define F_SECTORS   ((uint32_t)F_SPT * F_HEADS * F_CYLS)

/* Разметка дискеты на винте: первая начинается с 82-го сектора, шаг 1570,
   каталог занимает первые восемь, метка лежит в 1568-м. */
#define FIRST       82
#define STRIDE      1570
#define DIR_SECS    8
#define LAB_OFF     1568

static int      state = ATA_DRIVE_UNKNOWN;
static uint8_t  written[64][512];   /* немного места под записанное */
static uint32_t written_lba[64];
static int      written_n;

static const char *label_for(int floppy, char *out, size_t max)
{
    static const char *WORDS[] = { "GAMES", "TOOLS", "MUSIC", "TEXTS", "DEMOS" };
    snprintf(out, max, "%s %d", WORDS[floppy % 5], floppy);
    return out;
}

/* Одна запись каталога CP/M: имя, расширение, номер записи, блоки. */
static void entry(uint8_t *e, int floppy, int no)
{
    static const char *NAMES[] = { "READ", "GAME", "PLAY", "DATA", "MAIN",
                                   "TEST", "DEMO", "SETUP" };
    static const char *EXTS[]  = { "COM", "TXT", "BIN", "DAT" };
    char name[9];
    int i, blk = 2 + no * 3;

    snprintf(name, sizeof name, "%s%02d", NAMES[(floppy + no) % 8], no);
    memset(e, ' ', 12);
    e[0] = 0;                                   /* область пользователя */
    for (i = 0; i < 8 && name[i]; i++) e[1 + i] = (uint8_t)name[i];
    memcpy(e + 9, EXTS[(floppy + no) % 4], 3);
    e[12] = 0;                                  /* номер расширения */
    e[13] = e[14] = 0;
    e[15] = 32;                                 /* записей: 16 КБ файл */
    memset(e + 16, 0, 16);
    for (i = 0; i < 2; i++) {                   /* два блока по 2 КБ */
        e[16 + i * 2]     = (uint8_t)((blk + i) & 0xFF);
        e[16 + i * 2 + 1] = (uint8_t)((blk + i) >> 8);
    }
}

static void make_sector(uint32_t lba, uint8_t *buf)
{
    int i;

    memset(buf, 0xE5, 512);                     /* свободно — как на диске */

    if (lba == 0) {                             /* заголовок винта */
        buf[0x80] = F_SPT;
        buf[0x81] = F_HEADS;
        buf[0x82] = (uint8_t)((F_CYLS - 1) & 0xFF);
        buf[0x83] = (uint8_t)((F_CYLS - 1) >> 8);
        buf[0x84] = (uint8_t)(F_FLOPPIES & 0xFF);
        buf[0x85] = (uint8_t)(F_FLOPPIES >> 8);
        return;
    }

    if (lba >= FIRST) {
        uint32_t off = (lba - FIRST) % STRIDE;
        int floppy = (int)((lba - FIRST) / STRIDE) + 1;

        if (floppy > F_FLOPPIES) return;

        if (off == LAB_OFF) {                   /* метка дискеты */
            char text[17];
            label_for(floppy, text, sizeof text);
            memset(buf, ' ', 512);
            memcpy(buf, text, strlen(text));
            return;
        }
        if (off < DIR_SECS) {                   /* каталог: по 16 записей */
            int base = (int)off * 16;
            /* Дискеты с номером кратным семи оставляем пустыми: в морде должно
               быть на что посмотреть и в режиме «показать пустые». */
            if (floppy % 7 == 0) return;
            for (i = 0; i < 16; i++) {
                int no = base + i;
                if (no >= 24) break;            /* по 24 файла на дискету */
                entry(buf + i * 32, floppy, no);
            }
            return;
        }
    }
}

int ata_init(ata_dev_t *dev)
{
    memset(dev, 0, sizeof *dev);
    snprintf(dev->model, sizeof dev->model, "COMBOLITE FAKE DRIVE");
    snprintf(dev->serial, sizeof dev->serial, "no-fpga");
    dev->sectors = F_SECTORS;
    state = ATA_DRIVE_READY;
    return ATA_OK;
}

int ata_read(uint32_t lba, uint8_t *buf)
{
    int i;
    if (lba >= F_SECTORS) return ATA_E_RANGE;
    for (i = 0; i < written_n; i++)
        if (written_lba[i] == lba) { memcpy(buf, written[i], 512); return ATA_OK; }
    make_sector(lba, buf);
    return ATA_OK;
}

int ata_read_run(uint32_t lba, uint32_t count, uint8_t *buf, uint32_t *done)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        int rc = ata_read(lba + i, buf + i * 512);
        if (rc != ATA_OK) { if (done) *done = i; return rc; }
    }
    if (done) *done = count;
    return ATA_OK;
}

/* Записанное запоминается, пока есть место: морда после применения перечитывает
   каталог и должна увидеть своё, иначе замер превратится в проверку упорства. */
int ata_write(uint32_t lba, const uint8_t *buf)
{
    int i;
    if (lba >= F_SECTORS) return ATA_E_RANGE;
    for (i = 0; i < written_n; i++)
        if (written_lba[i] == lba) { memcpy(written[i], buf, 512); return ATA_OK; }
    if (written_n < (int)(sizeof written / sizeof written[0])) {
        written_lba[written_n] = lba;
        memcpy(written[written_n++], buf, 512);
    }
    return ATA_OK;
}

/* Сторож привода. У настоящего он доводит начатый сброс, здесь доводить
   нечего — но srv_run зовёт его каждый оборот, и без него не слинкуется. */
uint8_t ata_kvaz_diag(uint8_t sel) { (void)sel; return 0; }
uint8_t ata_kvaz_diag2(uint8_t sel) { (void)sel; return 0; }
uint8_t ata_kvaz_diag3(uint8_t sel) { (void)sel; return 0; }
void    ata_kvaz_diag_clr(void) { }
void    ata_kvaz_latch(int on) { (void)on; }
void    ata_kvaz_vid(int on) { (void)on; }
void    ata_kvaz_vid_pol(int b) { (void)b; }
void    ata_kvaz_cegate(int on) { (void)on; }
void    ata_kvaz_ceby(int by_cas) { (void)by_cas; }
void ata_tick(void)          { }

int ata_flush(void)          { return ATA_OK; }
int ata_reset_start(void)    { state = ATA_DRIVE_READY; return ATA_OK; }
int ata_drive_state(void)    { return state; }
uint8_t ata_last_error(void) { return 0; }
uint8_t ata_last_status(void) { return 0; }
uint32_t ata_sectors(void)    { return 0; }
void     ata_chs(uint16_t *c, uint8_t *h, uint8_t *s) { *c = 0; *h = 0; *s = 0; }
int      ata_can_flush(void)  { return 1; }

int  ata_lock(void)          { return ATA_OK; }
void ata_unlock(void)        { }
int  ata_lock_long(void)     { return ATA_OK; }

/* «Вектора» здесь нет, значит он и не трогал привод, и всегда молчит. */
int  ata_cpu_touched(void)   { return 0; }
int  ata_cpu_quiet(void)     { return 1; }
void ata_cpu_forget(void)    { }

const char *ata_strerror(int err)
{
    switch (err) {
    case ATA_OK:       return "ok";
    case ATA_E_RANGE:  return "sector past end of disk";
    default:           return "unknown error";
    }
}

#endif /* CONFIG_COMBO_FAKE_DRIVE */
