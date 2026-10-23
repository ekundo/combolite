/* Язык морды.
 *
 * Ключ перевода — сама английская строка. Так в коде остаётся читаемый текст,
 * а не выдуманные имена вроде `panel.erase.confirm`: по строке видно, что
 * будет на экране, и не приходится держать в голове второй словарь — из имён
 * в смыслы. Цена — правка английского текста ломает перевод; ловится это
 * проверкой на полноту.
 *
 * Подстановки нумерованные, %1 и %2, а не по порядку: в русском порядок слов
 * другой, и переводчик обязан иметь право их переставить.
 *
 * Английский словаря не имеет вовсе: он и есть ключи.
 */

export const LANGS = { en: 'English', ru: 'Русский' };

/* Число по-русски: 1 файл, 2 файла, 5 файлов. Три формы вместо двух — это не
 * прихоть, «5 файл» читается как ошибка, а не как перевод. */
export function ru_plural(n, forms) {
  const a = Math.abs(n) % 100, b = a % 10;
  if (a > 10 && a < 20) return forms[2];
  if (b === 1)          return forms[0];
  if (b >= 2 && b <= 4) return forms[1];
  return forms[2];
}

/* Ключ — английское единственное число, ровно то, что стоит в вызове plural. */
export const RU_FORMS = {
  'file':           ['файл', 'файла', 'файлов'],
  'entry':          ['запись', 'записи', 'записей'],
  'disk':           ['диск', 'диска', 'дисков'],
  'block':          ['блок', 'блока', 'блоков'],
  'directory entry': ['запись каталога', 'записи каталога', 'записей каталога'],
  'slot':           ['слот', 'слота', 'слотов'],
  'item':           ['позиция', 'позиции', 'позиций'],
  'change':         ['изменение', 'изменения', 'изменений'],
  'pending change': ['неприменённое изменение', 'неприменённых изменения',
                     'неприменённых изменений'],
  'damaged entry':  ['испорченная запись', 'испорченные записи', 'испорченных записей'],
  'empty disk':   ['пустой диск', 'пустых диска', 'пустых дисков'],
};

/* Состояния привода — их присылает плата (drive_word в firmware/http.c) — и роды
 * файлов приходят в t() ПЕРЕМЕННОЙ, а не строкой. Извлекатель ключей таких не
 * видит, поэтому они живут здесь: и морда берёт их отсюда, и проверка полноты
 * знает, что их тоже надо перевести. */
export const DRIVE_WORDS = ['unknown', 'resetting', 'ready', 'dead'];

export const KIND_NAME = {
  exec: 'program', music: 'music', code: 'source code', batch: 'batch file',
  config: 'settings', image: 'picture', font: 'font', text: 'text', data: 'file',
};

/* Справка: что морда умеет, парами «что нажать — что будет».
 *
 * Живёт ЗДЕСЬ, а не в морде, по той же причине, что и роды файлов: строки
 * приходят в t() переменной, и извлекатель ключей их не видит. Отсюда их
 * берёт и морда, и проверка полноты — иначе непереведённой справку заметил бы
 * только человек, открывший её на русском.
 */
