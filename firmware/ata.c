/* Последовательности ATA поверх нашей ПЛИС. Оговорки — в ata.h. */

/* Настоящий привод по SPI. Когда собран привод-генератор (COMBO_FAKE_DRIVE,
   см. ata_fake.c), этот файл пуст: имена у них одни и те же, и линковщик
   принял бы два определения за ошибку. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifndef CONFIG_COMBO_FAKE_DRIVE

#include "ata.h"
#include "combo.h"
#include "link.h"

/* регистры (адрес на шине привода) */
#define R_DATA   0
#define R_ERR    1
#define R_COUNT  2
#define R_LBA0   3
#define R_LBA1   4
#define R_LBA2   5
#define R_DEV    6
#define R_CMD    7      /* при чтении — состояние */

/* разряды состояния */
#define S_BSY   0x80
#define S_DRDY  0x40
#define S_DF    0x20
#define S_DRQ   0x08
#define S_ERR   0x01

#define CMD_READ      0x20
#define CMD_WRITE     0x30
#define CMD_IDENTIFY  0xEC
#define CMD_FLUSH     0xE7
#define E_ABRT        0x04              /* регистр ошибки: команду не приняли */

#define BSY_TIMEOUT_US  5000000u        /* раскрутка блина бывает и дольше секунды */
#define DRQ_TIMEOUT_US   100000u        /* см. wait_drq: окно перехода, не работа привода */

/* Срок на готовность ПОСЛЕ АППАРАТНОГО СБРОСА — отдельный и куда более щедрый:
 * привод раскручивает блин с нуля. Это ПОТОЛОК, а не выдержка: крутящийся
 * привод готов за десятки миллисекунд, поднимающийся из остановки — за секунды.
 *
 * ЦИФРА ТРЕБУЕТ ПРОВЕРКИ. Тридцать одна секунда взята по памяти о стандарте
 * ATA, а не из документа под рукой. Перед тем как на неё полагаться, свериться
 * со стандартом: слишком короткий срок объявит исправный привод мёртвым. */
#define RESET_TIMEOUT_MS 31000u

static int lock_depth;

/* Ёмкость запоминается при опознании: до него сектор проверить не с чем и
 * обращение пропускается как есть. */
static uint32_t total_sectors;
static uint16_t chs_c;
static uint8_t  chs_h, chs_s;
static uint8_t  last_err;      /* регистр ошибки после последнего отказа */
static uint8_t  last_st;       /* и регистр состояния: по нему видно, чего не хватило */
static int      has_flush;     /* привод знает FLUSH CACHE; см. identify и ata_flush */
/* Состояние привода. Объявлено ЗДЕСЬ, а не рядом со своим геттером ниже:
   выставляет его ata_init, а он выше по файлу. */
static int      drive_state;

const char *ata_strerror(int err)
{
    switch (err) {
    case ATA_OK:        return "ok";
    case ATA_E_LINK:    return "FPGA not responding (no alive bit)";
    case ATA_E_BUS:     return "arbiter did not grant the bus";
    case ATA_E_PREEMPT: return "bus preempted mid-sequence";
    case ATA_E_TIMEOUT: return "drive kept BSY";
    case ATA_E_NODRQ:   return "drive did not raise DRQ";
    case ATA_E_DRIVE:   return "drive reported an error";
    case ATA_E_RANGE:   return "sector past end of disk";
    case ATA_E_PROTO:   return "FPGA did not answer in time";
    case ATA_E_CPU_BUSY: return "the Vector is using the drive right now";
    default:            return "unknown error";
    }
}

static int lock_common(int longhold)
{
    if (lock_depth++ == 0) {
        if (!combo_alive()) { lock_depth = 0; return ATA_E_LINK; }
        if ((longhold ? combo_request_long() : combo_request()) < 0) {
            lock_depth = 0;
            return ATA_E_BUS;
        }
    }
    return ATA_OK;
}

int ata_lock(void)      { return lock_common(0); }

