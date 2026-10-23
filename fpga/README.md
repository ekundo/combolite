# ПЛИС ComboLite

Lattice MachXO2 `LCMXO2-7000HE-4TG144C`. Исходники на Verilog, лицензия GNU
GPL v3 ([LICENSE](../LICENSE)); чужие ядра — под своими условиями
([THIRD_PARTY.md](../THIRD_PARTY.md)).

| каталог | что |
|---|---|
| `hdl/` | исходники, верхний модуль `rev4_top` (`hdl/rev4_top.v`), общая логика — `hdl/top.v` |
| `hdl/vendor/fdc1793/` | ядро контроллера дисковода, совместимое с ВГ93 |
| `hdl/vendor/boot26/` | подставной загрузчик BOOT26: образ и лицензия; в ПЛИС он попадает постоянной в `hdl/boot_rom.v` |
| `constraints/pins.lpf` | выводы, частоты, ограничения на задержки |
| `diamond/` | проект Lattice Diamond и стратегия сборки |
| `bitstream/combolite.jed` | готовый образ |

## Готовый образ

`bitstream/combolite.jed` собран из этих исходников. Плата прошивается им с
ESP по Wi-Fi: из веб-интерфейса (обновление прошивки) или с компьютера:

    python3 tools/combo.py fpga fpga/bitstream/combolite.jed

Самую первую прошивку чистой ПЛИС нужно делать по JTAG — так пишется строка
настроек кристалла. Как — в [doc/ASSEMBLY.md](../doc/ASSEMBLY.md), этап 4.

## Сборка

Lattice Diamond 3.5, синтез Synplify Pro.

1. Открыть `diamond/combolite.ldf`. Исходники и `pins.lpf` подключены
   относительными путями, реализация `impl1`, стратегия `combolite.sty`.
2. В стратегии проверить два флажка: Map → Infer GSR = False, Synplify →
   Force GSR = False.
3. Process → Export Files → JEDEC File.

Образ появится в `diamond/impl1/`. После сборки стоит посмотреть отчеты:

- `.mrp` — занятость кристалла: около 94 % LUT4 и все 26 блоков памяти;
- `.twr` — все ограничения выполнены, ошибок времени нет. Самое тесное —
  `MAXDELAY` от `ras_n` до `latch_clk` (порядка 8,3 нс из 10).

## Тактовые частоты

| такт | откуда | для чего |
|---|---|---|
| 50 МГц | генератор на плате | ядро: шина «Вектора», квазидиски, IDE, звук |
| 40 МГц | ФАПЧ | VGA |
| 16 МГц | та же ФАПЧ | ядро дисковода |