export const HELP = [
  ['Disks', [
    ['click a disk', 'opens it and shows what is on it'],
    ['drag a disk onto another', 'swaps them: contents and label move, ' +
                                 'the numbers stay with their places'],
    ['the icons on a disk', 'save it as a .fdd image, or erase its directory ' +
                            '— erasing asks twice'],
    ['show empty', 'reveals disks with nothing on them; while dragging, ' +
                   'hidden ones appear in the gaps so you can drop into them'],
    ['numbers are hexadecimal', 'as the Vector itself calls them — 1, 2 … 9, ' +
                                'A, B … F, 10'],
  ]],
  ['Files', [
    ['click a file', 'selects it'],
    ['Ctrl or Cmd + click', 'adds one to the selection'],
    ['Shift + click', 'selects everything from the last one clicked'],
    ['drag onto another disk', 'copies — the source keeps its files'],
    ['drag into another user area', 'moves, if it is the same disk: ' +
                                    'an area is one byte in the directory entry'],
    ['the "Delete N files" button', 'removes the selected ones; all or nothing'],
    ['the icons on a file', 'download it, or delete just that one'],
  ]],
  ['Bringing files in', [
    ['drop files onto a disk', 'adds them; names become 8.3 in upper case'],
    ['drop a .fdd onto an empty disk', 'lays the whole image on it; ' +
                                       'the label comes from the file name'],
    ['a full disk refuses', 'the whole batch is rejected, not half of it'],
  ]],
  ['The archive', [
    ['drag disks or files onto the archive icon', 'puts them in; they are read ' +
      'right away, so later changes do not affect what you saved'],
    ['click the archive icon', 'shows what is inside and saves it as one .zip'],
    ['the archive holds up to 128 MB', 'it lives in this tab\'s memory; ' +
      'for more, save in parts'],
  ]],
  ['Finding', [
    ['*.COM or NAME', 'a pattern, as the Vector writes it: * for any run, ' +
                      '? for one character'],
    ['it matches whole names', 'ZORK does not find ZORK.COM — write ZORK.* ' +
                               'or ZORK.COM'],
    ['sort by', 'orders files inside a disk: by name, by kind, by size'],
  ]],
  ['Writing', [
    ['nothing is written until Apply', 'everything you do is kept aside; ' +
      'Discard throws it all away, Undo changes throws away one disk\'s'],
    ['the “Undo” button', 'takes back one step at a time — the button says ' +
      'which one. Handy when a drag lands on the wrong disk'],
    ['permission to write', 'the board takes it by itself, only for the seconds ' +
      'it writes, and gives it back at once'],
    ['read-only', 'a badge in the header, and only if the board was flashed that ' +
      'way: then nothing can be written at all'],
    ['drive ready', 'click it for the model, size and what the board says ' +
                    'about writing'],
  ]],
  ['The network', [
    ['the dBm badge', 'signal strength, right in the header. Its colour says ' +
      'what the board will do: green — it stays put; yellow — it moves if the ' +
      'network offers to; red — it moves on its own, breaking connections'],
    ['the air', 'every access point with your network name, and which one the ' +
      'board uses. The scan takes a couple of seconds and holds the board'],
    ['the log', 'what the board has written down — why it did not connect, ' +
      'which point it chose, why the drive did not wake up'],
  ]],
  ['Keys', [
    ['Ctrl+Z, ⌘Z on a Mac', 'one step back, same as the Undo button'],
    ['Enter in the label field', 'saves the label'],
    ['Escape in the label field', 'puts the old one back'],
    ['Escape', 'closes this help and the pop-ups'],
    ['that is all of them', 'the rest is done with the mouse'],
  ]],
];
export const RU = {
  /* Названия шагов отката. Каждое отвечает на вопрос «что сейчас отменится»,
     поэтому это существительные и обороты, а не команды. */
  'Undo':                         'Шаг назад',
  'the “Undo” button':            'кнопка «Шаг назад»',
  'The network':                  'Сеть',
  'the dBm badge':                'плашка с дБм',
  'signal strength, right in the header. Its colour says what the board will do: green — it stays put; yellow — it moves if the network offers to; red — it moves on its own, breaking connections':
    'сила сигнала прямо в шапке. Цвет говорит, что плата собирается делать: зелёный — останется на месте; жёлтый — уйдёт, если сеть переведёт её сама; красный — будет уходить своими силами, разрывая соединения',
  'every access point with your network name, and which one the board uses. The scan takes a couple of seconds and holds the board':
    'все точки с именем вашей сети и та, которой плата пользуется. Обзор идёт пару секунд и всё это время занимает плату',
  'what the board has written down — why it did not connect, which point it chose, why the drive did not wake up':
    'что плата записала: почему не подключилась, какую точку выбрала, отчего не ожил привод',
  'takes back one step at a time — the button says which one. Handy when a drag lands on the wrong disk':
    'отменяет по одному действию за раз, и на кнопке написано, по какому. '
    + 'Пригождается, когда перетаскивание попало не на тот диск',
  'Ctrl+Z, ⌘Z on a Mac':          'Ctrl+Z, на маке ⌘Z',
  'one step back, same as the Undo button':
                                  'шаг назад, то же самое, что кнопка',
  'undo the last step: %1':       'отменить последнее: %1',
  'undone: %1':                   'отменено: %1',
  'nothing to undo':              'отменять нечего',
  '%1 to disk %2':                '%1 на диск %2',
  'move to user area %1':         'перенос в область %1',
  'image on disk %1':             'образ на диске %1',
  'swap disks %1 and %2':         'обмен дисков %1 и %2',
  'undo swap on disk %1':         'отмена обмена на диске %1',
  'erase disk %1':                'очистка диска %1',
  'delete %1':                    'удаление: %1',
  'discard all changes':          'отмену всех правок',

  'the Vector touched the drive': '«Вектор» трогал диск',
  'the Vector wrote to the disk itself — what you see may be stale':
    '«Вектор» сам писал на диск — показанное могло устареть. Нажмите, чтобы перечитать',
  'apply or discard your changes first': 'сперва примените или отмените правки',
  'the Vector is working with the disk — try again in a moment':
    '«Вектор» сейчас работает с диском — попробуйте через минуту',

  /* --- обход диска и поиск */
  'scanned %1 of %2…':            'прочитано %1 из %2…',
  'read %1 in %2 s':           'прочитано %1 за %2 с',
  '%1, %2 with files':            '%1, из них с файлами %2',
  ', %1 failed':                  ', не прочитано %1',
  'find file: *.COM or NAME':     'найти файл: *.COM или ИМЯ',
  'show empty':                   'показывать пустые',
  'searching the %1 disks read so far — the rest are not read yet':
    'ищем по прочитанным дискам (%1) — остальные ещё не прочитаны',
  'disks with this file: %1':  'дисков с таким файлом: %1',
  '%1 of %2 match':               'совпало %1 из %2',
  '%1 hidden here — hover to show': 'здесь спрятано %1 — наведите, чтобы показать',

  /* --- шапка */
  'connecting…':                  'соединяемся…',
  'source':                       'источник',
  'serial':                       'серийный номер',
  'capacity':                     'объём',
  'sector':                       'сектор',
  'writes':                       'запись',
  'signal strength, and what the board sees around':
                                  'сила сигнала и что плата видит вокруг',
  'in the release: %1':           'в раздаче: %1',
  'the published firmware differs from the one running':
                                  'в раздаче лежит не та версия, что работает',
  'slot':                         'раздел',
  'firmware version; the board booted from %1':
                                  'версия прошивки; плата загрузилась из %1',
  'published':                    'выпущена',
  'running now':                  'сейчас работает',
  'The board downloads it itself and checks the signature. The old firmware stays in the spare slot: if the new one does not start, the board comes back to it by itself.':
    'Плата скачает её сама и проверит подпись. Прежняя останется в запасном разделе: если новая не запустится, плата вернётся к ней сама.',
  'install and restart':          'поставить и перезагрузить',
  'downloading… this takes a minute': 'скачиваем… это займёт с минуту',
  'downloading and flashing the FPGA… this takes a few minutes':
                                  'скачиваем и прошиваем ПЛИС… это займёт несколько минут',
  'installed':                    'поставлено',
  'FPGA':                         'ПЛИС',
  'will be reflashed':            'будет перешита',
  'Flashing the FPGA restarts the machine: save what you are working on.':
    'Прошивка ПЛИС перезагрузит «Вектор»: сохраните, что в работе.',
  'installed — restarting the board': 'поставлено — перезагружаем плату',
  'network':                      'сеть',
  'Set up the board':             'Настройка платы',
  'The board could not join a network, so it put up its own. Pick yours and give the password — it is stored on the board, not here.':
    'Плата не смогла подключиться к сети и подняла свою. Выберите вашу и введите пароль — он останется на плате, а не здесь.',
  'Write down the board secret — you will need it to sign in, and it is shown only now: %1':
    'Запишите секрет платы: он нужен для входа и показывается только сейчас — %1',
  'the secret':                   'секрет',
  'look again':                   'посмотреть ещё раз',
  'looking around…':              'смотрим вокруг…',
  'nothing in the air':           'в эфире пусто',
  'open':                         'без пароля',
  'password':                     'пароль',
  'Join':                         'Подключиться',
  'joining…':                     'подключаемся…',
  'Saved':                        'Готово',
  'The board is restarting and joining “%1”. It will get its address from your router — look for it there, or plug the board in and read the log. This page is served by the board itself and will stop answering.':
    'Плата перезагружается и подключается к «%1». Адрес она возьмёт у роутера — посмотрите там, либо подключите плату шнурком и прочтите журнал. Эту страницу отдаёт сама плата, и отвечать она перестанет.',
  'the air':                      'эфир',
  'the log':                      'журнал',
  'asking the board…':            'спрашиваем плату…',
  'the scan takes a couple of seconds and holds the board':
                                  'обзор идёт пару секунд и всё это время занимает плату',
  '%1 sectors':                   '%1 секторов',
  '%1 MB':                        '%1 МБ',
  '%1 bytes':                     '%1 байт',
  '%1 dBm':                       '%1 дБм',
  'refused — the board is read-only': 'запрещена — плата только на чтение',
  'allowed':                      'разрешена',
  'allowed — no lock at all':     'разрешена — замка нет вовсе',
  'what the board says about the drive': 'что плата говорит о приводе',
  'drive %1':                     'привод %1',
  'the board refuses writes — it was flashed read-only':
    'плата не пишет — она прошита только на чтение',
  'this board is flashed read-only': 'плата прошита только на чтение',
  'read-only':                    'только чтение',
  'unknown':                      'не опрошен',
  'resetting':                    'сбрасывается',
  'ready':                        'готов',
  'dead':                         'не отвечает',

  /* --- корзина архива */
  'archive':                      'архив',
  '%1 · %2 — click to see and save': '%1 · %2 — кликните, чтобы посмотреть и скачать',
  'drag disks and files here, then save them as one .zip':
    'перетащите сюда диски и файлы, потом заберите одним .zip',
  ['Nothing here yet. Drag disks or files onto the archive icon — they are read ' +
  'right away, so whatever happens to them later, you get what you saw here.']:
    'Пока пусто. Перетащите на значок архива диски или файлы — они читаются ' +
    'сразу, и что бы потом ни случилось с диском, заберёте вы именно их.',
  'remove':                       'убрать',
  'remove from the archive':      'убрать из архива',
  'Clear all':                    'Очистить',
  'Save .zip':                    'Скачать .zip',
  'packing…':                     'упаковываем…',
  'disk %1 is already in the archive': 'диск %1 уже в архиве',
  'disk %1 added to the archive':      'диск %1 добавлен в архив',
  '%1 added to the archive':      '%1 добавлено в архив',
  'already in the archive':       'уже в архиве',
  ['the archive would grow past %1 — save what is there and start over']:
    'архив перевалит за %1 — заберите собранное и начните заново',

  /* --- вход */
  'Sign in':                      'Вход',

  /* --- справка */
  'What this can do':             'Что здесь умеют',
  'Close':                        'Закрыть',
  /* --- справка */
  'what this can do':             'что здесь умеют',
  'help':                         'справка',
  'dark theme':                   'тёмная тема',
  'light theme':                  'светлая тема',
  'Disks':                        'Диски',
  'click a disk':                 'клик по диску',
  'opens it and shows what is on it': 'раскрывает его и показывает, что на нём',
  'drag a disk onto another':     'перетащить диск на диск',
  ['swaps them: contents and label move, the numbers stay with their places']:
    'меняет их местами: едут содержимое и метка, номера остаются за местами',
  'the icons on a disk':          'значки на плитке диска',
  ['save it as a .fdd image, or erase its directory — erasing asks twice']:
    'сохранить образом .fdd или очистить каталог — очистка переспрашивает',
  ['reveals disks with nothing on them; while dragging, hidden ones appear ' +
   'in the gaps so you can drop into them']:
    'показывает диски, на которых ничего нет; во время перетаскивания спрятанные ' +
    'проступают в разрывах, чтобы можно было бросить в них',
  'numbers are hexadecimal':      'номера шестнадцатеричные',
  ['as the Vector itself calls them — 1, 2 … 9, A, B … F, 10']:
    'так их зовёт сам «Вектор» — 1, 2 … 9, A, B … F, 10',

  'Files':                        'Файлы',
  'click a file':                 'клик по файлу',
  'selects it':                   'выделяет его',
  'Ctrl or Cmd + click':          'Ctrl или Cmd + клик',
  'adds one to the selection':    'добавляет один к выделенному',
  'Shift + click':                'Shift + клик',
  'selects everything from the last one clicked':
    'выделяет всё от последнего нажатого',
  'drag onto another disk':       'перетащить на другой диск',
  'copies — the source keeps its files': 'копирует — на источнике файлы остаются',
  'drag into another user area':  'перетащить в другую область',
  ['moves, if it is the same disk: an area is one byte in the directory entry']:
    'переносит, если диск тот же: область — это один байт в записи каталога',
  'the "Delete N files" button':  'кнопка «Удалить N файлов»',
  'removes the selected ones; all or nothing':
    'убирает выделенные; всё или ничего',
  'the icons on a file':          'значки на плитке файла',
  'download it, or delete just that one': 'скачать его или удалить только его',

  'Bringing files in':            'Как положить файлы',
  'drop files onto a disk':       'бросить файлы на диск',
  'adds them; names become 8.3 in upper case':
    'кладёт их; имена становятся 8.3 заглавными',
  'drop a .fdd onto an empty disk': 'бросить .fdd на пустой диск',
  ['lays the whole image on it; the label comes from the file name']:
    'кладёт весь образ целиком; метка берётся из имени файла',
  'a full disk refuses':          'на забитый диск не влезет',
  'the whole batch is rejected, not half of it':
    'отвергается вся пачка, а не половина',

  'The archive':                  'Архив',
  'drag disks or files onto the archive icon':
    'перетащить диски или файлы на значок архива',
  ['puts them in; they are read right away, so later changes do not affect ' +
   'what you saved']:
    'кладёт их туда; они читаются сразу, и что бы потом ни случилось с диском, ' +
    'заберёте вы именно их',
  'click the archive icon':       'клик по значку архива',
  'shows what is inside and saves it as one .zip':
    'показывает содержимое и отдаёт его одним .zip',
  'the archive holds up to 128 MB': 'в архив влезает до 128 МБ',
  ["it lives in this tab's memory; for more, save in parts"]:
    'он лежит в памяти этой вкладки; больше — забирайте частями',

  'Finding':                      'Поиск',
  '*.COM or NAME':                '*.COM или ИМЯ',
  ['a pattern, as the Vector writes it: * for any run, ? for one character']:
    'образец, как их пишет сам «Вектор»: * — сколько угодно знаков, ? — один',
  'it matches whole names':       'совпадает имя целиком',
  'ZORK does not find ZORK.COM — write ZORK.* or ZORK.COM':
    'ZORK не найдёт ZORK.COM — пишите ZORK.* или ZORK.COM',
  'sort by':                      'порядок',
  'orders files inside a disk: by name, by kind, by size':
    'как показывать файлы внутри диска: по имени, по типу, по размеру',

  'Writing':                      'Запись',
  'nothing is written until Apply': 'до «Применить» ничего не пишется',
  ["everything you do is kept aside; Discard throws it all away, " +
   "Undo changes throws away one disk's"]:
    'всё сделанное копится в стороне; «Отменить» выбрасывает всё, «Отменить ' +
    'правки» — только по одному диску',
  'permission to write':          'разрешение на запись',
  ['the board takes it by itself, only for the seconds it writes, and gives it back at once']:
    'плата берёт его сама, только на те секунды, что пишет, и сразу возвращает',
  ['a badge in the header, and only if the board was flashed that way: then nothing ' +
   'can be written at all']:
    'плашка в шапке, и только если плата так прошита: тогда записать нельзя ничего',
  'drive ready':                  'привод готов',
  'click it for the model, size and what the board says about writing':
    'кликните, чтобы увидеть модель, объём и что плата говорит о записи',

  'Keys':                         'Клавиши',
  'Enter in the label field':     'Enter в поле метки',
  'saves the label':              'сохраняет метку',
  'Escape in the label field':    'Escape в поле метки',
  'puts the old one back':        'возвращает прежнюю',
  'Escape':                       'Escape',
  'closes this help and the pop-ups': 'закрывает эту справку и всплывающие окна',
  'that is all of them':          'это все',
  'the rest is done with the mouse': 'остальное делается мышью',
  'secret':                       'секрет',
  '128-bit hex':                  '128 бит шестнадцатеричными',
  ['The board secret — printed to its console on first start. Needed once: the ' +
  'session key is derived from it and the board\'s challenge, and lives until this ' +
  'tab is closed. The secret itself is not stored here.']:
    'Секрет платы — она печатает его в свою консоль при первом запуске. Нужен один ' +
    'раз: из него и вызова платы выводится ключ сеанса, и живёт он, пока открыта ' +
    'эта вкладка. Сам секрет здесь не хранится.',
  ['Neither the secret nor the key travels over the network — the board derives the ' +
  'same key on its side. Only request signatures go on the wire, and they cannot ' +
  'be replayed.']:
    'Ни секрет, ни ключ по сети не ходят — плата выводит тот же ключ у себя. ' +
    'По проводу идут только подписи запросов, и повторить их нельзя.',
  'unlock failed: %1':            'замок не снялся: %1',

  /* --- плитка диска */
  'Disk %1':                    'Диск %1',
  'failed':                       'не прочитана',
  'empty':                        'пусто',
  'save as .fdd':                 'сохранить как .fdd',
  'save this disk as a .fdd image': 'сохранить этот диск образом .fdd',
  'erase disk':                 'очистить диск',
  'no label':                     'без метки',
  'disk label':                   'метка диска',
  'click again to erase the directory': 'кликните ещё раз, чтобы очистить каталог',
  'erase the whole directory; data blocks are left as they are':
    'очистить весь каталог; блоки данных остаются как есть',
  'disk %1: erased %2':         'диск %1: очищено %2',
  'shows disk %1 — swap not applied yet':
    'показан диск %1 — перестановка ещё не применена',

  /* --- панель */
  '%1 (%2 KB) and %3 free':       'свободно %1 (%2 КБ) и %3',
  ', %1 damaged':                 ', испорчено %1',
  '%1 selected — drag to another disk to copy':
    'выделено %1 — перетащите на другой диск, чтобы скопировать',
  'Delete %1':                    'Удалить %1',
  'remove the selected files from this disk': 'убрать выделенные файлы с этого диска',
  '%1 deleted — not written yet': '%1 удалено — ещё не записано',
  'show an empty user area so files can be dropped into it':
    'показать пустую область пользователя, чтобы было куда положить',
  '+ area':                       '+ область',
  'user %1':                      'область %1',
  'Add files…':                   'Добавить файлы…',
  ['lost directory entries: neither free nor a file. Rewriting them to E5h gives ' +
  'them back']:
    'потерянные записи каталога: ни свободные, ни файл. Переписав их в E5h, ' +
    'их возвращают в оборот',
  'repaired %1':                  'починено %1',
  'Repair %1':                    'Починить %1',
  'Undo changes':                 'Отменить правки',
  'Undo swap':                    'Отменить перестановку',
  'sort by:':                     'порядок:',
  'how files are ordered in a disk': 'в каком порядке показывать файлы',
  'name':                         'по имени',
  'type':                         'по типу',
  'size':                         'по размеру',
  'reading directory…':           'читаем каталог…',
  'disk is empty':              'диск пуст',
  'empty — drop files here':      'пусто — перетащите файлы сюда',
  ['%1 left alone: they point at real blocks and may be a file whose first byte got ' +
  'clobbered. Freeing them would hand those blocks out for reuse.']:
    '%1 не тронуты: они ссылаются на настоящие блоки и могут оказаться файлом, ' +
    'которому затёрли первый байт. Освободив их, эти блоки отдадут под запись.',
  'this character has no place in KOI-8': 'такого знака нет в КОИ-8',

  /* --- роды файлов */
  'program':                      'программа',
  'music':                        'музыка',
  /* «source» уже занято строкой в балуне привода — там это источник данных.
     Род файла назван полнее: и по-английски точнее, и столкновения нет. */
  'source code':                  'исходник',
  'batch file':                   'пакетный файл',
  'settings':                     'настройки',
  'picture':                      'картинка',
  'font':                         'шрифт',
  'text':                         'текст',
  'file':                         'файл',

  /* --- файлы: перенос, скачивание, отказы */
  '%1: %2 of %3 blocks…':         '%1: %2 из %3 блоков…',
  '%1: %2 bytes':                 '%1: %2 байт',
  '(not written yet)':            '(ещё не записано)',
  'not written yet':              'ещё не записано',
  'R — read-only, the system refuses to change it':
    'R — только чтение, система не даст его изменить',
  'S — system, DIR does not list it':
    'S — системный, DIR его не показывает',
  'download this file':           'скачать этот файл',
  'delete this file':             'удалить этот файл',
  'download':                     'скачать',
  'delete':                       'удалить',
  'already in user area %1':      'уже в области %1',
  '%1 moved to user area %2 — not written yet':
    '%1 перенесено в область %2 — ещё не записано',
  '%1 is gone from disk %2':    '%1 больше нет на диске %2',
  'copied, but disk %1 left as it was: %2':
    'скопировано, но диск %1 остался как был: %2',
  'disk %1':                    'диск %1',
  'user area %1':                 'область %1',
  'disk %1, user area %2':      'диск %1, область %2',
  '%1 moved to %2 — not written yet':  '%1 перенесено на %2 — ещё не записано',
  '%1 copied to %2 — not written yet': '%1 скопировано на %2 — ещё не записано',
  'nothing changed':              'ничего не изменилось',
  ', %1 of %2 refused':           ', отказано %1 из %2',
  '%1 and %2 more: %3':           '%1 и ещё %2: %3',
  'added %1':                     'добавлено %1',
  'to disk %1 — not written yet': 'на диск %1 — ещё не записано',

  /* --- применение */
  '%1 (%2 of %3)…':               '%1 (%2 из %3)…',
  'disk %1 ← %2':                 'диск %1 ← %2',
  'disk %1: %2 of %3 sectors…': 'диск %1: %2 из %3 секторов…',
  'disk %1 changed since it was read':
    'диск %1 успели изменить после того, как его прочитали',
  'label on disk %1':           'метка диска %1',
  'swapping disks %1 and %2':  'меняем местами диски %1 и %2',
  'swapping %1':               'переставляем %1',
  'swapping %1: %2 of %3 sectors…': 'перестановка %1: %2 из %3 секторов…',
  'apply stopped: %1':            'применение остановлено: %1',
  'stopped after %1 of %2: %3':   'остановлено после %1 из %2: %3',
  ['written, but the drive did not confirm the flush (%1) — do not power off until ' +
  'it does']:
    'записано, но привод не подтвердил сброс кэша (%1) — не выключайте питание, ' +
    'пока не подтвердит',
  'applied %1':                   'применено %1',
  'Apply':                        'Применить',
  'Discard':                      'Отменить',
  '%1 recovered':                 'возвращено %1',
  'disks %1 ⇄ %2':             'диски %1 ⇄ %2',
  'disks %1 → %2':             'диски %1 → %2',
  'label on disk %1 → “%2”':    'метка диска %1 → «%2»',
  'clear label on disk %1':     'снять метку с диска %1',

  /* --- образы .fdd */
  'directory not read yet':       'каталог ещё не прочитан',

  /* --- отказы раскладчика и разбора образа (www/disk.mjs, www/fdd.mjs) */
  'more than one dot in "%1"':    'больше одной точки в «%1»',
  'empty name':                   'пустое имя',
  'name "%1" is longer than 8 characters': 'имя «%1» длиннее восьми знаков',
  'extension "%1" is longer than 3 characters':
    'расширение «%1» длиннее трёх знаков',
  'character "%1" has no place in a CP/M name':
    'знак «%1» в имя CP/M не ложится',
  'no file "%1" in user area %2': 'файла «%1» в области %2 нет',
  'user area %1 is outside 0..%2': 'области %1 не бывает, они от 0 до %2',
  '%1 already exists in user area %2': '%1 уже есть в области %2',
  'needs %1, %2 free':            'нужно %1, свободно %2',
  'the CP/M area must be %1 or %2 sectors':
    'файловая область должна быть %1 или %2 секторов',
  'the system tracks must be %1 sectors':
    'системных дорожек должно быть %1 секторов',
  'the image length is not a multiple of 512':
    'длина образа не кратна 512',
  'the image is too short: %1 sectors': 'образ слишком короток: %1 секторов',
  'no CP/M directory at sector 80 — not a disk image?':
    'на секторе 80 нет каталога CP/M — это точно образ диска?',
  'the directory points past the end of the image (block %1)':
    'каталог ссылается дальше конца образа (блок %1)',
  'the image uses block %1, past %2 — more than a disk here holds':
    'образ занимает блок %1, дальше %2 — больше, чем держит здешний диск',
  'reading…':                     'читаем…',
  '%1 of %2 sectors…':            '%1 из %2 секторов…',
  'with the system tracks':       'с системными дорожками',
  'with empty system tracks':     'с пустыми системными дорожками',
  'larger than usual: it uses blocks past 389':
    'крупнее обычного: заняты блоки за 389-м',
  'disk %1 is not empty — erase it first':
    'диск %1 не пуст — сначала очистите его',
  'disk %1 ← %2: %3':             'диск %1 ← %2: %3',
  ', %1 tail sectors dropped':    ', отброшено %1 хвостовых секторов',
  'the directory of disk %1 points at block %2, past %3 — it looks damaged':
    'каталог диска %1 ссылается на блок %2, дальше %3 — похоже, он испорчен',
  /* Плата: узлы ПЛИС (Board и Screen в index.html). Имена клавиш -- как на
     клавиатуре «Вектора», по-английски тоже: так их и ищут глазами. */
  'board':                        'плата',
  'the FPGA: devices on the bus, the machine keys, the screen':
                                  'ПЛИС: устройства на шине, клавиши машины, экран',
  'devices on the bus':           'устройства на шине',
  'floppy controller':            'контроллер дисковода',
  'quasi-disk 10h':               'квазидиск 10h',
  'quasi-disk 11h':               'квазидиск 11h',
  'hard drive':                   'винт',
  'AY sound':                     'звук AY',
  'screen mirror':                'зеркало экрана',
  'floppy ready':                 'готовность дисковода',
  'pin 34':                       'контакт 34',
  'by index':                     'по индексу',
  'always':                       'всегда',
  'floppy cable':                 'шлейф дисковода',
  'straight, Shugart':            'прямой, по Шугарту',
  'PC, with a twist':             'от ПК, с перекруткой',
  'PC cable with a twist: Gotek on the middle connector (S1) is A:, the 3.5" drive after the twist (DS0) is B:':
    'шлейф от ПК с перекруткой: Gotek на среднем разъёме (S1) — A:, 3,5" за перекруткой (DS0) — B:',
  'write precompensation':        'предкомпенсация записи',
  '%1 ns':                        '%1 нс',
  'none':                         'нет',
  'only from track 44':           'только с 44-й дорожки',
  'VGA output':                   'выход VGA',
  '50 Hz (576p)': '50 Гц (576p)',
  '60 Hz':                        '60 Гц',
  'timer sound':                  'звук таймера',
  'ours':                         'наш',
  'the machine’s':                'машины',
  'the machine':                  'машина',
  'VVOD':                         'ВВОД',
  'SBR':                          'СБР',
  'reboots the machine from ROM': 'перезагружает машину из ПЗУ',
  'boots the machine with the ROM switched off':
                                  'загружает машину с отключённым ПЗУ',
  'once more — it reboots':       'ещё раз — машина перезагрузится',
  'resetting the drive…':         'сбрасываем привод…',
  'restarts the drive; a write of the Vector in progress is cut off':
                                  'перезапускает привод; запись «Вектора», если она идёт, оборвётся',
  'once more — the drive restarts': 'ещё раз — привод перезапустится',
  'reset the drive':              'сбросить привод',
  'writing the disk header…':     'пишем заголовок диска…',
  'clearing the directories: %1 of %2': 'очищаем каталоги: %1 из %2',
  'the card is laid out: %1 disks': 'карта размечена: дискет %1',
  'The card is not laid out on disks': 'Карта не размечена на дискеты',
  'There is no disk layout in its header: it is new or was used elsewhere. The board can lay it out the way INITHDD6 and FRMTHDD6 do on the Vector.':
    'В её заголовке нет разметки: она новая или была в другом устройстве. Плата может разметить её так же, как INITHDD6 и FRMTHDD6 на «Векторе».',
  'The board does not know the drive geometry: update the ESP firmware or lay the card out on the Vector with INITHDD6.':
    'Плата не знает геометрию привода: обновите прошивку ESP или разметьте карту на «Векторе» программой INITHDD6.',
  'geometry':                     'геометрия',
  'disks, at most %1':            'дискет, не больше %1',
  'The directories of all these disks are cleared: whatever was on the card becomes unreachable. The system area is not touched.':
    'Каталоги всех этих дискет будут очищены: что было на карте, станет недоступно. Системная область не трогается.',
  'once more — lay the card out': 'ещё раз — разметить карту',
  'lay out the card':             'разметить карту',
  'the board leaves this network and raises its own for setup':
                                  'плата уйдёт из этой сети и поднимет свою для настройки',
  'once more — the board leaves the network': 'ещё раз — плата уйдёт из сети',
  'forget the network':           'забыть сеть',
  'The board will raise its own network combolite-XXXX; it can be set up again only from a phone nearby or over the USB cable.':
    'Плата поднимет свою сеть combolite-XXXX; настроить её снова можно будет только с телефона рядом или шнурком.',
  'The board forgot the network and restarts into setup: connect to its own network combolite-XXXX and open http://192.168.4.1/':
    'Плата забыла сеть и перезагружается в настройку: подключитесь к её сети combolite-XXXX и откройте http://192.168.4.1/',
  'loader':                       'загрузчик',
  'the machine’s ROM':            'ПЗУ «Вектора»',
  'BOOT26 from the board':        'подставной BOOT26',
  'BOOT26, only at power-on':     'BOOT26, только при включении',
  'BOOT26 from the board: at power-on and after VVOD, without replacing the ROM':
                                  'BOOT26 с платы: при включении и после ВВОД, без замены ПЗУ',
  'start the loaded program':     'автозапуск загруженного',
  'when the loader blinks RUS/LAT at the end, the board presses SBR itself':
                                  'когда загрузчик в конце мигает РУС/ЛАТ, плата сама нажмёт СБР',
  'boot loader':                  'подставной загрузчик',
  'window open':                  'окно открыто',
  'armed':                        'взведён',
  'not armed':                    'не взведён',
  'arm':                          'взвести',
  'arm and boot':                 'взвести и СБР',
  'disarm':                       'снять',
  'the screen':                   'экран',
  'The screen of the machine':    'Экран машины',
  'refresh':                      'обновить',
  'live':                         'живой',
  '%1 frames a second':           '%1 кадра в секунду',
  'the colors are arbitrary: the board does not know the palette':
                                  'цвета условные: палитры плата не знает',
  'the palette is the one of the frame: colour changes under the beam are not shown':
                                  'палитра — одна на кадр: смены цвета под лучом не показаны',

};

