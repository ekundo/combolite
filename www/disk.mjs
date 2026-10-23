/* Разметка винта «Вектора» и раскладчик CP/M.
 *
 * Отдельным файлом, а не внутри index.html, по одной причине: это самая
 * опасная часть морды — ошибка здесь портит диск молча. Отдельно её можно
 * прогнать в node и сверить её байты с эталоном. Внутри страницы такой сверки
 * не сделать.
 *
 * Ничего от браузера здесь нет: ни DOM, ни fetch, ни Preact.
 */

/* Разметка винта проверена на образе.
 * Здесь только чтение: ни записи, ни подсчёта свободного места. */
const FLOPPY   = 1570;  // секторов на дискету
const FIRST    = 82;    // дискета 1: за заголовком и системной областью
const DIR_SECS = 8;     // каталог: 128 записей по 32 байта
const BLK_SECS = 4;     // блок 2048 байт
const MIN_BLK  = 2;     // 0 и 1 — сам каталог, поэтому 0 значит «блока нет»
const USER_MAX = 0x1F;  // области 0..31: функция 20h маскирует аргумент ANI 1Fh
const MAX_BLK  = 391;   // 390 блоков под файлы
const LAB_OFF  = 1568;  // метка: сразу за файловой системой
const LAB_SIGN = 'HDLB';
const LAB_LEN  = 32;

/* Начало дискеты — это её каталог; блоки и метка отсчитываются от него.
   Дискеты идут встык, поэтому base(n+1) - base(n) = 1570. */
const base = n => FIRST + (n - 1) * FLOPPY;


/* Что за запись каталога. Разбор по документу МикроДОС и по коду БДОС: при
 * подключении диска пропускаются ТОЛЬКО записи с E5h, свободная под новый файл
 * ищется тоже только по E5h.
 *
 *   FILE   — экстент файла, байт 0 это область пользователя 0..31. Не 0..15:
 *            области 16..31 достижимы функцией 20h, хотя команда U туда не
 *            пускает. Считать их мусором нельзя — тогда их блоки окажутся
 *            «свободными» и будут затёрты.
 *   SYSTEM — метка диска (20h) и штамп времени (21h). На «Векторе» их НЕ
 *            бывает: микродос это CP/M 2.2, а 3.1 здесь никогда не было. Ветка
 *            для образов с чужой машины, и затем, чтобы не предложить
 *            «починить» законную запись, приняв её за порчу.
 *   BAD    — всё прочее. Для машины такой слот потерян навсегда: свободным он
 *            не считается, а как запись файла не читается. На этом винте таких
 *            32 — хвост каталога дискеты 1, забитый FF.
 */
const E_FREE = 0, E_FILE = 1, E_SYSTEM = 2, E_BAD = 3;
const entry_kind = b =>
  b === 0xE5 ? E_FREE : b <= USER_MAX ? E_FILE
  : (b === 0x20 || b === 0x21) ? E_SYSTEM : E_BAD;

function parseDir(buf) {
  const byKey = new Map();
  for (let o = 0; o < buf.length; o += 32) {
    const user = buf[o];
    if (entry_kind(user) !== E_FILE) continue;
    let name = '', ext = '';
    for (let i = 1; i <= 8; i++)  name += String.fromCharCode(buf[o + i] & 0x7f);
    for (let i = 9; i <= 11; i++) ext  += String.fromCharCode(buf[o + i] & 0x7f);
    /* Сырые одиннадцать знаков, как они лежат в каталоге: восемь имени плюс
     * три расширения, с дополняющими пробелами. Это ЕДИНСТВЕННЫЙ однозначный
     * ключ файла.
     *
     * Показываем-то мы склейку «имя.расширение», а разобрать её обратно
     * нельзя: у «SPACE.MA» с расширением «STM» она выходит «SPACE.MA.STM», и
     * где здесь разделитель — по строке не понять. Оттого поиск и удаление
     * ходят по сырым полям, а склейка остаётся только для глаз. */
    const raw = name + ext;
    name = name.replace(/ +$/, '');
    ext  = ext.replace(/ +$/, '');
    if (!name.trim()) continue;
    const full = ext ? name + '.' + ext : name;
    const rc = buf[o + 15];
    const extno = (buf[o + 12] & 0x1f) | ((buf[o + 14] & 0x3f) << 5);
    const blocks = [];
    for (let k = 0; k < Math.ceil(rc / 16); k++) {  // в блоке 16 записей по 128
      const b = buf[o + 16 + k * 2] | (buf[o + 17 + k * 2] << 8);
      if (b < MIN_BLK || b > MAX_BLK) break;
      blocks.push(b);
    }
    const key = user + ':' + raw;
    let f = byKey.get(key);
    if (!f) {
      f = { name: full, raw, user, extents: [], records: 0,
            ro: !!(buf[o + 9] & 0x80), sys: !!(buf[o + 10] & 0x80) };
      byKey.set(key, f);
    }
    f.extents.push({ ext: extno, rc, blocks });
    f.records += rc;
  }
  const out = [...byKey.values()];
  for (const f of out) {
    f.extents.sort((a, b) => a.ext - b.ext);
    f.size = f.records * 128;
    f.blocks = f.extents.flatMap(e => e.blocks);
  }
  out.sort((a, b) => a.name.localeCompare(b.name));
  return out;
}

