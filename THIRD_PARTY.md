# Чужой код в составе

Все, что не перечислено здесь, написано для ComboLite и распространяется под
GNU GPL v3 ([LICENSE](LICENSE)), плата — под CERN-OHL-S v2
([hardware/LICENSE](hardware/LICENSE)).

## ПЛИС

| что | где у нас | откуда | условия |
|---|---|---|---|
| загрузчик BOOT26 (2 КБ) | `fpga/hdl/vendor/boot26/`, постоянной в `fpga/hdl/boot_rom.v` | ImproverX, [github.com/ImproverX/BOOTROM](https://github.com/ImproverX/BOOTROM), коммит `37deb077d6e6cae94cb1f6a0e869cf8bf7fb3736`; исходник `BOOT26.asm` — там же | GNU GPL v3, текст — `fpga/hdl/vendor/boot26/LICENSE` |
| ядро контроллера дисковода, совместимое с КР1818ВГ93 | `fpga/hdl/vendor/fdc1793/` | IanPo (zx-pk.ru) — исходное ядро для компьютера Firefly; andykarpov — переработка для Karabas-Go; solegstar — [github.com/solegstar/FDC1793-Emul](https://github.com/solegstar/FDC1793-Emul); модуль CRC — генератор Easics NV | лицензия авторами не указана, шапки файлов сохранены |
| ядро звука YM2149 | `fpga/hdl/ym2149.v` — перевод с VHDL на Verilog | `quartus/src/ay/ym2149.vhd` из [svofski/vector06c-lesshadoks](https://github.com/svofski/vector06c-lesshadoks) | у файла автор и лицензия не указаны; репозиторий в целом — модифицированная BSD (ниже) |
| обвязка AY | `fpga/hdl/ay_iface.v` | `quartus/src/ay/ayglue.v` из svofski/vector06c-lesshadoks | модифицированная BSD (ниже) |
| страницы квазидиска | `fpga/hdl/kvaz_sram.v` | `kvaz.v` из svofski/vector06c-lesshadoks (В. Славинский, поддержка расширений Баркаря — И. Городецкий) | модифицированная BSD (ниже) |
| подавитель дребезга `purifier` | `fpga/hdl/vu_bus.v` | `vu_bus.v` из svofski/vector06c-lesshadoks | модифицированная BSD (ниже) |

Интерфейс USB-мыши на порту `D4h` повторяет контроллер KTSerg (тема
«Вектор-06Ц. подключение USB-мыши» на zx-pk.ru), чтобы работали программы,
написанные под него; код свой.

### Модифицированная BSD (В. Славинский)

```
Copyright (c) 2007, 2008  Viacheslav Slavinsky

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, 
are permitted provided that the following conditions are met:

	Redistributions of source code must retain the above copyright notice, this 
	list of conditions and the following disclaimer.

	Redistributions in binary form must reproduce the above copyright notice, 
	this list of conditions and the following disclaimer in the documentation 
	and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Веб-интерфейс

| что | где у нас | откуда | условия |
|---|---|---|---|
| Preact 10.24.3 и hooks | `www/vendor/preact.mjs`, `www/vendor/hooks.mjs` | [preactjs.com](https://preactjs.com), файлы без изменений | MIT |
| htm 3.1.1 | `www/vendor/htm.mjs` | [github.com/developit/htm](https://github.com/developit/htm), файл без изменений | Apache-2.0 |
