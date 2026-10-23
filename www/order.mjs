/* --------------------------------------------------- перестановка дискет
 *
 * Отдельным модулем, а не внутри морды, по одной причине: перестановка
 * переписывает дискеты целиком, и ошибка здесь стирает данные. Проверять её
 * тыканьем в браузере — плохой способ; отсюда её берёт и морда, и node.
 *
 * Номер дискеты физический: он привязан к месту на винте и никуда не едет.
 * Едет СОДЕРЖИМОЕ — все секторы дискеты разом, а с ними и метка: она лежит
 * внутри той же дискеты.
 *
 * Хранится не журнал действий, а состояние: order.get(pos) — чья дискета
 * должна оказаться на месте pos. Поменяв местами одну и ту же пару пять раз,
 * пользователь возвращается к исходному порядку, запись из order исчезает, и
 * делать физически нечего. Ровно этого журнал действий не умеет.
 */

export const src_of = (order, pos) => order.has(pos) ? order.get(pos) : pos;

/* Тождество не хранится: иначе «поменял и вернул обратно» осталось бы висеть
 * изменением, которого нет. */
export const place = (order, pos, src) =>
  void (pos === src ? order.delete(pos) : order.set(pos, src));

export function swap(order, a, b) {
  if (a === b) return;
  const sa = src_of(order, a), sb = src_of(order, b);
  place(order, a, sb);
  place(order, b, sa);
}

/* Убрать одно место из перестановки: тот, кто брал у него, начинает брать у
 * того, у кого брало оно. Для обычной пары это просто отмена обмена, но так же
 * работает и внутри длинной цепочки. */
export function unswap(order, pos) {
  const takes = src_of(order, pos);
  const taker = [...order.keys()].find(k => order.get(k) === pos);
  place(order, pos, pos);
  if (taker !== undefined && taker !== pos) place(order, taker, takes);
}

/* Перестановка разбирается на циклы: место берёт у следующего, тот у
 * следующего, последнее — у первого. Цикл длины 1 работы не даёт. */
export function cycles(order) {
  const seen = new Set(), out = [];
  for (const pos of [...order.keys()].sort((a, b) => a - b)) {
    if (seen.has(pos)) continue;
    const cyc = [];
    for (let p = pos; !seen.has(p); p = src_of(order, p)) { seen.add(p); cyc.push(p) }
    if (cyc.length > 1) out.push(cyc);
  }
  return out;
}

/* Выполнить один цикл.
 *
 * Кусками по lim секторов, а не дискету целиком: буфер тогда десятки килобайт
 * вместо восьмисот, и то же самое годится для платы, где восьмисот попросту
 * нет.
 *
 * Место нужно прочесть ДО того, как в него написали, — оттого первое снимается
 * во временный буфер, а последнее из него и заполняется.
 *
 * Атомарности тут нет и быть не может: свободного места под третью дискету на
 * винте нет — за последней остаётся 172 сектора при нужных 1570. Обрыв посреди
 * перестановки портит обе дискеты, и об этом сказано в самой морде.
 *
 * io: { base(n), read(lba, cnt) -> байты, write(lba, байты) }.
 */
export async function rotate(cyc, per_floppy, lim, io, on_step) {
  const last = cyc[cyc.length - 1];
  for (let off = 0; off < per_floppy; off += lim) {
    const cnt = Math.min(lim, per_floppy - off);
    const tmp = await io.read(io.base(cyc[0]) + off, cnt);
    for (let i = 0; i < cyc.length - 1; i++)
      await io.write(io.base(cyc[i]) + off,
                     await io.read(io.base(cyc[i + 1]) + off, cnt));
    await io.write(io.base(last) + off, tmp);
    if (on_step) on_step(off + cnt);
  }
}