/* Что занято на дискете. Понадобится раскладчику: «влезет ли» упирается и в
 * блоки, и в записи каталога, и на этом винте оба предела достигнуты. */
function dir_stats(buf) {
  const used = new Set();
  const kinds = [0, 0, 0, 0];
  for (let o = 0; o < buf.length; o += 32) {
    const k = entry_kind(buf[o]);
    kinds[k]++;
    if (k !== E_FILE && k !== E_BAD) continue;   /* см. Layout.used_blocks */
    const np = k === E_BAD ? 8 : Math.min(8, Math.ceil(buf[o + 15] / 16));
    for (let j = 0; j < np; j++) {
      const b = buf[o + 16 + j * 2] | (buf[o + 17 + j * 2] << 8);
      if (b >= MIN_BLK && b <= MAX_BLK) used.add(b);
    }
  }
  return { used_blocks: used.size, free_blocks: MAX_BLK - MIN_BLK + 1 - used.size,
           extents: kinds[E_FILE], free_slots: kinds[E_FREE],
           system_slots: kinds[E_SYSTEM], bad_slots: kinds[E_BAD] };
}


function decodeLabel(rec) {
  for (let i = 0; i < LAB_SIGN.length; i++)
    if (rec[i] !== LAB_SIGN.charCodeAt(i)) return '';
  let end = rec.length;                    // хвост режем по байтам, до перекодировки
  while (end > LAB_SIGN.length &&
         (rec[end - 1] === 0x20 || rec[end - 1] === 0x00 || rec[end - 1] === 0xE5)) end--;
  return new TextDecoder('koi8-r').decode(rec.subarray(LAB_SIGN.length, end));
}


/* Сектор метки — ровно то же, что делает HDIR: весь сектор E5h,
 * и если метка не из одних пробелов, подпись плюс 32 знака, добитых пробелами.
 * Сверено побайтно с тем, что записала сама система. */
/* Обратная таблица КОИ-8. Строится из прямой, а не пишется руками: расшифровщик
 * в браузере есть, зашифровщика нет, а своя таблица на двести пятьдесят шесть
 * строк — лишний источник расхождений с эталоном. */
const KOI8_BACK = (() => {
  const all = new Uint8Array(256);
  for (let i = 0; i < 256; i++) all[i] = i;
  const chars = new TextDecoder('koi8-r').decode(all);
  const m = new Map();
  for (let i = 0; i < 256; i++) m.set(chars[i], i);
  return m;
})();

/* Знак -> байт КОИ-8. Незнакомое заменяется вопросом, как это делает
 * encode('koi8-r', 'replace') в эталоне. */
const koi8 = ch => KOI8_BACK.has(ch) ? KOI8_BACK.get(ch) : 0x3F;

function label_sector(text) {
  const sec = new Uint8Array(512).fill(0xE5);
  const t = text.trim();
  if (t) {
    sec.set([0x48, 0x44, 0x4C, 0x42], 0);        /* HDLB */
    sec.fill(0x20, 4, 4 + LAB_LEN);
    for (let i = 0; i < Math.min(t.length, LAB_LEN); i++)
      sec[4 + i] = koi8(t[i]);
  }
  return sec;
}