/* Долгое владение берётся только внешним, первым захватом: вложенные
 * ata_lock внутри ata_write режима не меняют — иначе первая же запись сбила бы
 * его обратно на обычный. */
int ata_lock_long(void) { return lock_common(1); }

void ata_unlock(void)
{
    if (lock_depth > 0 && --lock_depth == 0)
        combo_release();
}

/* Признак отъёма липкий и снимается только новым запросом (fpga/hdl/ide_arb.v:89).
 * Поэтому проверять его достаточно в конце последовательности: если шину
 * забирали, всё прочитанное после этого — мусор, и операцию надо повторить. */
static int preempted(void)
{
    return (combo_status() & ST_TIMEOUT) != 0;
}

static int wait_not_busy(uint8_t *status)
{
    uint32_t waited = 0;
    for (;;) {
        uint16_t s;
        if (combo_reg_read(R_CMD, &s) < 0) return ATA_E_PROTO;
        if (!(s & S_BSY)) { if (status) *status = (uint8_t)s; return ATA_OK; }
        if (waited >= BSY_TIMEOUT_US) return ATA_E_TIMEOUT;
        link_delay_us(100);
        waited += 100;
    }
}

static int check(uint8_t status, ata_dev_t *dev)
{
    if (status & (S_ERR | S_DF)) {
        uint16_t e = 0;
        combo_reg_read(R_ERR, &e);
        last_err = (uint8_t)e;
        if (dev) dev->last_error = last_err;
        return ATA_E_DRIVE;
    }
    return ATA_OK;
}

uint8_t ata_last_error(void) { return last_err; }
uint8_t ata_last_status(void) { return last_st; }
uint32_t ata_sectors(void)    { return total_sectors; }

void ata_chs(uint16_t *c, uint8_t *h, uint8_t *s)
{
    *c = total_sectors ? chs_c : 0;
    *h = total_sectors ? chs_h : 0;
    *s = total_sectors ? chs_s : 0;
}
int      ata_can_flush(void)  { return has_flush; }

/* Адресация LBA28. Регистр устройства: 1110 dLLL, где d — ведущий/ведомый
 * (у нас всегда ведущий), LLL — старшие разряды адреса. */
static int issue(uint8_t cmd, uint32_t lba, uint8_t count)
{
    if (combo_reg_write(R_DEV,   0xE0 | ((lba >> 24) & 0x0F)) < 0) return ATA_E_PROTO;
    if (combo_reg_write(R_COUNT, count)                       < 0) return ATA_E_PROTO;
    if (combo_reg_write(R_LBA0,  (lba      ) & 0xFF)          < 0) return ATA_E_PROTO;
    if (combo_reg_write(R_LBA1,  (lba >>  8) & 0xFF)          < 0) return ATA_E_PROTO;
    if (combo_reg_write(R_LBA2,  (lba >> 16) & 0xFF)          < 0) return ATA_E_PROTO;
    if (combo_reg_write(R_CMD,   cmd)                         < 0) return ATA_E_PROTO;
    /* По стандарту после команды регистр состояния годен не раньше 400 нс.
     * Один цикл нашего генератора длится 740 нс, так что задержка уже
     * выбрана, но пусть будет явной — читающему так понятнее. */
    link_delay_us(1);
    return ATA_OK;
}

/* Буфер ПЛИС отдаёт слова старшим байтом вперёд (fpga/hdl/ide_remote.v:128), а на
 * диске принят младший вперёд. Перестановка здесь, чтобы наружу уходил
 * привычный образ сектора: тот же, что видит сам «Вектор». */
static void swap_words(uint8_t *p)
{
    int i;
    for (i = 0; i < ATA_SECTOR; i += 2) {
        uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t;
    }
}

