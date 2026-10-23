/* Сборка zip-архива.
 *
 * Пишется руками, потому что сторонних библиотек в проекте нет и заводить их
 * ради одного формата незачем: zip без «улучшений» — это три записи на файл,
 * и весь он умещается в полторы сотни строк.
 *
 * Сжатие берётся у самого браузера: CompressionStream('deflate-raw') — это
 * ровно тот поток, который zip кладёт способом 8. Своего deflate тут нет и не
 * будет. Если сжатие недоступно или не помогло, запись уходит как есть,
 * способом 0: смешивать способы внутри одного архива формат разрешает, и
 * распаковщику это безразлично.
 *
 * Образы дискет сжимаются в разы: 820 КБ, из которых свободное место забито
 * одним и тем же E5h.
 *
 * Чего здесь НЕТ: zip64. Он нужен от 4 ГБ, а до таких размеров эта корзина не
 * доберётся — весь винт «Вектора» вчетверо меньше. Попытка собрать больше
 * отвергается вслух, а не молча портит архив.
 */

const LIMIT = 0xFFFFFFFF;          /* дальше нужен zip64, а его тут нет */

/* --------------------------------------------------------------- CRC-32 */

/* Таблица считается один раз при загрузке: 256 чисел, доли миллисекунды. */
const CRC_TAB = (() => {
  const t = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[i] = c >>> 0;
  }
  return t;
})();

export function crc32(u8) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < u8.length; i++) c = CRC_TAB[(c ^ u8[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

/* ------------------------------------------------------------ мелочи формата */

/* Время в zip лежит в формате DOS: двухсекундная точность и год от 1980-го.
 * Раньше 1980-го записать нельзя вовсе, оттого нижняя граница. */
export function dos_stamp(d) {
  const y = Math.max(1980, d.getFullYear());
  const time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
  const date = ((y - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  return { time, date };
}

const utf8 = s => new TextEncoder().encode(s);

/* Сжать, если от этого есть толк. Возвращает null, когда толку нет: короткие
 * файлы после deflate нередко ДЛИННЕЕ исходных, и класть их сжатыми — значит
 * тратить время распаковщика впустую. */
async function squeeze(data) {
  if (typeof CompressionStream !== 'function' || !data.length) return null;
  let out;
  try {
    const cs = new CompressionStream('deflate-raw');
    const w = cs.writable.getWriter();
    w.write(data); w.close();
    out = new Uint8Array(await new Response(cs.readable).arrayBuffer());
  } catch { return null; }
  return out.length < data.length ? out : null;
}

/* ------------------------------------------------------------------ сборка */

/* Собрать архив. entries — [{ name, data }], имена уже развёрнутые, с косыми
 * чертами для папок. Порядок записей сохраняется: он же и порядок в списке,
 * который человек видел перед скачиванием.
 *
 * when — время, которое проставится всем записям. Одно на весь архив: минута
 * сборки к содержимому отношения не имеет, а разное время у соседних записей
 * только сбивает с толку. */
export async function zip(entries, when = new Date()) {
  const { time, date } = dos_stamp(when);
  const parts = [], dir = [];
  let at = 0, n = 0;

  for (const e of entries) {
    const name = utf8(e.name);
    const data = e.data;
    const packed = await squeeze(data);
    const body = packed || data;
    const how = packed ? 8 : 0;
    const sum = crc32(data);

    if (at + body.length + name.length + 30 > LIMIT)
      throw new Error('the archive would be over 4 GB, which needs zip64');

    /* Заголовок записи. Флаг 0x0800 — имена в UTF-8; метки дискет бывают
       кириллицей, и без него распаковщик прочтёт их как попало. */
    const lh = new Uint8Array(30 + name.length);
    const lv = new DataView(lh.buffer);
    lv.setUint32(0, 0x04034B50, true);
    lv.setUint16(4, 20, true);
    lv.setUint16(6, 0x0800, true);
    lv.setUint16(8, how, true);
    lv.setUint16(10, time, true);
    lv.setUint16(12, date, true);
    lv.setUint32(14, sum, true);
    lv.setUint32(18, body.length, true);
    lv.setUint32(22, data.length, true);
    lv.setUint16(26, name.length, true);
    lh.set(name, 30);

    parts.push(lh, body);

    const ch = new Uint8Array(46 + name.length);
    const cv = new DataView(ch.buffer);
    cv.setUint32(0, 0x02014B50, true);
    cv.setUint16(4, 20, true);          /* кем создан: 2.0, без изысков */
    cv.setUint16(6, 20, true);          /* и такой же нужен для распаковки */
    cv.setUint16(8, 0x0800, true);
    cv.setUint16(10, how, true);
    cv.setUint16(12, time, true);
    cv.setUint16(14, date, true);
    cv.setUint32(16, sum, true);
    cv.setUint32(20, body.length, true);
    cv.setUint32(24, data.length, true);
    cv.setUint16(28, name.length, true);
    cv.setUint32(42, at, true);         /* где лежит заголовок записи */
    ch.set(name, 46);
    dir.push(ch);

    at += lh.length + body.length;
    n++;
  }

  const dir_len = dir.reduce((s, d) => s + d.length, 0);
  const end = new Uint8Array(22);
  const ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054B50, true);
  ev.setUint16(8, n, true);
  ev.setUint16(10, n, true);
  ev.setUint32(12, dir_len, true);
  ev.setUint32(16, at, true);

  const all = [...parts, ...dir, end];
  const out = new Uint8Array(all.reduce((s, p) => s + p.length, 0));
  let o = 0;
  for (const p of all) { out.set(p, o); o += p.length; }
  return out;
}