/* ------------------------------------------------------------- раскладчик
 *
 * Байты раскладчика сверены с эталоном на питоне — расхождение здесь портит
 * диск молча, и одной аккуратности мало.
 *
 * Работает КАК ПРАВКА поверх исходного каталога, а не как пересборка: записи,
 * которых мы не трогали, остаются байт в байт — со всеми признаками, старшими
 * битами имени и чем угодно, чего мы не поняли. Слоты, занятые не файлами
 * (метка, потерянные), не двигаются вовсе.
 */

/* Чем добивается хвост последней записи. Договор CP/M — ^Z, и проверено на
 * диске: так добиты 67 текстовых файлов из примерно восьмидесяти. */
const PAD = 0x1A;

/* Отказ раскладчика.
 *
 * Сообщение складывается ЗДЕСЬ и по-английски: про язык морды раскладчик не
 * знает и знать не должен, а его отказы читает и node, и эталон на питоне. Но
 * рядом с готовой строкой он сохраняет КЛЮЧ и подставленное, и морда переводит
 * их у себя. Без этого «needs 16 blocks, 6 free» приходило бы по-английски и
 * на русской морде: строку, сложенную здесь, переводить уже нечем.
 *
 * count(n, one, many) — число со словом. Раскладчик отдаёт обе части врозь,
 * потому что по-русски форм три, а какая нужна — решает не он. */
export const count = (n, one, many) => ({ n, one, many });

export const fill = (key, args) => key.replace(/%(\d)/g, (_, i) => {
  const a = args[i - 1];
  return a && a.one !== undefined ? `${a.n} ${a.n === 1 ? a.one : a.many}` : a;
});

class FitError extends Error {
  constructor(key, ...args) { super(fill(key, args)); this.key = key; this.args = args }
}

function split_name(fname) {
  /* Срезаем ТОЛЬКО хвостовые пробелы: имя в каталоге дополнено ими до восьми
   * знаков, и они ничего не значат. А ведущий пробел значит: « READ ME» и
   * «READ ME» — разные имена, и trim() слил бы их: файл, который морда
   * показывает, не нашёлся бы ни для удаления, ни для копирования. */
  const up = String(fname).replace(/ +$/, '').toUpperCase();
  const dot = up.indexOf('.');
  const name = dot < 0 ? up : up.slice(0, dot);
  const ext  = dot < 0 ? '' : up.slice(dot + 1);
  if (ext.indexOf('.') >= 0) throw new FitError('more than one dot in "%1"', fname);
  if (!name)           throw new FitError('empty name');
  if (name.length > 8) throw new FitError('name "%1" is longer than 8 characters', name);
  if (ext.length > 3)  throw new FitError('extension "%1" is longer than 3 characters', ext);
  /* Проверяем ХРАНИМОСТЬ, а не приличия.
   *
   * В каталоге имя это одиннадцать байт, и лечь туда может любой печатный
   * семибитный знак: старший бит занят признаками файла, управляющие знаки
   * ломают разбор. Всё, что в эти границы влезает, CP/M хранит и читает.
   *
   * А запрет на < > . , ; : = ? * [ ] живёт в разборщике КОМАНДНОЙ СТРОКИ, не
   * в BDOS: такое имя не набрать с клавиатуры, но программа, строящая FCB
   * сама, его заводит, и на дисках такие имена встречаются — с пробелом
   * посередине, с косой чертой.
   *
   * Здесь запрещать их нельзя. Иначе выходит нелепость: файл лежит на диске,
   * морда его показывает, а скопировать на соседний диск не даёт — потому что
   * при укладке заново её же проверка отвергает имя, которое она сама только
   * что прочитала. Приличия соблюдает suggest_name, и только для того, что
   * приносят СНАРУЖИ: такому имени ещё жить в командной строке «Вектора». */
  for (const c of name + ext)
    if (c.charCodeAt(0) < 0x20 || c.charCodeAt(0) > 0x7E)
      throw new FitError('character "%1" has no place in a CP/M name', c);
  return [name.padEnd(8), ext.padEnd(3)];
}