/* Привести привод в состояние, когда ему можно давать команду: дождаться
 * снятия BSY и дочитать хвост брошенной пачки, если он остался.
 *
 * Хвост появляется так: многосекторное чтение прервали посередине (отобрали
 * шину, привод споткнулся), и привод держит DRQ, ожидая переноса, а не новой
 * команды. Дочитываем и выбрасываем — иначе следующая команда уйдёт в привод
 * посреди чужой передачи.
 *
 * Дёшево вдвойне: сектор забирает ПЛИС, по SPI не едет ничего, а в обычном
 * случае — когда DRQ и так снят — здесь ровно одно чтение состояния, столько
 * же, сколько стоило простое ожидание BSY до появления пачек.
 *
 * Предел — 256 секторов, больше одна команда оставить не может. Если DRQ не
 * снялся и после этого, привод не в себе, и лечит его только сброс. */
/* Готовность данных: BSY снят И DRQ поднят.
 *
 * ПОЧЕМУ НЕ ОДНА ПРОВЕРКА DRQ СРАЗУ ПОСЛЕ BSY. На живой карте она даёт один
 * отказ примерно на сотню каталогов: «привод не поднял DRQ» на случайной
 * дискете, каждый раз другой. Дело не в приводе и не в гонке между запросами —
 * измерено, один поток отказывает так же, как три. Дело в том, что состояние
 * читается по SPI, то есть с задержкой в микросекунды, и чтение иногда
 * попадает в окно между снятием BSY и установкой DRQ.
 *
 * Ждать здесь безопасно: настоящее отсутствие DRQ никуда не денется и доживёт
 * до конца тайм-аута, а ошибку привода поймает check раньше. Тайм-аут взят
 * заведомо больше окна перехода, но много меньше BSY_TIMEOUT_US: если DRQ не
 * появился за сотню миллисекунд после снятого BSY, ждать его дальше нечего. */
static int wait_drq(uint8_t *status)
{
    uint32_t waited = 0;
    for (;;) {
        int rc = wait_not_busy(status);
        if (rc != ATA_OK) return rc;
        if ((rc = check(*status, 0)) != ATA_OK) return rc;
        if (*status & S_DRQ) return ATA_OK;
        /* ПУСТОЕ СОСТОЯНИЕ ЗНАЧИТ ПОТЕРЮ ШИНЫ, А НЕ МОЛЧАНИЕ ПРИВОДА. Живой
         * привод при снятом BSY держит хотя бы DRDY, поэтому ноль оттуда не
         * приходит: он приходит от ПЛИС, когда цикл не выполнен — а не выполнен
         * он ровно тогда, когда владение истекло посреди пачки. Отличать важно:
         * вытеснение выше повторяют (PREEMPT_TRIES), а «привод не поднял DRQ»
         * считают отказом чтения. Если их спутать, скан каталогов теряет по
         * две-три дискеты из 324, каждый раз другие. */
        if (!*status || !(combo_status() & ST_GRANT)) return ATA_E_PREEMPT;
        if (waited >= DRQ_TIMEOUT_US) { last_st = *status; return ATA_E_NODRQ; }
        link_delay_us(50);
        waited += 50;
    }
}

static int ready(uint8_t *status)
{
    int n;
    for (n = 0; n <= 256; n++) {
        int rc = wait_not_busy(status);
        if (rc != ATA_OK) return rc;
        if (!(*status & S_DRQ)) return ATA_OK;
        if (combo_sector_from_drive() < 0) return ATA_E_PROTO;
    }
    return ATA_E_NODRQ;
}

/* Запись одного сектора. Пачками не пишем нарочно: прерванная посередине
 * запись оставила бы на диске половину, и разбираться, какую именно, нечем —
 * читать назад пришлось бы всё. Чтение таких хлопот не создаёт, поэтому пачки
 * есть только у него. */
