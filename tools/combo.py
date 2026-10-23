#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Разговор с платой из командной строки: подписанные запросы к её API.

ЗАЧЕМ. Всё, что плата умеет сверх морды, — обновиться, забыть сеть,
рассказать про эфир, — идёт через подпись, и curl тут бесполезен: подпись
покрывает метод, путь и хеш тела, её надо посчитать. Скрипт делает ровно это.

СЕКРЕТ БЕРЁТСЯ ИЗ ОКРУЖЕНИЯ, а не из довода командной строки: доводы видны в
списке процессов всей машине и оседают в истории оболочки. Секрет платы — ключ
к диску, и класть его туда незачем.

    export COMBO_SECRET=...          один раз за сеанс
    export COMBO_HOST=192.168.1.2    если плата не там, где по умолчанию

    python3 tools/combo.py info                 что плата о себе говорит
    python3 tools/combo.py diag-clear           обнулить счётчики наблюдателя
    python3 tools/combo.py board                узлы ПЛИС, настройки, клавиши, растр, дисковод
    python3 tools/combo.py set off=fdc,ay fdc_rdy=index fdc_pc=4 vga=60 vi53=off
                                                настройки ПЛИС; плата их хранит и заливает
                                                после включения и перезаливки. off=none --
                                                всё включено; fdc_rdy=34|index|always
    python3 tools/combo.py set fdd=shugart|pc     шлейф дисковода: прямой (A: 10, B: 12) или от ПК с
                                                перекруткой (A: 12 -- Gotek на S1, B: 16 -- 3,5" на DS0)
    python3 tools/combo.py set boot=rom|power|vvod
                                                загрузка машины: из ПЗУ, подставным
                                                загрузчиком при включении, и после
                                                каждого ВВОД с клавиатуры тоже
    python3 tools/combo.py keys vvod|sbr|release нажать ВВОД (из ПЗУ), СБР (без ПЗУ), отпустить
    python3 tools/combo.py loader arm|boot|off  подставной загрузчик: взвести, взвести и СБР, снять
    python3 tools/combo.py screen shot.png [palette=00..ff x16]
                                                снимок экрана машины, в цветах машины, если
                                                битстрим отдаёт палитру; иначе -- условные.
                                                palette= -- свои 16 байт BBGGGRRR
    python3 tools/combo.py fdcwatch [secs=60]   опрос ВГ93: команда, состояние, дорожка, сектор;
                                                за секунду -- байты, адресные метки, импульсы, индексы
    python3 tools/combo.py palog [raw.bin]      журнал последних 128 записей машины в порты
                                                палитры и порта B: где стоял луч (fpga/hdl/vid_rec.v)
    python3 tools/combo.py fpga --detect        опознать ПЛИС по SSPI
    python3 tools/combo.py fpga file.jed        залить битстрим во флеш ПЛИС: плата пишет сама
                                                по SSPI (секунды).
                                                --force -- строка настроек в файле другая,
                                                --no-refresh -- не перезагружать ПЛИС,
                                                --no-verify -- без постраничной сверки (запасной выход)
    python3 tools/combo.py latch-on             заморозить защёлку адреса (опыт)
    python3 tools/combo.py latch-off            вернуть прежнее поведение
    python3 tools/combo.py vid-on               гасить такт защёлки на видеоспаде (опыт)
    python3 tools/combo.py vid-off              вернуть прежнее поведение
    python3 tools/combo.py vid-pol-a            различитель: видеоспад после длинного промежутка ~RAS
    python3 tools/combo.py vid-pol-b            видеоспад после короткого промежутка
    python3 tools/combo.py ce-on                выбирать ОЗУ только на стробах (опыт)
    python3 tools/combo.py ce-off               вернуть прежнее поведение
    python3 tools/combo.py ce-by-cas            квалифицировать по ~CAS
    python3 tools/combo.py ce-by-strobe         квалифицировать по стробам
    python3 tools/combo.py wifi                 что она видит в эфире
    python3 tools/combo.py log                  её журнал
    python3 tools/combo.py ota firmware/esp32/build/combolite.bin
    python3 tools/combo.py ota firmware/esp32/build/www.bin
    python3 tools/combo.py update               что выпущено на GitHub (из кэша платы)
    python3 tools/combo.py update --check       спросить GitHub заново, не ждать суток
    python3 tools/combo.py update --apply       плата скачает это сама и поставит
    python3 tools/combo.py reboot
    python3 tools/combo.py forget --yes         забыть сеть и уйти в настройку

Что куда заливать, скрипт решает по имени файла: www.bin — статика морды,
всё прочее — прошивка. Ошибиться тут дорого, а имена эти неизменны.
"""
import hashlib
import hmac
import http.client
import json
import os
import struct
import sys
import time
import zlib

HOST = os.environ.get('COMBO_HOST', '192.168.1.2')
PORT = int(os.environ.get('COMBO_PORT', '80'))
EMPTY = hashlib.sha256(b'').hexdigest()


class Board:
    def __init__(self, secret):
        self.c = http.client.HTTPConnection(HOST, PORT, timeout=180)
        self.c.request('GET', '/api/challenge')
        r = self.c.getresponse()
        if r.status != 200:
            raise SystemExit('вызов не получен: %d — на плате не задан секрет?'
                             % r.status)
        self.nonce = json.loads(r.read())['nonce']
        self.key = hmac.new(secret.encode(), bytes.fromhex(self.nonce),
                            hashlib.sha256).digest()
        self.seq = 0

    def go(self, method, path, body=b''):
        self.seq += 1
        h = hashlib.sha256(body).hexdigest() if body else EMPTY
        msg = '\n'.join([self.nonce, str(self.seq), method, path, h])
        mac = hmac.new(self.key, msg.encode(), hashlib.sha256).hexdigest()
        head = {'X-Combo-Nonce': self.nonce, 'X-Combo-Seq': str(self.seq),
                'X-Combo-Mac': mac}
        if body:
            head['Content-Length'] = str(len(body))
            head['Content-Type'] = 'application/octet-stream'
        self.c.request(method, path, body=body or None, headers=head)
        r = self.c.getresponse()
        return r.status, r.read()


# Коды кристаллов (firmware/jtag.h) и страницы флеша конфигурации, DS-02056.
JED_PARTS = {'LCMXO2-2000HC': (0x012BB043, 3198), 'LCMXO2-4000HC': (0x012BC043, 5758),
             'LCMXO2-7000HC': (0x012BD043, 9212), 'LCMXO2-7000HE': (0x012B5043, 9212),
             'LCMXO2-7000ZE': (0x012B5043, 9212)}


def jed_image(path):
    """.jed -> образ CJED для /api/fpga/part (формат -- firmware/fpga.h).

    Разбор здесь, а не на плате: текст .jed -- 1,5 МБ, страниц в нём на
    99 КБ. Здесь же сверяется контрольная сумма JEDEC (поле C: сумма байт по
    восемь предохранителей, младший первым) -- испорченный файл не уедет."""
    t = open(path, 'rb').read().decode('latin1')
    dev = qf = csum = None
    dflt = 0
    fuses = {}
    feature = feabits = None
    usercode = 0
    for f in t.split('*'):
        s = f.strip()
        if s.startswith('NOTE DEVICE NAME:'):
            dev = s.split(':', 1)[1].strip()
        elif s.startswith('QF'):
            qf = int(s[2:])
        elif s in ('F0', 'F1'):
            dflt = int(s[1])
        elif s.startswith('L'):
            parts = s.split()
            fuses[int(parts[0][1:])] = ''.join(parts[1:])
        elif s.startswith('C') and len(s) == 5:
            csum = int(s[1:], 16)
        elif s.startswith('E') and not s.startswith('E0x'):
            lines = s[1:].split()
            feature, feabits = lines[0], lines[1] if len(lines) > 1 else ''
        elif s.startswith('UH'):
            usercode = int(s[2:], 16)
    part = next((k for k in JED_PARTS if dev and dev.startswith(k)), None)
    if not part:
        raise ValueError('не знаю кристалла %s' % dev)
    idcode, cf_pages = JED_PARTS[part]
    if not qf or not fuses or csum is None or feature is None or len(feature) != 64 \
            or len(feabits) != 16:
        raise ValueError('в файле нет нужных полей (QF, L, C, E)')
    arr = bytearray([0xFF if dflt else 0] * ((qf + 7) // 8))
    for start, bits in fuses.items():
        for i, ch in enumerate(bits):
            n = start + i
            if ch == '1':
                arr[n // 8] |= 1 << (n % 8)
            else:
                arr[n // 8] &= ~(1 << (n % 8)) & 0xFF
    if sum(arr) & 0xFFFF != csum:
        raise ValueError('контрольная сумма JEDEC не сошлась: файл испорчен')
    rows = {}
    for start, bits in fuses.items():
        for i in range(len(bits) // 128):
            rows[start // 128 + i] = bits[i * 128:(i + 1) * 128]
    nz = [k for k, v in rows.items() if '1' in v]
    last = max(nz) + 1 if nz else 0
    if last > cf_pages:
        raise ValueError('в файле данные UFM (страницы за %d): этот путь их не пишет'
                         % cf_pages)
    data = b''.join(int(rows.get(i, '0' * 128), 2).to_bytes(16, 'big')
                    for i in range(last))
    fr = bytes(int(feature[8 * k:8 * k + 8], 2) for k in range(8))
    fb = bytes(int(feabits[8 * k:8 * k + 8], 2) for k in range(2))
    hdr = (b'CJED' + bytes([1, 0, 40, 0]) + struct.pack('<II', idcode, usercode) +
           fr + fb + b'\0\0' + struct.pack('<III', last, zlib.crc32(data) & 0xFFFFFFFF, 0))
    return hdr + data, part, last


DEVS = ['fdc', 'kvaz1', 'kvaz2', 'ide', 'ay', 'mirror']      # разряды C_DEV
RDY = {'34': 0, 'index': 1, 'always': 2}


def set_arg(k, v):
    """Довод `set` -> часть запроса. Имена вместо чисел: маску устройств
    наизусть не помнит никто."""
    if k == 'off':
        m = 0
        for n in filter(None, v.split(',')):
            if n == 'none':
                continue
            if n not in DEVS:
                raise ValueError('off: нет устройства %s, есть %s' % (n, ','.join(DEVS)))
            m |= 1 << DEVS.index(n)
        return ['dev_off=%d' % m]
    if k == 'dev_off':
        return ['dev_off=%d' % int(v, 0)]
    if k == 'fdc_rdy':
        return ['fdc_rdy=%d' % (RDY[v] if v in RDY else int(v, 0))]
    if k == 'fdc_pc':
        return ['fdc_pc=%d' % int(v, 0)]
    if k == 'fdd':
        if v not in FDD_CABLE:
            raise ValueError('fdd: %s' % '|'.join(FDD_CABLE))
        return ['fdd_cable=%d' % FDD_CABLE.index(v)]
    if k == 'vga':
        if v not in ('50', '60'):
            raise ValueError('vga: 50 или 60')
        return ['vga60=%d' % (v == '60')]
    if k == 'vi53':
        if v not in ('on', 'off'):
            raise ValueError('vi53: on или off')
        return ['vi53=%d' % (v == 'on')]
    if k == 'boot':
        if v not in BOOT:
            raise ValueError('boot: %s' % '|'.join(BOOT))
        return ['boot=%d' % BOOT.index(v)]
    raise ValueError('не знаю настройки %s: off, fdc_rdy, fdc_pc, fdd, vga, vi53, boot' % k)


# Шлейф дисковода, firmware/board.h fdd_cable: по номеру.
FDD_CABLE = ['shugart', 'pc']
FDD_CABLE_RU = ['прямой (Шугарт: A: 10, B: 12, мотор 16)',
                'от ПК с перекруткой (A: 12, B: 16, мотор 10)']


# Загрузка машины, firmware/board.h BOOT_*: по номеру.
BOOT = ['rom', 'power', 'vvod']
BOOT_RU = ['из ПЗУ', 'подставным загрузчиком при включении',
           'подставным при включении и после ВВОД']
KIND_RU = {'unknown': 'не разобран', 'own': 'свой', 'not_vvod': 'не ВВОД',
           'vvod': 'ВВОД'}


# Условные цвета: различимые, по порядку номеров. Нужны, когда битстрим
# палитру не отдаёт (до команды 0x57).
STOCK = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170), (170, 0, 0),
         (170, 0, 170), (170, 85, 0), (170, 170, 170), (85, 85, 85), (85, 85, 255),
         (85, 255, 85), (85, 255, 255), (255, 85, 85), (255, 85, 255),
         (255, 255, 85), (255, 255, 255)]


def vector_rgb(b):
    """Байт палитры «Вектора» BBGGGRRR -> RGB. У синего два разряда."""
    r, g, bl = b & 7, (b >> 3) & 7, (b >> 6) & 3
    return (r * 255 // 7, g * 255 // 7, bl * 255 // 3)


def screen_png(frame, palette, m512=False):
    """256 строк по 128 байт (слои 0..3 по 32 колонки, firmware/board.h) -> PNG
    256x256 с палитрой. Номер цвета -- как в fpga/hdl/vid_play.v: слой 0 (8000h) --
    старший разряд, слой 3 (E000h) -- младший. В режиме 512 точка делится
    пополам: левая -- {0, 0, c1, c0}, правая -- {c3, c2, 0, 0}; картинка тогда
    512x512, строка удвоена, чтобы пропорции были те же, что у 256x256."""
    rows = []
    for y in range(256):
        r = frame[y * 128:(y + 1) * 128]
        line = bytearray([0])                     # фильтр строки: нет
        for col in range(32):
            p = [r[col + 32 * k] for k in range(4)]
            for bit in range(7, -1, -1):
                n = (((p[0] >> bit) & 1) << 3 | ((p[1] >> bit) & 1) << 2 |
                     ((p[2] >> bit) & 1) << 1 | ((p[3] >> bit) & 1))
                if m512:
                    line += bytes([n & 3, n & 12])
                else:
                    line.append(n)
        rows.append(bytes(line))
        if m512:
            rows.append(bytes(line))
    side = 512 if m512 else 256

    def chunk(t, data):
        return (struct.pack('>I', len(data)) + t + data +
                struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', side, side, 8, 3, 0, 0, 0)) +
            chunk(b'PLTE', b''.join(bytes(c) for c in palette)) +
            chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) +
            chunk(b'IEND', b''))


def print_palog(buf):
    """Журнал записей в порты (fpga/hdl/vid_rec.v, «журнал записей»): палитра, порт
    B, номер следующей записи (17), счёт кадров (18), с 1024 -- 128 записей по
    8 байт, от старой к новой."""
    print('палитра регистратора: %s; порт B %02x; кадр %d'
          % (buf[:16].hex(' '), buf[16], buf[18]))
    wp = buf[17] & 0x7F
    print('  №   кадр строка такт  порт знач  бордюр 512  место         ячейка у нас')
    for k in range(128):
        e = buf[1024 + 8 * ((wp + k) & 0x7F):][:8]
        if not any(e):
            continue
        val, fl, pos, ln_lo, ln_hi, frm, port, seq = e
        line = ln_lo | (ln_hi & 1) << 8
        in_pic, is_ctl, is_pb, pb = fl >> 7 & 1, fl >> 6 & 1, fl >> 5 & 1, fl & 0x1F
        if port >= 0xF0:
            kind = port & 3
            what = ['разрыв S', 'ОПОРА', 'INTA', '?'][kind]
            cell = (['S %d после %d' % (val, fl), 'S %d, строка была %d' % (val, fl),
                     'строка %d, видеоцикл %d' % (val, fl), ''][kind])
            print('%3d %5d %5d %5d   %02x               %-12s  %s' % (seq, frm, line, pos, port, what, cell))
            continue
        if is_ctl:
            what = 'ВВ55 упр.'
            cell = 'порт B -> 0'
        elif is_pb:
            what = 'порт B'
            cell = 'бордюр %d%s' % (val & 15, ', 512' if val & 0x10 else '')
        else:
            what = 'палитра'
            cell = 'под лучом' if in_pic else '%d' % (pb & 15)
        flags = ('' if ln_hi & 0x80 else ' строка?') + ('' if ln_hi & 0x40 else ' растр?')
        print('%3d %5d %5d %5d   %02x   %02x   %5d %3s  %-12s  %s%s  %s'
              % (seq, frm, line, pos, port, val, pb & 15, '512' if pb & 0x10 else '',
                 'КАРТИНКА' if in_pic else 'вне', cell, flags, what))


def print_board(d):
    s = d.get('settings', {})
    off = [n for i, n in enumerate(DEVS) if s.get('dev_off', 0) >> i & 1]
    rdy = {v: k for k, v in RDY.items()}.get(s.get('fdc_rdy'), '?')
    print('настройки: выключено %s; готовность ВГ93 %s; предкомпенсация %d%s; '
          'шлейф %s; VGA %s Гц; ВИ53 %s; загрузка %s'
          % (','.join(off) or 'ничего', rdy, s.get('fdc_pc', 0) & 15,
             ' с 44-й дорожки' if s.get('fdc_pc', 0) & 16 else '',
             FDD_CABLE_RU[s.get('fdd_cable', 0)] if s.get('fdd_cable', 0) < 2 else '?',
             '60' if s.get('vga60') else '50', 'наш' if s.get('vi53') else 'машины',
             BOOT_RU[s['boot']] if s.get('boot', 0) < len(BOOT_RU) else '?'))
    if not d.get('present'):
        print('ПЛИС: блоков 4 и 5 нет -- старый битстрим или ПЛИС молчит; '
              'настройки зальются, когда она ответит')
        return
    f = d['features']
    print('узлы битстрима (раскладка %d): %s' % (d['layout'], ' '.join(
        k for k in ('fdc', 'vi53', 'ay', 'kvaz2', 'mouse', 'keys', 'loader',
                    'mirror', 'watch', 'video', 'audio_split', 'kbd_reset') if f.get(k))))
    print('клавиши: ВВОД %s, СБР %s; загрузчик: %s'
          % ('нажат' if d['keys']['vvod'] else 'отпущен',
             'нажат' if d['keys']['sbr'] else 'отпущен',
             'окно' if d['loader']['window'] else
             'взведён' if d['loader']['armed'] else 'снят'))
    if f.get('video'):
        v = d['video']
        print('растр: опора %s, захват %s, VGA %s Гц, привязка %s (ошибка %d строк); '
              'прокрутка %d, строк кадра %d'
              % ('есть' if v['raster_ok'] else 'НЕТ', 'есть' if v['vid_lock'] else 'НЕТ',
                 '60' if v['vga60'] else '50', 'точна' if v['lock_good'] else 'плывёт',
                 v['lock_err'], v['scroll'], v['rows']))
        if 'pll_ok' in v:
            print('      ФАПЧ 40/16 МГц: %s' % ('держит захват' if v['pll_ok'] else
                                              'НЕТ захвата или был срыв с включения'))
        print('      опозданий строки %d, отбоев выборки %d, журнал сверх 16: %d, '
              'поправки растра %d/%d, промахов зеркала %d'
              % (v['late_lines'], v['fetch_aborts'], v['journal_over'],
                 v['slip_v'], v['slip_h'], d['mirror_miss']))
    if 'kbd' in d:
        # Первые байты после сброса; после нашего ВВОД это подпись ПЗУ.
        # Совпадения -- по восьми чтениям, разряд k -- чтение k.
        k = d['kbd']
        print('сброс с клавиатуры: подпись ПЗУ %s, последний %s%s; первые байты %s, '
              'совпало с ПЗУ %s; событий %d, ВВОД %d (счётчики по кругу до 15)%s'
              % ('есть' if k['rom_known'] else 'НЕТ', KIND_RU.get(k['kind'], '?'),
                 ' (наш ВВОД)' if k['kind'] == 'own' and k['own_vvod'] else
                 ' (наш СБР)' if k['kind'] == 'own' else '',
                 ' '.join(k['first'][i:i + 2] for i in range(0, 10, 2)),
                 format(k['same'], '08b')[::-1], k['events'], k['vvod'],
                 '; ждём своего СБР' if k['pending'] else ''))
    if f.get('fdc'):
        fd = d['fdd']
        print('дисковод: %s; мотор %s; %s%s%s%s'
              % ('A:' if fd['ds0'] else 'B:' if fd['ds1'] else 'не выбран',
                 'крутится' if fd['motor'] else 'стоит',
                 'индекс ' if fd['index'] else '', 'дорожка 0 ' if fd['track0'] else '',
                 'защита ' if fd['protect'] else '',
                 'готов по 34' if fd['ready34'] else 'не готов по 34'))
        c = d.get('fdc')
        if c and len(set(d.get('diag7', ''))) > 2:
            print('ВГ93: команда %02x, состояние %02x (%s), дорожка %d, сектор %d; '
                  'счётчики: байтов %d, меток %d, импульсов %d x256, индексов %d'
                  % (c['cmd'], c['status'], fdc_status_ru(c['cmd'], c['status']),
                     c['track'], c['sector'], c['data_reads'], c['marks'],
                     c['pulses_256'], c['index']))


def fdc_status_ru(cmd, st):
    """Разряды состояния ВГ93 по типу последней команды (WD179x)."""
    t1 = (cmd & 0x80) == 0                  # восстановление, поиск, шаг
    names = (['занят', 'индекс', 'дорожка 0', 'ошибка CRC', 'ошибка поиска',
              'голова', 'защита', 'не готов'] if t1 else
             ['занят', 'DRQ', 'потеря данных', 'ошибка CRC', 'не найден',
              'тип записи', 'защита', 'не готов'])
    on = [n for i, n in enumerate(names) if st >> i & 1]
    return ', '.join(on) or 'чисто'


def main():
    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        print(__doc__)
        return 0

    secret = os.environ.get('COMBO_SECRET', '')
    if not secret:
        print('нет COMBO_SECRET в окружении: export COMBO_SECRET=...',
              file=sys.stderr)
        return 2

    cmd = args[0]
    b = Board(secret)

    if cmd == 'info':
        code, out = b.go('GET', '/api/info')
        d = json.loads(out) if code == 200 else {}
        if code != 200:
            print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
            return 1
        print('прошивка   %s' % d.get('firmware'))
        print('привод     %s, %s' % (d.get('drive'), d.get('model') or '—'))
        print('сигнал     %d дБм  (жёлтый ниже %d, красный ниже %d)'
              % (d.get('rssi', 0), d.get('signal_fair', 0), d.get('signal_poor', 0)))
        print('запись     %s' % ('запрещена' if d.get('read_only') else 'разрешена'))
        # Регистр порта 0x10 иначе с платы не достать: на плате он только для
        # записи. Разряды: [1:0] страница ОЗУ, [3:2] страница стека,
        # [4] стек включён, [5] ОЗУ включено, [6] окно 0x8000-0x9FFF,
        # [7] окно 0xE000-0xFFFF. Липкое показывает, что выставлялось
        # КОГДА-ЛИБО, — одиночную запись мусора видно только по нему.
        if 'kvaz' in d:
            # ЛОВУШКА. Старый битстрим команды 0x38..0x3A
            # не знает, а на неизвестную команду ide_remote оставляет фазу
            # приёма и отдаёт БАЙТ СОСТОЯНИЯ. Тогда все три «регистра» приходят
            # одинаковыми, и расшифровка ниже врёт красиво и убедительно:
            # состояние 0xE0 читается как «ОЗУ вкл, оба окна Баркаря».
            # Признак надёжный: три независимых регистра совпасть не могут.
            #
            # Но одинаковые НУЛИ — это другое, и путать их нельзя. Минимумы
            # наблюдателя стартуют с 255 и записываются только по законченному
            # уровню, самый короткий из которых длится такт; ноль там
            # невозможен по построению. Все нули означают, что ПЛИС не отвечает
            # вовсе, а не что в ней старый образ. Два живых случая: «Вектор»
            # обесточен (плата питается от его пяти вольт, а ESP нет) либо
            # удерживается в сбросе — БЛК+СБР держит в сбросе и ide_remote,
            # а тот на любую команду отдаёт байт состояния, в сбросе нулевой.
            # И ТРЕТИЙ СЛУЧАЙ. Сразу после заливки битстрима все три регистра
            # честно нулевые: управление 0, липкое 0, записей 0 — «Вектор» к
            # квазидиску ещё не обращался. Совпадение здесь настоящее, а не
            # артефакт протокола, и по одним регистрам его не отличить от
            # молчащей ПЛИС, хотя она поднялась сразу и уже мерит шину.
            #
            # Различитель — САМ НАБЛЮДАТЕЛЬ. Его минимумы стартуют с 255 и
            # записываются только по законченному уровню; если там стоит
            # осмысленное число, битстрим свежий и отвечает, что бы ни было в
            # регистрах квазидиска. Старый битстрим этих полей не отдаёт вовсе.
            same  = d['kvaz'] == d['kvaz_seen'] == d['kvaz_writes']
            watch = bool(d.get('ras_lo_min', 0)) or bool(d.get('ras_hi_min', 0))
            # Без наблюдателя (HAS_WATCH 0) его минимумы -- нули, и живость
            # видна по различителю: захват или хотя бы замеренный строб.
            watch = (watch or bool(d.get('vid_state', 0))
                     or bool(d.get('stb_after_max', 0)))
            if same and not watch:
                if d['kvaz'] == 0:
                    print('квазидиск  ПЛИС НЕ ОТВЕЧАЕТ: по SPI приходят одни нули')
                    print('           (минимум ~RAS = 0, а он не может быть нулём —')
                    print('            значит это не показания. «Вектор» выключен')
                    print('            или удерживается в сбросе — БЛК+СБР)')
                else:
                    print('квазидиск  ДИАГНОСТИКИ НЕТ: в ПЛИС старый битстрим')
                    print('           (все три байта пришли одинаковыми — %02X,'
                          ' это байт состояния)' % d['kvaz'])
                return 0
            def bits(v):
                names = ['страница ОЗУ %d' % (v & 3)]
                if v & 0x20: names.append('ОЗУ вкл')
                if v & 0x10: names.append('стек вкл, страница %d' % ((v >> 2) & 3))
                if v & 0x40: names.append('ОКНО 8000-9FFF')
                if v & 0x80: names.append('ОКНО E000-FFFF')
                return ', '.join(names)
            print('квазидиск  %02X  (%s)' % (d['kvaz'], bits(d['kvaz'])))
            print('  видели   %02X  (%s)' % (d['kvaz_seen'], bits(d['kvaz_seen'])))
            def cnt(v):
                return '%d%s' % (v, '+ (насыщен)' if v >= 255 else '')
            print('  записей  %s' % cnt(d['kvaz_writes']))
            if same:
                print('           ВСЕ ТРИ СОВПАЛИ, но ПЛИС мерит шину —')
                print('           значит битстрим свежий, а машина к квазидиску')
                print('           ещё не обращалась. Это НЕ старый образ.')
            print('  обращений: окно A000-DFFF %s, стек %s'
                  % (cnt(d.get('kvaz_std', 0)), cnt(d.get('kvaz_stk', 0))))
            bar = d.get('kvaz_bar', 0)
            if bar:
                print('  ОКНА БАРКАРЯ: %s — плата отвечает за 8000-9FFF и/или'
                      % cnt(bar))
                print('                E000-FFFF, то есть за область экрана и')
                print('                верхнюю память, где живёт MicroDOS')
            else:
                print('  окна Баркаря: 0 — за экран и верхнюю память не отвечаем')
            # Наблюдатель за шиной (bus_watch.v). Ноль здесь — сильное
            # утверждение: ПЛИС смотрит непрерывно, слепого времени у неё нет,
            # в отличие от осциллографа с его окном захвата.
            stk = d.get('stk_unstable')
            ras = d.get('ras_glitch')
            late = d.get('stk_late')
            lo = d.get('ras_lo_min')
            hi = d.get('ras_hi_min')
            if stk is None or ras is None:
                print('  наблюдатель: нет в этой прошивке')
            elif not (stk or ras or late or lo or hi):
                # Минимумы ~RAS не бывают нулями у живого наблюдателя: все нули --
                # это сборка без него (HAS_WATCH 0), а не чистая шина.
                print('  наблюдатель: нет в этой сборке (все поля нулевые) -- '
                      'шина не меряется')
            else:
                print('  наблюдатель, счётчики порогов:')
                print('    СТЕК внутри обращения %s, узких импульсов ~RAS %s'
                      % (cnt(stk), cnt(ras)))
                if late is None or lo is None or hi is None:
                    print('  измерения: нет в этой прошивке')
                elif late == lo == hi:
                    # Второй блок команд старый битстрим не декодирует и
                    # отдаёт вместо него БАЙТ СОСТОЯНИЯ — одно и то же число на
                    # все три запроса. Именно так это и ловится: три
                    # независимых измерения не бывают равны.
                    print('  измерения: нет в этом битстриме '
                          '(три байта пришли одинаковыми — %02X)' % late)
                else:
                    # Минимумы — в тактах 50 МГц, по 20 нс. Это и есть ответ на
                    # вопрос, тот ли порог: не «больше или меньше», а число.
                    print('  наблюдатель, измерения:')
                    print('    СТЕК строго (с выдержкой) %s' % cnt(late))
                    print('    самый короткий ~RAS: низкий %d тактов = %d нс,'
                          % (lo, lo * 20))
                    print('                         высокий %d тактов = %d нс'
                          % (hi, hi * 20))
                    # Минимумы по НАШИМ чтениям.
                    rlmin = d.get('rd_len_min')
                    if rlmin is None:
                        print('    минимумы по нашим чтениям: нет в этой прошивке')
                    elif rlmin == 255:
                        print('    минимумы по нашим чтениям: НИ ОДНОГО нашего '
                              'чтения не видели')
                    else:
                        print('    наши чтения: самое короткое %d тактов = %d нс'
                              % (rlmin, rlmin * 20))
                    # Фаза строба относительно циклов шины. Отвечает на
                    # вопрос: можно ли средствами платы
                    # отличить процессорный цикл от видеоцикла.
                    r2r = d.get('ras_to_rd_min')
                    if r2r is not None:
                        print('    ФАЗА СТРОБА относительно шины:')
                        print('      от спада ~RAS до начала ~ЧТЗУ: %d..%d тактов = %d..%d нс'
                              % (r2r, d['ras_to_rd_max'], r2r * 20,
                                 d['ras_to_rd_max'] * 20))
                        sm = d.get('stb_max')
                        if sm is not None:
                            print('      срабатываний строба между спадами ~RAS: %s'
                                  % ('%d' % sm if sm < 2
                                     else '%d — ФАНТОМНЫЕ ФРОНТЫ' % sm))

                    # ПЕРИОДИЧНОСТЬ ВИДЕОСПАДОВ. По документу Шашкова такт 1
                    # каждого цикла шины — обращение видеоадаптера, значит
                    # видеоспады ~RAS обязаны идти через 1333 нс = 66-67 тактов.
                    sa = d.get('stb_after_min')
                    if sa is not None:
                        sax = d['stb_after_max']
                        sb, sbx = d['stb_before_min'], d['stb_before_max']
                        print('    ПРИВЯЗКА СТРОБА К СПАДУ ~RAS:')
                        print('      от спада до строба:  %d..%d тактов (разброс %d)'
                              % (sa, sax, sax - sa))
                        print('      от строба до спада:  %d..%d тактов (разброс %d)'
                              % (sb, sbx, sbx - sb))
                        # Узкий разброс — там и «свой» спад. Именно этим
                        # решается, какую полярность включать.
                        if (sax - sa) < (sbx - sb):
                            print('      -> строб привязан к ПРЕДЫДУЩЕМУ спаду '
                                  '(полярность A)')
                        elif (sbx - sb) < (sax - sa):
                            print('      -> строб привязан к СЛЕДУЮЩЕМУ спаду '
                                  '(полярность B)')
                        else:
                            print('      -> не разделяется')
                    rd = d.get('rd_ras_max')
                    wr = d.get('wr_ras_max')
                    rl = d.get('rd_len_max')
                    if rd is None:
                        print('    чужие циклы: нет в этой прошивке')
                    else:
                        print('    ЧУЖИХ ЦИКЛОВ ~RAS внутри строба:')
                        print('      чтение %d, запись %d' % (rd, wr))
                        print('      самый длинный ~ЧТЗУ %d тактов = %d нс'
                              % (rl, rl * 20))
            # Различитель видеоцикла (fpga/hdl/vid_phase.v) есть в любой
            # сборке, с наблюдателем или без, поэтому печатается вне блока
            # наблюдателя.
            st = d.get('vid_state')
            if st is not None:
                print('  различитель видеоцикла: захват %s, окно %s, совпадений %d'
                      % ('ЕСТЬ' if st & 1 else 'нет',
                         'поднято' if st & 2 else 'опущено', st >> 4))
                print('    предсказание не сбылось: %d' % d.get('vid_lost', 0))
        if d.get('setup'):
            print('РЕЖИМ НАСТРОЙКИ: плата не в сети и ждёт указаний')
        return 0

    if cmd in ('ce-by-cas', 'ce-by-strobe'):
        # Чем квалифицировать выбор кристалла. Строб не годится: фаза относительно
        # ~RAS плавает в 14 раз. ~CAS метит подмножество циклов и приходит раньше.
        by = 'cas' if cmd == 'ce-by-cas' else 'stb'
        code, out = b.go('POST', '/api/ceby/' + by, b'')
        print('%d: %s  (квалификация по %s)'
              % (code, out.decode('utf-8', 'replace').strip(),
                 '~CAS' if by == 'cas' else 'стробам'))
        return 0 if code == 200 else 1

    if cmd in ('ce-on', 'ce-off'):
        # Квалификация выбора кристалла ОЗУ стробом процессора. Без неё /CE
        # прижат и в чужих циклах регенерации экрана, а столбец адреса идёт
        # живьём и уезжает вслед за ними.
        code, out = b.go('POST', '/api/cegate/' + cmd[3:], b'')
        print('%d: %s  (выбор по стробу %s)'
              % (code, out.decode('utf-8', 'replace').strip(),
                 'включён' if cmd == 'ce-on' else 'выключен'))
        return 0 if code == 200 else 1

    if cmd in ('latch-on', 'latch-off'):
        # Заморозка защёлки адреса на время обращения. Переключается вживую,
        # чтобы сравнить оба поведения на одном битстриме. По сбросу ПЛИС
        # возвращается в выключенное, то есть прежнее.
        code, out = b.go('POST', '/api/latch/' + cmd[6:], b'')
        print('%d: %s  (заморозка %s)'
              % (code, out.decode('utf-8', 'replace').strip(),
                 'включена' if cmd == 'latch-on' else 'выключена'))
        return 0 if code == 200 else 1

    if cmd in ('vid-on', 'vid-off'):
        # Гашение такта защёлки на ПРЕДСКАЗАННОМ видеоспаде ~RAS. Не заморозка:
        # защёлка продолжает тактоваться от процессорных спадов, теряются только
        # видеоадаптерные. Разбор в fpga/hdl/vid_phase.v. По сбросу ПЛИС — выключено.
        code, out = b.go('POST', '/api/vid/' + cmd[4:], b'')
        print('%d: %s  (гашение видеоспада %s)'
              % (code, out.decode('utf-8', 'replace').strip(),
                 'включено' if cmd == 'vid-on' else 'выключено'))
        return 0 if code == 200 else 1

    if cmd in ('vid-pol-a', 'vid-pol-b'):
        # Различитель узнаёт видеоспад по промежутку перед ним
        # (fpga/hdl/vid_phase.v): A -- после длинного, B -- после короткого.
        code, out = b.go('POST', '/api/vid/pol' + cmd[-1], b'')
        print('%d: %s  (видеоспад после %s промежутка)'
              % (code, out.decode('utf-8', 'replace').strip(),
                 'ДЛИННОГО' if cmd.endswith('a') else 'КОРОТКОГО'))
        return 0 if code == 200 else 1

    if cmd == 'board':
        code, out = b.go('GET', '/api/board')
        if code != 200:
            print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
            return 1
        print_board(json.loads(out))
        return 0

    if cmd == 'set':
        q = []
        for a in args[1:]:
            k, _, v = a.partition('=')
            try:
                q += set_arg(k, v)
            except ValueError as e:
                print(e, file=sys.stderr)
                return 2
        if not q:
            print('что менять? combo.py set fdc_rdy=index ...', file=sys.stderr)
            return 2
        code, out = b.go('POST', '/api/board/settings?' + '&'.join(q), b'')
        if code != 200:
            print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
            return 1
        print_board(json.loads(out))
        return 0

    if cmd == 'fdcwatch':
        # Опрос ВГ93 (блок 7) раз в 0,2 с: смена команды или состояния --
        # строкой, счётчики -- приростом за секунду.
        secs = float(args[1]) if len(args) > 1 else 60
        t0 = time.time(); last = None; base = None; tb = t0
        while time.time() - t0 < secs:
            try:
                code, out = b.go('GET', '/api/board')
            except (OSError, http.client.HTTPException) as e:   # плата перезапускается -- ждём и снова
                print('%6.1f  нет связи с платой (%s)' % (time.time() - t0, e))
                time.sleep(2)
                try:
                    b = Board(secret)
                except (OSError, http.client.HTTPException):
                    pass
                base = None
                continue
            c = json.loads(out).get('fdc') if code == 200 else None
            if not c:
                print('%6.1f  нет блока ВГ93: старый битстрим или ПЛИС молчит'
                      % (time.time() - t0))
                time.sleep(1)
                continue
            key = (c['cmd'], c['status'], c['track'], c['sector'])
            now = time.time()
            if key != last:
                print('%6.1f  команда %02x  состояние %02x (%s)  дорожка %d  сектор %d'
                      % (now - t0, c['cmd'], c['status'], fdc_status_ru(*key[:2]),
                         c['track'], c['sector']))
                last = key
            if base is None:
                base = c; tb = now
            elif now - tb >= 1.0:
                dt = now - tb
                dd = {k: ((c[k] - base[k]) & 0xFF) / dt
                      for k in ('data_reads', 'marks', 'pulses_256', 'index')}
                if any(dd.values()):
                    print('        за секунду: байтов %.0f, меток %.0f, импульсов %.0f, индексов %.1f'
                          % (dd['data_reads'], dd['marks'], dd['pulses_256'] * 256, dd['index']))
                base = c; tb = now
            time.sleep(0.2)
        return 0

    if cmd == 'palog':
        code, out = b.go('GET', '/api/palog')
        if code != 200 or len(out) != 2048:
            print('%d: %s' % (code, out[:200].decode('utf-8', 'replace').strip()))
            return 1
        if len(args) > 1:
            with open(args[1], 'wb') as f:
                f.write(out)
        print_palog(out)
        return 0

    if cmd == 'screen':
        if len(args) < 2:
            print('combo.py screen shot.png [palette=<32 hex>]', file=sys.stderr)
            return 2
        pal = None
        for a in args[2:]:
            if a.startswith('palette='):
                h = bytes.fromhex(a[8:])
                if len(h) != 16:
                    print('palette: ровно 16 байт, 32 шестнадцатеричных цифры',
                          file=sys.stderr)
                    return 2
                pal = [vector_rgb(x) for x in h]
        t = time.time()
        code, out = b.go('GET', '/api/screen')
        if code != 200 or len(out) not in (256 * 128, 256 * 128 + 17):
            print('%d: %s' % (code, out[:200].decode('utf-8', 'replace').strip()))
            return 1
        # За кадром -- палитра и порт B, если битстрим их отдаёт (firmware/http.c).
        tail = out[256 * 128:]
        m512 = len(tail) == 17 and bool(tail[16] & 0x10)
        note = '; режим 512' if m512 else ''
        if pal is None and tail:
            pal = [vector_rgb(x) for x in tail[:16]]
            note += '; палитра машины %s' % tail[:16].hex()
        elif pal is None:
            pal = STOCK
            note += '; цвета условные: битстрим палитру не отдаёт (palette= -- свои)'
        with open(args[1], 'wb') as f:
            f.write(screen_png(out[:256 * 128], pal, m512))
        side = 512 if m512 else 256
        print('%s: %dx%d, %.2f с%s' % (args[1], side, side, time.time() - t, note))
        return 0

    if cmd in ('keys', 'loader'):
        if len(args) < 2:
            print('combo.py %s %s' % (cmd, 'vvod|sbr|release' if cmd == 'keys'
                                       else 'arm|boot|off'), file=sys.stderr)
            return 2
        code, out = b.go('POST', '/api/board/%s?k=%s' % (cmd, args[1]), b'')
        if code != 200:
            print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
            return 1
        d = json.loads(out)
        print('клавиши: ВВОД %s, СБР %s; загрузчик: %s'
              % ('нажат' if d['keys']['vvod'] else 'отпущен',
                 'нажат' if d['keys']['sbr'] else 'отпущен',
                 'окно' if d['loader']['window'] else
                 'взведён' if d['loader']['armed'] else 'снят'))
        return 0

    if cmd == 'diag-clear':
        code, out = b.go('POST', '/api/diag/clear', b'')
        print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
        return 0 if code == 200 else 1

    # ПЛИС пишет сама плата, по SSPI (firmware/sspi.c). Файл разбирается здесь
    # и уезжает частями по 32 КБ -- столько тело запроса у платы и принимает.
    if cmd == 'fpga':
        code, out = b.go('GET', '/api/fpga')
        info = json.loads(out) if code == 200 else {}
        if info.get('sspi'):
            rest = [a_ for a_ in args[1:] if not a_.startswith('--')]
            if '--detect' in args:
                print('SSPI: IDCODE %s, регистр состояния %s, DONE %s'
                      % (info['idcode'], info['status'], 'да' if info['done'] else 'НЕТ'))
                return 0
            if not rest:
                print('нужно: fpga --detect | fpga файл.jed', file=sys.stderr)
                return 2
            try:
                img, part, rows = jed_image(rest[0])
            except (OSError, ValueError) as e:
                print('%s: %s' % (rest[0], e), file=sys.stderr)
                return 2
            print('%s: %s, %d страниц, %d байт образа' % (rest[0], part, rows, len(img)))
            t = time.time()
            for off in range(0, len(img), 32768):
                code, out = b.go('POST', '/api/fpga/part?off=%d' % off, img[off:off + 32768])
                if code != 200:
                    print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
                    return 1
            q = 'refresh=%d&force=%d&verify=%d' % ('--no-refresh' not in args, '--force' in args,
                                                   '--no-verify' not in args)
            code, out = b.go('POST', '/api/fpga/flash?' + q, b'')
            d = json.loads(out) if out[:1] == b'{' else {}
            if code != 200:
                print('%d: %s' % (code, d.get('error') or out.decode('utf-8', 'replace').strip()))
                if code == 409 and d.get('feature'):
                    print('  в кристалле: строка настроек %s, FEABITS %s; код %s'
                          % (d['feature'], d['feabits'], d['idcode']))
                return 1
            print('залито за %.1f с: стирание %d мс, запись %d мс, сверка %d мс%s'
                  % (time.time() - t, d['erase_ms'], d['prog_ms'], d['verify_ms'],
                     '' if '--no-refresh' in args else
                     '; ПЛИС перезагружена -- машина перезагрузится вместе с ней'))
            return 0
        if code == 200:
            print('SSPI на этой плате нет: ПЛИС не прошить', file=sys.stderr)
        else:
            print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()),
                  file=sys.stderr)
        return 1

    if cmd in ('wifi', 'log'):
        code, out = b.go('GET', '/api/' + cmd)
        print(out.decode('utf-8', 'replace').rstrip())
        return 0 if code == 200 else 1

    if cmd == 'ota':
        if len(args) < 2:
            print('чем обновлять? укажите файл', file=sys.stderr)
            return 2
        data = open(args[1], 'rb').read()
        www = os.path.basename(args[1]) == 'www.bin'
        path = '/api/ota/www' if www else '/api/ota'
        print('%s: %.0f КБ → %s' % (args[1], len(data) / 1024,
                                    'статика морды' if www else 'прошивка'))
        t = time.time()
        code, out = b.go('POST', path, data)
        d = time.time() - t
        print('%d: %s  (%.1f с, %.0f КБ/с)'
              % (code, out.decode('utf-8', 'replace').strip(), d,
                 len(data) / 1024 / max(d, .001)))
        if code == 200 and not www:
            print('прошивка принята — перезагрузите: combo.py reboot')
        return 0 if code == 200 else 1

    if cmd == 'update':
        # `/api/update` ОТДАЁТ КЭШ, а не спрашивает GitHub. Плата ходит туда раз
        # в сутки и при старте, поэтому сразу после выпуска и здесь, и в морде
        # ещё висит прежняя версия — не потому, что что-то сломалось.
        # `--check` заставляет спросить заново.
        if '--check' in args:
            code, out = b.go('POST', '/api/update/check')
            if code != 200:
                print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
                return 1
        code, out = b.go('GET', '/api/update')
        if code != 200:
            print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
            return 1
        d = json.loads(out)
        if d.get('error'):
            print('проверка не удалась: %s' % d['error'])
        elif not d.get('checked'):
            print('плата ещё не спрашивала — первая проверка через минуту после старта')
        elif d.get('available'):
            # app и fpga -- по отдельности: бывает, что прошивка ESP
            # та же, а ПЛИС в раздаче другая (firmware/update.c).
            what = [w for w, k in (('прошивка ESP', 'app'), ('ПЛИС', 'fpga')) if d.get(k)]
            print('выпущено: %s; другое у платы: %s' % (d['version'],
                  ', '.join(what) or 'прошивка ESP'))
        else:
            print('обновлять нечего: у платы %s' % d['version'])

        if '--apply' not in args:
            if d.get('available'):
                print('поставить: combo.py update --apply')
            return 0
        if not d.get('available'):
            print('нечего ставить', file=sys.stderr)
            return 1

        # Долго: плата качает пару мегабайт с GitHub и всё это время занята.
        # С ПЛИС дольше: стирание флеша кристалла до минуты, потом запись и
        # сверка; «Вектор» при этом перезагрузится.
        print('плата качает и ставит%s — это займёт %s…' % (
              ' и прошивает ПЛИС («Вектор» перезагрузится)' if d.get('fpga') else '',
              'несколько минут' if d.get('fpga') else 'с минуту'))
        t = time.time()
        # Ответ приходит в конце: 180 с на всё может не хватить с ПЛИС.
        b.c.timeout = 600
        if b.c.sock:
            b.c.sock.settimeout(600)
        code, out = b.go('POST', '/api/update/apply')
        print('%d: %s  (%.0f с)' % (code, out.decode('utf-8', 'replace').strip(),
                                    time.time() - t))
        if code == 200 and b'"reboot":"none"' not in out:
            print('перезагрузите: combo.py reboot')
        return 0 if code == 200 else 1

    if cmd == 'reboot':
        try:
            code, out = b.go('POST', '/api/reboot')
            print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
        except OSError:
            # Плата уходит в перезагрузку сразу за ответом, и он не всегда
            # успевает дойти. Обрыв здесь — не беда, а то самое, чего просили.
            print('соединение оборвалось — плата перезагружается')
        return 0

    if cmd == 'forget':
        if '--yes' not in args:
            print('ЭТО УВЕДЁТ ПЛАТУ ИЗ СЕТИ.\n'
                  'Она поднимет свою точку combolite-XXXX, и вернуть её\n'
                  'можно будет только с телефона, стоя рядом, — или шнурком.\n'
                  'Если согласны: combo.py forget --yes', file=sys.stderr)
            return 2
        try:
            code, out = b.go('POST', '/api/wifi/forget')
            print('%d: %s' % (code, out.decode('utf-8', 'replace').strip()))
        except OSError:
            print('соединение оборвалось — плата перезагружается в настройку')
        print('дальше: подключитесь к combolite-XXXX и откройте http://192.168.4.1/')
        return 0

    print('не знаю такой команды: %s' % cmd, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())