function build_entry(user, n8, e3, extno, rc, blocks, ro, sysf, arch) {
  const e = new Uint8Array(32);
  e[0] = user;
  for (let i = 0; i < 8; i++) e[1 + i] = n8.charCodeAt(i);
  for (let i = 0; i < 3; i++) e[9 + i] = e3.charCodeAt(i);
  if (ro)   e[9]  |= 0x80;
  if (sysf) e[10] |= 0x80;
  if (arch) e[11] |= 0x80;
  e[12] = extno & 0x1F;
  e[13] = 0;                          /* S1: в CP/M 2.2 не используется */
  e[14] = (extno >> 5) & 0x3F;
  e[15] = rc;
  blocks.forEach((b, i) => { e[16 + i * 2] = b & 0xFF; e[17 + i * 2] = b >> 8 });
  return e;
}

class Layout {
  constructor(dir_bytes) {
    if (dir_bytes.length !== DIR_SECS * 512)
      throw new Error(`каталог это ${DIR_SECS * 512} байт`);
    this.dir = new Uint8Array(dir_bytes);
    this.data = new Map();            /* блок -> 2048 байт к записи */
  }

  /* Снимок раскладки. Нужен пакетным правкам: они делаются «всё или ничего», и
     проверить, влезет ли пачка целиком, можно только сложив её куда-то — а
     трогать настоящую проекцию до того, как всё сошлось, нельзя.

     Байты блоков не копируются, а разделяются: их никто не правит на месте,
     каждая укладка кладёт новые. */
  clone() {
    const c = new Layout(this.dir);
    for (const [b, d] of this.data) c.data.set(b, d);
    return c;
  }

  stats() { return dir_stats(this.dir) }

  used_blocks() {
    const used = new Set();
    for (let i = 0; i < this.dir.length; i += 32) {
      /* E_FILE и E_BAD, но не E_SYSTEM. Так же считает и сама система: вектор
         распределения строится по всем записям с байтом 0 != E5h, и потерянный
         слот в их числе — блоки под ним заняты, пока он цел. У E_SYSTEM же в
         байтах 16..31 не таблица блоков вовсе, см. entry_kind. */
      const kind = entry_kind(this.dir[i]);
      if (kind !== E_FILE && kind !== E_BAD) continue;
      /* У потерянной записи RC значит не больше остального: смотрим все восемь
         указателей. У целой их и так не выйдет больше восьми — RC не превышает
         128, — но испорченный байт не должен уводить за таблицу, поэтому предел
         стоит в обоих случаях. */
      const np = kind === E_BAD ? 8 : Math.min(8, Math.ceil(this.dir[i + 15] / 16));
      for (let j = 0; j < np; j++) {
        const b = this.dir[i + 16 + j * 2] | (this.dir[i + 17 + j * 2] << 8);
        if (b >= MIN_BLK && b <= MAX_BLK) used.add(b);
      }
    }
    return used;
  }

  free_slots() {
    const out = [];
    for (let i = 0; i < this.dir.length; i += 32)
      if (entry_kind(this.dir[i]) === E_FREE) out.push(i / 32);
    return out;
  }

  /* Потерянные слоты, разложенные на безопасные к починке и рискованные.

     Безопасен слот, в таблице блоков которого нет ни одного правдоподобного
     номера: терять там нечего. Если номера правдоподобны, слот вполне может
     оказаться экстентом настоящего файла с испорченным первым байтом —
     освободив такой, мы отдадим чужие блоки под перезапись, и файл, у которого
     уцелели остальные экстенты, потеряет середину. Такие не трогаем и
     показываем отдельно.

     Смотрим все восемь указателей, не оглядываясь на RC: в испорченной записи
     RC значит не больше остального. */
  bad_slots() {
    const safe = [], risky = [];
    for (let i = 0; i < this.dir.length; i += 32) {
      if (entry_kind(this.dir[i]) !== E_BAD) continue;
      let plausible = false;
      for (let j = 0; j < 8; j++) {
        const b = this.dir[i + 16 + j * 2] | (this.dir[i + 17 + j * 2] << 8);
        if (b >= MIN_BLK && b <= MAX_BLK) plausible = true;
      }
      (plausible ? risky : safe).push(i / 32);
    }
    return { safe, risky };
  }