static int write_one(uint32_t lba, const uint8_t *buf)
{
    uint8_t st, tmp[ATA_SECTOR];
    int rc, i;

    if (total_sectors && lba >= total_sectors) return ATA_E_RANGE;

    if ((rc = ata_lock()) != ATA_OK) return rc;

    /* «Вектор» работает с диском — не пишем вовсе.
     *
     * Спрашиваем ДО команды привода, а не перед переносом данных: отказ на
     * середине оставил бы привод с выданной CMD_WRITE и ждущим DRQ, то есть
     * разбираться пришлось бы сбросом.
     *
     * Отказать здесь ОБЯЗАТЕЛЬНО, хотя запись и так не начнётся: ПЛИС не пустит
     * цикл, но combo_sector_to_drive ждёт СПАДА признака переноса, а у
     * неначавшегося он спал изначально — и молчаливый отказ выглядел бы как
     * успешная запись. */
    if (!(combo_status() & ST_QUIET))  { rc = ATA_E_CPU_BUSY; goto out; }

    if ((rc = ready(&st))               != ATA_OK) goto out;
    if ((rc = issue(CMD_WRITE, lba, 1)) != ATA_OK) goto out;
    if ((rc = wait_drq(&st))            != ATA_OK) goto out;


    for (i = 0; i < ATA_SECTOR; i++) tmp[i] = buf[i];
    swap_words(tmp);
    combo_buf_write(tmp);
    if (combo_sector_to_drive() < 0)   { rc = ATA_E_PROTO; goto out; }
    if ((rc = wait_not_busy(&st))       != ATA_OK) goto out;
    if ((rc = check(st, 0))             != ATA_OK) goto out;

    /* Проверять отъём нужно ИМЕННО ЗДЕСЬ, после переноса: сама передача
     * длится сотни микросекунд, и если арбитр вернул шину «Вектору»
     * посередине, привод увидит чужие циклы, а мы — правдоподобный мусор. */
    if (preempted()) rc = ATA_E_PREEMPT;

out:
    ata_unlock();
    return rc;
}

/* Строки в опознании лежат парами, переставленными внутри слова. После
 * swap_words порядок байт уже привычный, поэтому берём подряд и обрезаем
 * хвостовые пробелы. */