  /* Сырые одиннадцать знаков вместо имени. Всё, что уже лежит на диске,
   * ищется и убирается ИМЕННО так: склейка «имя.расширение» однозначно не
   * разбирается, см. parseDir. */
  static show(raw) {
    /* Обратно в показанный вид: восемь знаков имени, три расширения, хвостовые
       пробелы прочь. Нужно только для отказов — чтобы человек увидел «NOPE.BIN»,
       а не «NOPE    BIN». */
    const n8 = String(raw).padEnd(11).slice(0, 8).replace(/ +$/, '');
    const e3 = String(raw).padEnd(11).slice(8, 11).replace(/ +$/, '');
    return e3 ? n8 + '.' + e3 : n8;
  }

  find_raw(raw, user = 0) {
    const want = String(raw).padEnd(11).slice(0, 11);
    const out = [];
    for (let i = 0; i < this.dir.length / 32; i++) {
      const o = i * 32;
      if (this.dir[o] !== user) continue;
      let got = '';
      for (let k = 1; k <= 11; k++) got += String.fromCharCode(this.dir[o + k] & 0x7F);
      if (got === want) out.push(i);
    }
    return out;
  }

  remove_raw(raw, user = 0) {
    const slots = this.find_raw(raw, user);
    if (!slots.length)
      throw new FitError('no file "%1" in user area %2', Layout.show(raw), user);
    /* Система метит только первый байт — так же делаем и мы. */
    for (const k of slots) this.dir[k * 32] = 0xE5;
    return slots.length;
  }

  find(name, user = 0) {
    const [n8, e3] = split_name(name);
    const want = n8 + e3;
    const out = [];
    for (let i = 0; i < this.dir.length; i += 32) {
      if (entry_kind(this.dir[i]) !== E_FILE || this.dir[i] !== user) continue;
      /* Старший бит в имени и расширении — признак, при сравнении он
         игнорируется; так же делает и сама система. */
      let got = '';
      for (let k = 1; k <= 11; k++) got += String.fromCharCode(this.dir[i + k] & 0x7F);
      if (got === want) out.push(i / 32);
    }
    return out;
  }

  /* Вернуть безопасные потерянные слоты в оборот. Возвращает их число. Правка
     ровно в один байт на слот: E5h — единственное, что система считает
     свободным. Данных за такой записью нет, блоки её указателей и так вне
     диска. */
  repair() {
    const { safe } = this.bad_slots();
    safe.forEach(k => { this.dir[k * 32] = 0xE5 });
    return safe.length;
  }

  /* Очистить дискету: весь каталог в E5h. Возвращает число занятых слотов,
     которые при этом пропадут.

     Блоки данных не трогаем — так же делает и сама система при удалении, и так
     очистка это восемь секторов вместо полутора тысяч. Прочесть старое после
     этого нечем: указателей на него не осталось нигде. */
  erase() {
    let gone = 0;
    for (let i = 0; i < this.dir.length; i += 32)
      if (entry_kind(this.dir[i]) !== E_FREE) gone++;
    this.dir.fill(0xE5);
    this.data.clear();
    return gone;
  }

  remove(name, user = 0) {
    const slots = this.find(name, user);
    if (!slots.length) throw new FitError('no file "%1" in user area %2', name, user);
    /* Система метит только первый байт — так же делаем и мы. */
    slots.forEach(k => { this.dir[k * 32] = 0xE5 });
    return slots.length;
  }

  /* Перенести файл в другую область пользователя.
   *
   * Это правка ОДНОГО БАЙТА на экстент: область живёт в первом байте записи
   * каталога и больше нигде. Данные не двигаются — на диске файл остаётся там
   * же, меняется только то, кому он виден.
   *
   * Копией так делать нельзя: две записи на одни и те же блоки МикроДОС не
   * переживёт. Вектор занятости она строит по каталогу, и удаление одной из
   * записей освободит блоки, на которые ссылается вторая. Настоящая копия
   * между областями — это отдельная укладка, см. add. */
  /* Парой, как find и remove: по имени — для того, что приносят снаружи, по
   * сырым знакам — для того, что уже лежит на диске. */
  chuser(name, user, to) {
    const [n8, e3] = split_name(name);
    return this.chuser_raw(n8 + e3, user, to);
  }

  chuser_raw(raw, user, to) {
    if (to < 0 || to > USER_MAX)
      throw new FitError('user area %1 is outside 0..%2', to, USER_MAX);
    const shown = Layout.show(raw);
    const slots = this.find_raw(raw, user);
    if (!slots.length) throw new FitError('no file "%1" in user area %2', shown, user);
    if (to !== user && this.find_raw(raw, to).length)
      throw new FitError('%1 already exists in user area %2', shown, to);
    slots.forEach(k => { this.dir[k * 32] = to });
    return slots.length;
  }

  add(name, data, { user = 0, ro = false, sysf = false, arch = false, raw = null } = {}) {
    /* raw — сырые одиннадцать знаков. Их передают, когда файл КОПИРУЮТ: имя
     * уже лежит на диске, и пересобирать его из склейки нельзя. */
    const [n8, e3] = raw
      ? [String(raw).slice(0, 8).padEnd(8), String(raw).slice(8, 11).padEnd(3)]
      : split_name(name);
    if ((raw ? this.find_raw(raw, user) : this.find(name, user)).length)
      throw new FitError('%1 already exists in user area %2', name, user);
    if (user < 0 || user > USER_MAX)
      throw new FitError('user area %1 is outside 0..%2', user, USER_MAX);

    const records = Math.ceil(data.length / 128);
    const need_blocks = Math.ceil(records / 16);
    const need_slots = Math.max(1, Math.ceil(records / 128));

    const used = this.used_blocks();
    const free_b = [];
    for (let b = MIN_BLK; b <= MAX_BLK; b++) if (!used.has(b)) free_b.push(b);
    const free_s = this.free_slots();
    if (need_blocks > free_b.length)
      throw new FitError('needs %1, %2 free',
                         count(need_blocks, 'block', 'blocks'), free_b.length);
    if (need_slots > free_s.length)
      throw new FitError('needs %1, %2 free',
                         count(need_slots, 'directory entry', 'directory entries'),
                         free_s.length);

    /* Добивка до целых записей по 128 байт, дальше — до целых блоков. */
    const padded = new Uint8Array(need_blocks * 2048).fill(PAD);
    padded.set(data);

    const taken = [];
    let left = records, extno = 0;
    for (;;) {
      const rc = Math.min(128, left);
      const nb = Math.ceil(rc / 16);
      const blocks = free_b.slice(taken.length, taken.length + nb);
      blocks.forEach((b, j) => {
        const off = (taken.length + j) * 2048;
        this.data.set(b, padded.slice(off, off + 2048));
      });
      this.dir.set(build_entry(user, n8, e3, extno, rc, blocks, ro, sysf, arch),
                   free_s[extno] * 32);
      taken.push(...blocks);
      left -= rc;
      extno++;
      if (left <= 0) break;
    }
    /* Возвращаем сырые одиннадцать знаков: по ним отложенные байты и ищутся,
       а у нового файла их знает только тот, кто их и сложил. */
    this.last_raw = n8 + e3;
    return taken;
  }

  dir_image() { return this.dir }

  /* Что писать на диск, в правильном порядке: сперва блоки, потом каталог.
     Смещения — в секторах от начала дискеты.

     Блоки, на которые итоговый каталог не ссылается, отбрасываются: файл
     могли положить и тут же удалить, и писать за него незачем. */
  writes() {
    const live = this.used_blocks();
    const out = [...this.data.keys()].sort((a, b) => a - b)
      .filter(b => live.has(b))
      .map(b => [b * BLK_SECS, this.data.get(b)]);
    out.push([0, this.dir_image()]);
    return out;
  }
}


export {
  FLOPPY, FIRST, DIR_SECS, BLK_SECS, MIN_BLK, MAX_BLK, USER_MAX,
  LAB_OFF, LAB_SIGN, LAB_LEN, PAD, base,
  E_FREE, E_FILE, E_SYSTEM, E_BAD, entry_kind, parseDir, dir_stats,
  decodeLabel, label_sector, koi8, split_name, build_entry, Layout, FitError,
};