static void take_string(const uint8_t *buf, int word, int words, char *dst)
{
    int i, n = words * 2;
    for (i = 0; i < n; i++) {
        uint8_t c = buf[word * 2 + (i ^ 1)];
        dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

static int identify(ata_dev_t *dev)
{
    uint8_t buf[ATA_SECTOR], st;
    int rc;

    dev->model[0] = dev->serial[0] = '\0';
    dev->sectors = 0;
    total_sectors = 0;
    dev->last_error = 0;

    if ((rc = ata_lock()) != ATA_OK) return rc;

    if ((rc = ready(&st))                     != ATA_OK) goto out;
    if ((rc = issue(CMD_IDENTIFY, 0, 0))      != ATA_OK) goto out;
    if ((rc = wait_drq(&st))                  != ATA_OK && rc != ATA_E_NODRQ) goto out;
    last_st = st;               /* пригодится, если дальше не пойдёт */
    if (rc == ATA_OK) rc = check(st, dev);
    /* ДАННЫЕ ЗАБИРАЕМ, ДАЖЕ ЕСЛИ ПРИВОД ПОЖАЛОВАЛСЯ. Пока DRQ поднят, сектор с
     * ответом уже лежит в приводе, и оставлять его там незачем: по протоколу
     * непрочитанный DRQ висит до следующего сброса. А главное — прочитанное
     * имя модели сразу говорит, верен ли тракт данных: осмысленная строка
     * значит, что все шестнадцать разрядов на месте, и разбираться надо с
     * самим приводом, а не с платой. Код отказа при этом сохраняется. */
    if (rc != ATA_OK && !(st & S_DRQ)) goto out;
    if (rc == ATA_OK && !(st & S_DRQ))       { rc = ATA_E_NODRQ; goto out; }

    if (combo_sector_from_drive() < 0)       { if (rc == ATA_OK) rc = ATA_E_PROTO; goto out; }
    combo_buf_read(buf);
    swap_words(buf);

    take_string(buf, 27, 20, dev->model);
    take_string(buf, 10, 10, dev->serial);
    /* Слово 83, разряд 12 — «умею FLUSH CACHE». Слово годно только если его
     * старшие разряды 15..14 равны 01: у приводов, не знающих этой части
     * стандарта, там нули или единицы, и верить содержимому нельзя. Команда
     * появилась в ATA-4, а CompactFlash такого возраста её не знает и отвечает
     * ABRT — спросить заранее дешевле, чем дёргать привод впустую. */
    has_flush = (buf[167] & 0xC0) == 0x40 && (buf[167] & 0x10);

    dev->sectors = (uint32_t)buf[120] | ((uint32_t)buf[121] << 8)
                 | ((uint32_t)buf[122] << 16) | ((uint32_t)buf[123] << 24);
    /* Слова 1, 3, 6: геометрия по умолчанию, в которой привод понимает CHS.
     * Головок больше 16 и секторов больше 255 у CHS не бывает -- такие числа
     * значат мусор, и разметке их лучше не видеть вовсе. */
    chs_c = (uint16_t)(buf[2] | buf[3] << 8);
    chs_h = buf[7] ? 0 : buf[6];
    chs_s = buf[13] ? 0 : buf[12];
    if (chs_h > 16) chs_h = 0;
    if (rc != ATA_OK) goto out;      /* отказ остаётся отказом */
    total_sectors = dev->sectors;

    if (preempted()) rc = ATA_E_PREEMPT;

out:
    ata_unlock();
    return rc;
}

/* Итог опознания — это и есть состояние привода, и неудачное опознание его
 * тоже меняет: «не спрашивали» и «спросили, молчит» — разные новости для
 * того, кто смотрит на плашку.
 *
 * ОБЁРТКА, А НЕ СТРОЧКА У МЕТКИ out: у identify есть ранний выход, когда шину
 * не дали, а без ПЛИС срабатывает именно он. Метка «в конце функции»
 * покрывает не все выходы, обёртка покрывает все. */
int ata_init(ata_dev_t *dev)
{
    int rc = identify(dev);
    drive_state = (rc == ATA_OK) ? ATA_DRIVE_READY : ATA_DRIVE_DEAD;
    return rc;
}

/* Привод может держать запись в своём кэше. NBD умеет требовать сброс, и
 * оставлять это без внимания нельзя: удалённо отредактированный сектор должен
 * лежать на блине, а не в микросхеме привода. */
int ata_flush(void)
{
    uint8_t st;
    int rc;

    /* Привод, не знающий команды, нечего и просить: у него нет кэша записи, о
     * котором стоило бы беспокоиться, и запись завершается на носителе. Молча
     * отвечаем «сброшено» — иначе морда пугает «не выключайте питание», хотя
     * выключать можно. */
    if (!has_flush) return ATA_OK;

    if ((rc = ata_lock()) != ATA_OK) return rc;
    if ((rc = ready(&st))                 != ATA_OK) goto out;
    if ((rc = issue(CMD_FLUSH, 0, 0))     != ATA_OK) goto out;
    if ((rc = wait_not_busy(&st))         != ATA_OK) goto out;
    rc = check(st, 0);
    /* ABRT значит «команду не приняли», а не «сброс не удался». Слово 83 могло
     * и соврать — такое бывает у переходников, — поэтому верим приводу больше,
     * чем его паспорту, и второй раз уже не спрашиваем. */
    if (rc == ATA_E_DRIVE && (last_err & E_ABRT)) {
        has_flush = 0;
        rc = ATA_OK;
    }
out:
    ata_unlock();
    return rc;
}

/* ------------------------------------------------------------------ сброс
 *
 * Шину на всё время сброса НЕ держим, хотя соблазн есть. Причина в арбитре:
 * предел владения 10 мс, а сброс длится секунды — шину всё равно отобрали бы,
 * и запрос заперся бы до снятия. Поэтому каждый опрос берёт её заново, что при
 * молчащей машине стоит двух тактов (fpga/hdl/ide_arb.v:79).
 *
 * Цена размена: пока привод поднимается, процессор может до него дотянуться.
 * Увидит он `BSY` — то есть правду, а не мусор. Полностью закрыть эту щель
 * можно было бы режимом долгого удержания, но его в арбитре пока нет.
 */
static uint32_t reset_deadline;

int ata_drive_state(void) { return drive_state; }

int  ata_cpu_touched(void) { return (combo_status() & ST_CPU_SEEN) != 0; }
int  ata_cpu_quiet(void)   { return (combo_status() & ST_QUIET)    != 0; }
void ata_cpu_forget(void)  { combo_seen_clear(); }

int ata_reset_start(void)
{
    int rc;

    if ((rc = ata_lock()) != ATA_OK) return rc;
    combo_reset_drive();
    ata_unlock();

    drive_state    = ATA_DRIVE_RESET;
    reset_deadline = link_ms() + RESET_TIMEOUT_MS;
    total_sectors  = 0;         /* прежнее опознание больше не описывает привод */
    return ATA_OK;
}

/* Один шаг. Дёшев: захват шины при молчащей машине плюс одно чтение регистра. */
uint8_t ata_kvaz_diag(uint8_t sel)  { return combo_diag(sel); }
uint8_t ata_kvaz_diag2(uint8_t sel) { return combo_diag2(sel); }
uint8_t ata_kvaz_diag3(uint8_t sel) { return combo_diag3(sel); }
void    ata_kvaz_diag_clr(void)     { combo_diag_clr(); }
void    ata_kvaz_latch(int on)      { combo_latch(on); }
void    ata_kvaz_vid(int on)        { combo_vid(on); }
void    ata_kvaz_vid_pol(int b)     { combo_vid_pol(b); }
void    ata_kvaz_cegate(int on)     { combo_cegate(on); }
void    ata_kvaz_ceby(int by_cas)   { combo_ceby(by_cas); }

void ata_tick(void)
{
    uint16_t st;
    int ok;

    if (drive_state != ATA_DRIVE_RESET) return;

    /* Сравнение через вычитание, а не «now > deadline»: счётчик миллисекунд
     * переполняется примерно раз в 49 суток, и прямое сравнение в этот момент
     * объявило бы привод мёртвым. */
    if ((int32_t)(link_ms() - reset_deadline) >= 0) {
        drive_state = ATA_DRIVE_DEAD;
        return;
    }

    if (ata_lock() != ATA_OK) return;           /* шину не дали — попробуем позже */
    ok = combo_reg_read(R_CMD, &st) == 0;
    ata_unlock();

    if (ok && !(st & S_BSY) && (st & S_DRDY)) {
        /* ПЕРЕОПОЗНАЁМ, А НЕ ПРОСТО ОБЪЯВЛЯЕМ ГОТОВЫМ. Сброс обнулил
         * total_sectors — прежнее опознание больше не описывает привод, — и без
         * повторного IDENTIFY плата остаётся с готовым приводом, но нулевым
         * объёмом и пустым именем. Морде этого мало: по объёму она считает
         * раскладку дискет, а «ready, —» выглядит поломкой. */
        ata_dev_t dev;
        drive_state = ATA_DRIVE_READY;
        if (identify(&dev) != ATA_OK) drive_state = ATA_DRIVE_DEAD;
    }
}

int ata_reset(void)
{
    int rc = ata_reset_start();

    if (rc != ATA_OK) return rc;
    while (drive_state == ATA_DRIVE_RESET) {
        ata_tick();
        if (drive_state == ATA_DRIVE_RESET) link_delay_us(1000);
    }
    return drive_state == ATA_DRIVE_READY ? ATA_OK : ATA_E_TIMEOUT;
}

/* Одна команда привода на пачку. Длину сюда передаёт ata_read_run, она же и
 * следит, чтобы пачка укладывалась в окно владения. */
static int read_run(uint32_t lba, uint32_t count, uint8_t *buf, uint32_t *done)
{
    uint8_t st;
    uint32_t i;
    int rc, pend;

    if ((rc = ata_lock()) != ATA_OK) return rc;

    if ((rc = ready(&st))                                 != ATA_OK) goto out;
    /* Ноль в счётчике означает 256 — так по стандарту. */
    if ((rc = issue(CMD_READ, lba, (uint8_t)(count & 0xFF))) != ATA_OK) goto out;

    /* Конвейер: перенос сектора i пускается и НЕ ожидается — пока ПЛИС занята
     * им, по SPI уезжает сектор i-1 из второго буфера. Порознь это 190 и 510
     * мкс, вместе — большее из двух.
     *
     * БУФЕР НАЗЫВАЕТСЯ ЯВНО: номер едет В КОМАНДЕ (0x54|N наполнить, 0x44|N
     * выбрать), и смениться под ней он не может. Положиться на то, что ПЛИС
     * сама чередует буферы, нельзя: владение переходит по КОНЦУ ПЕРЕНОСА, то
     * есть по событию, асинхронному потоку команд. ESP живёт под FreeRTOS рядом
     * с Wi-Fi, и вытеснение на 200 мкс между двумя посылками SPI — дело
     * обычное. Перенос (190 мкс) к тому времени кончается, владение уже
     * перешло, и выборка отдала бы сектор i вместо i-1: на живой плате это
     * шесть сбойных секторов на 9420 прочитанных, каждый — побайтная копия
     * СЛЕДУЮЩЕГО. Запись этим не страдает: там владение меняется на самой
     * команде, синхронно.
     *
     * Чередуем по номеру сектора: наполняется i&1, выбирается (i-1)&1. Это
     * чистая функция счётчика, а не память о прошлом шаге, поэтому сбиться на
     * шаг внутри пачки нечем.
     *
     * pend — сектор, который уже перенесён в буфер, но ещё не забран по SPI.
     * Он забирается и на отказе тоже: данные в нём настоящие, а без них
     * счётчик done соврал бы, и отказ показал бы на сектор раньше сбойного. */
    pend = -1;
    for (i = 0; i < count; i++) {
        if ((rc = wait_drq(&st)) != ATA_OK) goto drain;
        combo_block_start_n((int)(i & 1));
        if (pend >= 0) {
            uint8_t *p = buf + (size_t)pend * ATA_SECTOR;
            combo_buf_read_n(pend & 1, p);
            swap_words(p);
            /* Отъём проверяем после КАЖДОГО сектора, а не в конце пачки:
             * перенос длится сотни микросекунд, и если шину вернули «Вектору»
             * посередине, привод увидит чужие циклы, а мы — правдоподобный
             * мусор. */
            if (preempted()) { rc = ATA_E_PREEMPT; goto out; }
            if (done) (*done)++;
            pend = -1;
        }
        /* Сюда — только goto out: перенос в полёте, забирать нечего. */
        if (combo_block_wait() < 0) { rc = ATA_E_PROTO; goto out; }
        pend = (int)i;
    }

drain:
    if (pend >= 0) {
        uint8_t *p = buf + (size_t)pend * ATA_SECTOR;
        combo_buf_read_n(pend & 1, p);
        swap_words(p);
        if (rc == ATA_OK && preempted()) rc = ATA_E_PREEMPT;
        if (done) (*done)++;
    }

out:
    ata_unlock();
    return rc;
}

int ata_read_run(uint32_t lba, uint32_t count, uint8_t *buf, uint32_t *done)
{
    int rc;

    if (done) *done = 0;
    if (count == 0) return ATA_OK;
    /* Проверяем всю пачку разом, а не по сектору: вычитание, а не сложение,
     * чтобы конец диска не переполнил счётчик. */
    if (total_sectors && (lba >= total_sectors || count > total_sectors - lba))
        return ATA_E_RANGE;

    while (count > 0) {
        uint32_t n = count > ATA_RUN ? ATA_RUN : count;
        if ((rc = read_run(lba, n, buf, done)) != ATA_OK) return rc;
        lba += n;
        count -= n;
        buf += (size_t)n * ATA_SECTOR;
    }
    return ATA_OK;
}

int ata_read(uint32_t lba, uint8_t *buf)
{
    return ata_read_run(lba, 1, buf, 0);
}

int ata_write(uint32_t lba, const uint8_t *buf)
{
    return write_one(lba, buf);
}

#endif /* !CONFIG_COMBO_FAKE_DRIVE */
