// Ядро платы: квазидиск снаружи (статика), IDE, AY, ВГ93 и остальное внутри
// FPGA. Выводы корпуса и ФАПЧ -- в rev4_top.v.
//
// ТАКТ. Основной такт -- 50 МГц прямо с генератора. AY получает свои ровно
// 1,75 МГц дробным делителем 7/200 внутри ay_iface.v. Ядру ВГ93 и точке VGA
// нужны 16 и 40 МГц, их даёт ФАПЧ (pll_fdc.v).
//
// ШИНА ДАННЫХ. И ШД «Вектора», и DD привода идут через двунаправленные
// трансляторы, поэтому наружу выведено направление (`shd_dir`, `ide_dd_dir`).
// Данные квазидиска через FPGA НЕ идут: статика отвечает сама через буфер
// данных (см. «буфер данных» ниже).
//
// /CS1 ПРИВОДА ВЫВОДА НЕ ЗАНИМАЕТ -- как в оригинале 1999 года. Блок
// управления ATA нам не нужен.

`default_nettype none

module top #(
    // Тишина процессора, после которой шину привода можно отдать удалённой
    // стороне, и предел владения. Выведены наружу, чтобы тестбенч мог их
    // укоротить: при боевых 50 мс проверка потребовала бы 2,5 млн тактов.
    parameter IDE_ARB_IDLE  = 2500000,
    parameter IDE_ARB_GRANT = 500000,
    parameter IDE_RESET_TICKS = 12'd2500,      // 50 мкс при 50 МГц
    parameter LED_TICKS       = 28'd2500000,   // 50 мс при 50 МГц
    // Автозапуск (autostart.v): удержание ВВОД с включения, нажатие, пауза
    // между клавишами. Наружу -- ради тестбенча, как и прочие времена.
    parameter KEY_HOLD_TICKS  = 100_000_000,   // 2 с
    parameter KEY_PRESS_TICKS = 2_500_000,     // 50 мс
    parameter KEY_GAP_TICKS   = 1_000_000,     // 20 мс
    // Генератор кадра (vid_*.v). Ему нужны счётчики адреса и /OE статики;
    // rev4_top.v включает его (VIDEO = 1).
    parameter VIDEO = 0,
    // ВЫКЛЮЧАТЕЛИ УЗЛОВ НА СБОРКУ. Выключенный узел в кристалл не попадает, его
    // выходы стоят в покое. Плата берёт всё.
    parameter HAS_FDC    = 1,    // ВГ93 (выводы дисковода)
    parameter HAS_KVAZ2  = 1,    // второй квазидиск, порт 0x11 (A18 статики)
    parameter HAS_AY     = 1,
    parameter HAS_VI53   = 1,    // эмуляция ВИ53 и её такт от машины
    parameter HAS_MOUSE  = 1,
    parameter HAS_KEYS   = 1,    // автозапуск (ключи ВВОД и СБР)
    parameter HAS_LOADER = 1,    // подставной загрузчик (нужен ~ШАП)
    parameter HAS_MIRROR = 1,    // зеркало в DD6B
    parameter HAS_WATCH  = 1,    // наблюдатель шины bus_watch -- оснастка отладки
    parameter HAS_CHAN   = 1,    // Wi-Fi модем: порты 28h-2Dh как у UART 16550 (vchan.v)
    parameter VID_STATS  = 1,    // замеры различителя по стробу (stb_* в info)
    parameter AY_PORTS   = 1,    // регистры портов ввода-вывода AY R14/R15
    // Диагностика для SPI -- через регистр-снимок (1) или прямо (0). Источники
    // диагностики шли до сдвигового регистра SPI одним тактом через выбор байта
    // из сорока, и по Diamond это худший путь clk50: no_mark -> сравнение ->
    // бит блока 5 -> выбор -> spi_sh_out, 18,6 нс, 11 уровней. Снимок раз в
    // такт: значения медленные, лишний такт ничего не значит, а снимок
    // целостный. Регистр -- на ВХОДЕ выбора, не на выходе: ответ обязан быть
    // комбинацией от фазы (ide_remote.v).
    parameter DIAG_PIPE  = 1,
    // Звук раздельно: слева только бипер с контакта 42, справа только наш ВИ53.
    // Для сверки эмуляции с настоящим таймером, если вход `beep` заведён (на
    // плате его нет, rev4_top.v подаёт 0).
    parameter AUDIO_SPLIT = 0
) (
    input  wire        clk,            // 50 МГц с генератора

    // шина «Вектора», через трансляторы
    input  wire [7:0]  shavv_n,        // ~ШАВВ: порт либо A15..A8
    input  wire [7:0]  shap,           // ~ШАП: MA0..MA7 динамической памяти
    inout  wire [7:0]  shd,            // ШД0-7
    output wire        shd_dir,        // 1 = FPGA выдаёт наружу
    input  wire        ras_n,
    input  wire        cas_n,
    input  wire        chtzu_n,
    input  wire        zpzu_n,
    input  wire        chtvv_n,
    input  wire        zpvv_n,
    input  wire        stack,
    input  wire        sbros_n,
    input  wire        beep,           // ВИ53 «Вектора» с разъёма ВУ
    output wire        blk_n,          // ~БЛК на разъём
    output wire        xlat_oe_n,      // разрешение всех трансляторов уровня

    // квазидиск на плате
    output wire [2:0]  sram_page,      // A16..A18; A18 -- который из двух квазидисков
    output wire        latch_clk,      // такт защёлки 574 адреса строки
    output wire        latch_clk2,     // такт защёлки столбца DD7B
    output wire        sram_ce0_n,     // /CE DD6: оба квазидиска
    output wire        sram_ce1_n,     // /CE DD6B: зеркало ОЗУ машины (mirror.v)

    // IDE
    inout  wire [15:0] ide_dd,
    output wire        ide_dd_dir,
    output wire [2:0]  ide_da,
    output wire        ide_cs0_n,
    output wire        ide_dior_n,
    output wire        ide_diow_n,
    output wire        ide_reset_n,
    output wire        ide_cs1_n,
    output wire        ide_led_n,

    // звук
    output wire        audio_l,
    output wire        audio_r,

    // SPI к модулю ESP32, FPGA — ведомый. Протокол в spi_slave.v, поверх него
    // ide_remote.v, доступ к шине через ide_arb.v.
    input  wire        esp_sck,
    input  wire        esp_mosi,
    input  wire        esp_cs,
    output wire        esp_miso,
    output wire        esp_attn,

    // дисковод: ядро ВГ93 (fdc.v), такт 16 МГц с ФАПЧ платы (pll_fdc.v)
    input  wire        clk_fdc,
    input  wire        clk_fdc_lock,   // захват ФАПЧ; до него ядро ВГ93 в сбросе
    output wire        fdd_ds0_n,
    output wire        fdd_ds1_n,
    output wire        fdd_moton_n,
    output wire        fdd_dir_n,
    output wire        fdd_step_n,
    output wire        fdd_wdata_n,
    output wire        fdd_wgate_n,
    output wire        fdd_side1_n,
    input  wire        fdd_index_n,
    input  wire        fdd_trk00_n,
    input  wire        fdd_wpt_n,
    input  wire        fdd_rdata_n,
    input  wire        fdd_rdy_n,

    // клавиатура машины: ключи на контакты 39 (ВВОД) и 36 (СБР), 1 -- нажата
    output wire        key_vvod,
    output wire        key_sbr,

    // генератор кадра (VIDEO = 1): такт 40 МГц, СТРОБ СОСТ., VGA 3-3-2 и
    // четыре сигнала счётчиков адреса статики
    input  wire        clk_vga,
    input  wire        strob,
    output wire [2:0]  vga_r,
    output wire [2:0]  vga_g,
    output wire [1:0]  vga_b,
    output wire        vga_hs,
    output wire        vga_vs,
    output wire        addr_cpc,
    output wire        addr_cpc_d0,    // такт 590-го половинами для ODDRXE (vid_fetch.v)
    output wire        addr_cpc_d1,
    output wire        addr_mrc,
    output wire        addr_src,
    output wire        sram_oe_n,
    // буфер данных SN74LVC8T245: сторона A --
    // память (статика, транслятор DD3), B -- ШД разъёма
    output wire        buf_dir,        // 1 -- память -> разъём (A -> B)
    output wire        buf_oe_n
);

// ------------------------------------------------------------ сброс по питанию
// ~СБРОС с шины приходит через vu_bus, но до первого сброса «Вектора» логика
// тоже должна быть в известном состоянии.
reg [7:0] por_cnt = 8'h00;
always @(posedge clk)
    if (por_cnt != 8'hFF) por_cnt <= por_cnt + 1'b1;
// por и reset ниже -- регистры, а не комбинация: оба расходятся на сотни
// нагрузок (reset -- на 445), и сравнение счётчика вместе с ИЛИ перед ними
// стояло в худшем пути clk50 (по Diamond 18,0 нс из 20).
// Начальное значение -- сброс поднят.
reg por = 1'b1;
always @(posedge clk) por <= (por_cnt != 8'hFF);

// -------------------------------------------------- разрешение трансляторов
// Пока FPGA не сконфигурирована, её выводы в третьем состоянии со слабой
// подтяжкой вниз. Без этого сигнала трансляторы вытолкнули бы на «Вектор»
// произвольные уровни, в том числе ~БЛК в активный нуль — машина не загрузилась
// бы. На плате OE подтянут вверх, поэтому до конфигурации трансляторы закрыты
// сами собой, а открываем мы их только досчитав сброс по питанию.
// Даташит SN74LVC16T245 просит того же: «To ensure the high-impedance state
// during power up or power down, OE should be tied to VCC through a pull-up».
assign xlat_oe_n = por;

// ------------------------------------------------------------------- шина
wire [7:0] bus_addr;
wire       iord, iowr, memrd, memwr;
wire       clean_stack, sys_reset;
wire       iord_fall, iowr_fall, ras_fall;
wire       vid_win, vid_lock;         // различитель видеоцикла, для vid_raster.v
wire       vid_rd;                    // генератор кадра читает зеркало (vid_fetch.v)
wire       iowr_reg;              // импульс записи в регистры платы, см. vu_bus.v


// Заморозка защёлки адреса на время обращения — переключаемый опыт, разбор
// в vu_bus.v. Объявлено до инстанса шины, потому что туда и уходит.
wire       latch_gate;
wire       vid_gate;               // гасить такт защёлки на видеоспаде
wire       vid_pol;               // различитель: 0 -- видеоспад после длинного промежутка
wire       vi53_off;              // эмуляция ВИ53 выключена, команда 0x48 по SPI
wire       ms_stb, ms_present;    // мышь: отчёт от ESP пакетом 0x42 по SPI
wire [7:0] dev_off;               // выключатели устройств, команда 0x49 по SPI
wire [1:0] fdc_rdy_sel;           // источник готовности ВГ93, команда 0x4A
wire       fdd_pc_cable;          // шлейф ПК с перекруткой, разряд 2 того же байта
wire [63:0] fdc_dbg;              // диагностика ВГ93, блок 7 (0x78), fdc.v
wire [4:0] fdc_pc;                // предкомпенсация записи ВГ93, команда 0x4F
wire       key_stb;               // команда клавишам, 0x4B по SPI
wire [1:0] key_cmd;
wire       loader_stb;            // подставной загрузчик, 0x4C по SPI
wire [1:0] loader_cmd;
wire       fr_blk, fr_on;         // окно подставного загрузчика (fakerom.v)
wire       boot_kbd;              // загрузчик после ВВОД с клавиатуры, 0x56 по SPI
wire       boot_auto;             // автозапуск загруженного (BOOT26 и ПЗУ), 0x56 разряд 1
wire       kb_rom_live;           // после ВВОД работает ПЗУ (rom_sense.v)
wire       kb_boot;               // ВВОД с клавиатуры узнан: взвести и СБР
wire [63:0] kb_diag;              // блок диагностики 6 (rom_sense.v)
wire       kb_listen;             // rom_sense ждёт первых чтений: буфер ШД к ПЛИС
wire [10:0] pal_ra;               // палитра для снимка экрана: ячейка (0x57)
wire [7:0]  pal_rd;               //   и её байт (vid_rec.v), дальше журнал портов
wire        pal_busy;             // выдача идёт -- журнал портов стоит
wire       dm_start, dm_next;     // карта блоков зеркала, 0x4D по SPI
wire [7:0] dm_byte;
wire       vga60;                 // режим VGA, 0x4E по SPI
wire       scr_req_stb;           // заявка на строку экрана, 0x52 по SPI
wire [7:0] scr_req_row;
wire       sr_start, sr_next;     // выдача строки экрана, 0x53
wire [7:0] sr_byte;               // её байт (scr_buf.v)
wire [7:0] mir_miss;              // записей машины мимо зеркала (mirror.v)
wire       fr_armed;
wire [63:0] diag4, diag5;
wire [7:0] ms_btn, ms_dx, ms_dy, ms_wh;
wire       diag_clr;              // обнуление счётчиков, из ide_remote
wire [7:0] stb_after_min, stb_after_max, stb_before_min, stb_before_max;
wire [7:0] vid_lost, vid_state, stb_max;
wire       ce_gate;
wire       ce_by_cas;

vu_bus #(.VID_STATS(VID_STATS)) bus (
    .clk(clk), .reset(por),
    .shavv_n(shavv_n), .ras_n(ras_n), .cas_n(cas_n),
    .chtzu_n(chtzu_n), .zpzu_n(zpzu_n),
    .chtvv_n(chtvv_n), .zpvv_n(zpvv_n),
    .stack(stack), .sbros_n(sbros_n),
    .bus_addr(bus_addr), .iord(iord), .iowr(iowr),
    .memrd(memrd), .memwr(memwr),
    .clean_stack(clean_stack), .sys_reset(sys_reset),
    .iord_fall(iord_fall), .iowr_fall(iowr_fall), .ras_fall(ras_fall),
    .iowr_reg(iowr_reg), .vid_win(vid_win), .vid_lock(vid_lock),
    .latch_gate(latch_gate), .vid_gate(vid_gate), .vid_clr(diag_clr), .vid_pol(vid_pol),
    .stb_after_min(stb_after_min), .stb_after_max(stb_after_max),
    .stb_before_min(stb_before_min), .stb_before_max(stb_before_max),
    .vid_lost(vid_lost), .vid_state(vid_state), .stb_max(stb_max),
    .latch_clk(latch_clk)
);

reg reset = 1'b1;
always @(posedge clk) reset <= sys_reset | por;

wire [7:0] shd_in = shd;

// -------------------------------------------------------------- квазидиск
// addr_hi и port — одна и та же шина: в цикле обращения к памяти ~ШАВВ несёт
// A15..A8, в цикле ввода-вывода — адрес порта. Подробнее в vu_bus.v.
wire [7:0] kvaz_control;   // объявлено до места использования
wire       kvaz_hit_std, kvaz_hit_bar, kvaz_hit_stack;

wire [1:0] kv1_page, kv2_page;
wire       kv1_blk_n, kv2_blk_n;
wire [7:0] kvaz2_control;
wire       kv2_hit_std, kv2_hit_bar, kv2_hit_stack;

// ВЫКЛЮЧАТЕЛИ (dev_off, команда 0x49): выключенному устройству не доходят
// стробы ввода-вывода, а квазидиск ещё и не опускает ~БЛК -- для машины, где
// своё такое уже стоит.
wire kv1_off = dev_off[1], kv2_off = dev_off[2], ide_off = dev_off[3], ay_off = dev_off[4];
wire mir_off = dev_off[5];

kvaz_sram #(.PORT_KVAZ(8'h10)) kvaz (
    .clk(clk), .reset(reset),
    .port(bus_addr), .addr_hi(bus_addr),
    .data_in(shd_in), .iowr_reg(iowr_reg & ~kv1_off), .stack(clean_stack),
    .chtzu_n(chtzu_n), .zpzu_n(zpzu_n), .cas_n(cas_n),
    .ce_gate(ce_gate), .ce_by_cas(ce_by_cas),
    .page(kv1_page), .blk_n(kv1_blk_n), .control(kvaz_control),
    .hit_std(kvaz_hit_std), .hit_bar(kvaz_hit_bar), .hit_stack(kvaz_hit_stack)
);

// Второй квазидиск, порт 0x11 -- тот же модуль.
generate if (HAS_KVAZ2) begin : g_has_kvaz2
kvaz_sram #(.PORT_KVAZ(8'h11)) kvaz2 (
    .clk(clk), .reset(reset),
    .port(bus_addr), .addr_hi(bus_addr),
    .data_in(shd_in), .iowr_reg(iowr_reg & ~kv2_off), .stack(clean_stack),
    .chtzu_n(chtzu_n), .zpzu_n(zpzu_n), .cas_n(cas_n),
    .ce_gate(ce_gate), .ce_by_cas(ce_by_cas),
    .page(kv2_page), .blk_n(kv2_blk_n), .control(kvaz2_control),
    .hit_std(kv2_hit_std), .hit_bar(kv2_hit_bar), .hit_stack(kv2_hit_stack)
);

end else begin : g_no_kvaz2
    assign kv2_page = 2'd0; assign kv2_blk_n = 1'b1; assign kvaz2_control = 8'h00;
    assign kv2_hit_std = 1'b0; assign kv2_hit_bar = 1'b0; assign kv2_hit_stack = 1'b0;
end endgenerate
// ПАМЯТЬ. Оба диска -- на DD6 (/CE0): первый в страницах 0..3 (A18 = 0), второй
// в 4..7 (A18 = 1). DD6B (/CE1) отдана зеркалу ОЗУ машины (mirror.v).
// Спорить за один цикл два диска могут только по вине программы -- у живой
// машины с двумя доработками это была бы драка на шине. Здесь главнее первый.
wire kv1_on = ~kv1_blk_n & ~kv1_off;
wire kv2_on = ~kv2_blk_n & ~kv2_off;
// ~БЛК ставит и подставной загрузчик (fakerom.v), но статику он не выбирает:
// его байты идут из блочной памяти ПЛИС через транслятор шины данных.
wire kv_on = kv1_on | kv2_on;
assign blk_n      = ~(kv_on | fr_blk);
// Зеркало (DD6B) -- страница 0: в нём 64 КБ, и адрес целиком с защёлок.
wire mir_ce1, mir_col;
// Чтение зеркала генератором кадра (vid_rd, только VIDEO = 1): DD6B, страница
// 0, и DD6 обязана молчать -- в тишине шины выбор квазидиска по ~ШАВВ может
// остаться от прошлого цикла, а /OE статики у нас общий. vid_rd снимается
// живыми сигналами машины (vid_fetch.v), так что её циклам не мешает.
// Один квазидиск, ни зеркала, ни кадра: нулевая страница не нужна никому --
// без квазидиска /CE0 и так снят, -- и страница идёт прямо с него. Путь СТЕК
// -> страница так короче на ~БЛК и выключатель: по Diamond 15,28 нс через них
// против 8,66 прямо.
localparam PAGE_ZERO = (HAS_MIRROR != 0) || (HAS_KVAZ2 != 0) || (VIDEO != 0);
// Различитель сброса с клавиатуры: нужны свои клавиши (подпись ПЗУ -- по
// своему ВВОД) и сам загрузчик.
localparam KBD_RST = (HAS_LOADER != 0) && (HAS_KEYS != 0);
assign sram_page  = !PAGE_ZERO ? {1'b0, kv1_page}
                  : vid_rd ? 3'd0
                  : kv1_on ? {1'b0, kv1_page}
                  : kv2_on ? {1'b1, kv2_page}
                  :          3'd0;
assign sram_ce0_n = ~kv_on | vid_rd;
assign sram_ce1_n = ~(mir_ce1 | vid_rd);
// ЗАЩЁЛКА СТОЛБЦА (DD7B): берёт ~ШАП по спаду ~CAS, как строку DD7 берёт по
// спаду ~RAS (latch_clk в vu_bus.v). Заморозки ей не нужно: спадов ~CAS
// внутри процессорного чтения ровно один, свой (замер, kvaz_sram.v, «ЧЕМ
// КВАЛИФИЦИРОВАТЬ»); лишние внутри строба -- только ~RAS.
// Но ~CAS приходит только при ~БЛК, а зеркалу нужны и обычные записи машины:
// для них такт столбца делает mirror.v по времени от ~RAS. Где ~CAS есть,
// такт столбца от mirror.v приходит, пока ~CAS уже низкий, и лишнего фронта
// не даёт.
assign latch_clk2 = ~cas_n | mir_col;

// ------------------------------------------------- диагностика квазидиска
// Регистр порта 0x10 был только для записи, и это слепило отладку: что именно
// пишет туда MicroDOS, узнать было нечем, а от этого зависит выбранная
// страница, стековый режим и окна Баркаря (разряды 6-7 открывают
// 0x8000-0x9FFF и 0xE000-0xFFFF, то есть область экрана и верхнюю память).
//
// Три байта наружу через SPI, команды 0x38..0x3A:
//   текущее значение — что стоит сейчас;
//   ЛИПКОЕ «какие разряды когда-либо выставлялись» — одиночную запись мусора
//     иначе не поймать вообще никак;
//   счётчик записей с насыщением — часто ли драйвер трогает регистр.
//
// Сброс только по питанию (por), а НЕ по reset: сброс с шины машины приходит
// на каждый Ввод+Блк и обнулил бы накопленное ровно тогда, когда оно нужно.
reg  [7:0] kvaz_seen;
reg  [7:0] kvaz_writes;
// Счётчики обращений по окнам. Считается ФРОНТ обращения: окно ответило и при
// этом идёт строб памяти. Насыщение на 255 — нам нужен не точный счёт, а ответ
// «ноль или не ноль» и порядок величины. Особенно про окна Баркаря: если там
// НЕ ноль, плата отвечает за область экрана и верхнюю память, где живёт сама
// система, — и тогда MicroDOS портит себя нашими руками.
reg  [7:0] kvaz_std, kvaz_bar, kvaz_stk;
wire       mem_acc = memrd | memwr;
reg        acc_std_r, acc_bar_r, acc_stk_r;
wire       acc_std = kvaz_hit_std   & mem_acc;
wire       acc_bar = kvaz_hit_bar   & mem_acc;
wire       acc_stk = kvaz_hit_stack & mem_acc;

always @(posedge clk) begin
    if (por) begin
        kvaz_seen   <= 8'h00;
        kvaz_writes <= 8'h00;
        kvaz_std    <= 8'h00;
        kvaz_bar    <= 8'h00;
        kvaz_stk    <= 8'h00;
        acc_std_r   <= 1'b0;
        acc_bar_r   <= 1'b0;
        acc_stk_r   <= 1'b0;
    end
    else begin
        kvaz_seen <= kvaz_seen | kvaz_control;
        if (iowr_reg && bus_addr == 8'h10 && kvaz_writes != 8'hFF)
            kvaz_writes <= kvaz_writes + 1'b1;

        acc_std_r <= acc_std;
        acc_bar_r <= acc_bar;
        acc_stk_r <= acc_stk;
        if (acc_std & ~acc_std_r & (kvaz_std != 8'hFF)) kvaz_std <= kvaz_std + 1'b1;
        if (acc_bar & ~acc_bar_r & (kvaz_bar != 8'hFF)) kvaz_bar <= kvaz_bar + 1'b1;
        if (acc_stk & ~acc_stk_r & (kvaz_stk != 8'hFF)) kvaz_stk <= kvaz_stk + 1'b1;
    end
end

// ------------------------------------------------ наблюдатель за шиной
// Считает аномалии на СТЕК и ~RAS теми же входами, какими их видит логика.
// Осциллограф смотрит меньше процента реального времени, здесь слепого времени
// нет вовсе. Разбор — в bus_watch.v.
wire [7:0] stk_unstable, ras_glitch, stk_late, ras_lo_min, ras_hi_min;
wire [7:0] adr_setup_min, adr_hold_min;   // считаются, но НЕ выводятся:
                                         // меряют ~ШАВВ, а не вход защёлки
wire [7:0] rd_len_min, rd_cas_min;
wire [7:0] rd_ras_max, wr_ras_max, rd_len_max;
wire [7:0] ras_to_rd_min, ras_to_rd_max;
wire [7:0] rd_cas_max;

generate if (HAS_WATCH) begin : g_has_watch
bus_watch watch (
    .clk(clk), .por(por), .clr(diag_clr),
    .ras_n(ras_n), .cas_n(cas_n), .stack(stack), .chtzu_n(chtzu_n), .zpzu_n(zpzu_n),
    // «обращение наше»: ровно `ram_sel | stack_sel` из kvaz_sram — три
    // попадания взаимно исключающи по построению, их ИЛИ и есть выбор.
    .sel(kvaz_hit_std | kvaz_hit_bar | kvaz_hit_stack | kv2_hit_std | kv2_hit_bar | kv2_hit_stack),
    .shavv_n(shavv_n),
    .stk_unstable(stk_unstable), .ras_glitch(ras_glitch),
    .stk_late(stk_late), .ras_lo_min(ras_lo_min), .ras_hi_min(ras_hi_min),
    .adr_setup_min(adr_setup_min), .adr_hold_min(adr_hold_min),
    .rd_ras_max(rd_ras_max), .wr_ras_max(wr_ras_max), .rd_len_max(rd_len_max),
    .ras_to_rd_min(ras_to_rd_min), .ras_to_rd_max(ras_to_rd_max),
    .rd_cas_max(rd_cas_max), .rd_cas_min(rd_cas_min), .rd_len_min(rd_len_min)
);

end else begin : g_no_watch
    assign stk_unstable = 8'd0; assign ras_glitch = 8'd0; assign stk_late = 8'd0;
    assign ras_lo_min = 8'd0; assign ras_hi_min = 8'd0;
    assign adr_setup_min = 8'd0; assign adr_hold_min = 8'd0;
    assign rd_ras_max = 8'd0; assign wr_ras_max = 8'd0; assign rd_len_max = 8'd0;
    assign ras_to_rd_min = 8'd0; assign ras_to_rd_max = 8'd0;
    assign rd_cas_max = 8'd0; assign rd_cas_min = 8'd0; assign rd_len_min = 8'd0;
end endgenerate
// -------------------------------------------------------------------- IDE
wire [7:0]  ide_cpu_dout;
wire        ide_cpu_dout_oe;
wire [15:0] ide_dd_out;
wire        ide_dd_out_oe;

// Выводы привода ведёт либо процессор через ide.v, либо удалённая сторона
// через ide_cycle.v. Кто именно — решает арбитр (ide_arb.v).
wire [2:0]  cpu_da, cyc_da;
wire        cpu_cs0_n, cpu_dior_n, cpu_diow_n;
wire        cyc_cs0_n, cyc_dior_n, cyc_diow_n;
wire [15:0] cyc_dout;
wire        cyc_dout_oe;
wire        sel_remote, cpu_hold;
wire        rem_reset;      // импульс сброса привода от удалённой стороны
wire        cpu_ide_acc;

ide #(.RESET_TICKS(IDE_RESET_TICKS)) hdd (
    .clk(clk), .reset(reset),
    .port(bus_addr), .iord(iord & ~ide_off), .iowr(iowr & ~ide_off),
    .iowr_reg(iowr_reg & ~ide_off), .hold(cpu_hold),
    .rem_reset(rem_reset),
    .cpu_din(shd_in), .cpu_dout(ide_cpu_dout), .cpu_dout_oe(ide_cpu_dout_oe),
    .ide_da(cpu_da), .ide_cs0_n(cpu_cs0_n), .ide_cs1_n(ide_cs1_n),
    .ide_dior_n(cpu_dior_n), .ide_diow_n(cpu_diow_n),
    .ide_reset_n(ide_reset_n),
    .ide_din(ide_dd), .ide_dout(ide_dd_out), .ide_dout_oe(ide_dd_out_oe)
);

// Признак «процессор в обращении к приводу» для арбитра. Строб чтения или
// записи по адресам командного блока — этого достаточно: арбитр смотрит только
// на тишину, а не на смысл обращения.
assign cpu_ide_acc = (bus_addr[7:3] == 5'b01010) & (iord | iowr) & ~ide_off;

// ---------------------------------------------- удалённая сторона: ESP -> привод

wire [7:0]  spi_rx, spi_tx;
wire        spi_rx_valid, spi_cs_active;
wire        esp_miso_o, esp_miso_oe;
wire        rem_req, rem_long, rem_grant, rem_timeout;
wire        cpu_seen, cpu_quiet, cpu_seen_clr;
wire        cyc_start, cyc_wr, cyc_busy, cyc_done;
wire [2:0]  cyc_addr;
wire [15:0] cyc_wdata, cyc_rdata;

// SPI и разбор посылок -- на сбросе по ПИТАНИЮ: машину в сбросе держит и ВВОД,
// и наш автозапуск, а ESP должна говорить с платой и тогда. См. ide_remote.v.
spi_slave spi (
    .clk(clk), .reset(por),
    .sck(esp_sck), .mosi(esp_mosi), .cs_n(esp_cs),
    .miso(esp_miso_o), .miso_oe(esp_miso_oe),
    .rx_data(spi_rx), .rx_valid(spi_rx_valid),
    .tx_data(spi_tx), .tx_load(), .cs_active(spi_cs_active)
);

wire [447:0] diag_all = {fdc_dbg, kb_diag, diag5, diag4,
           stb_before_max, stb_before_min,
           vid_lost, vid_state, stb_max, stb_after_max,
           ras_to_rd_max, ras_to_rd_min,
           rd_len_max, wr_ras_max, rd_ras_max,
           stb_after_min, rd_len_min,
           ras_hi_min, ras_lo_min, stk_late,
           ras_glitch, stk_unstable,
           kvaz_stk, kvaz_bar, kvaz_std, kvaz_writes, kvaz_seen, kvaz_control};
reg  [447:0] diag_q = 448'd0;
always @(posedge clk) diag_q <= diag_all;

// Канал «Вектор» <-> ESP (vchan.v): провода к ide_remote.
wire       ch_poll, ch_pop, ch_push, ch_flushed, ch_avail, ch_e2v_empty, ch_alive;
wire [7:0] ch_wdata, ch_head;

ide_remote #(.KBD(KBD_RST ? 1 : 0), .PAL(VIDEO ? 1 : 0), .CHAN(HAS_CHAN ? 1 : 0)) remote (
    .clk(clk), .reset(reset), .por(por),
    .rem_reset(rem_reset),
    .rx_data(spi_rx), .rx_valid(spi_rx_valid), .tx_data(spi_tx),
    .cs_active(spi_cs_active),
    .rem_req(rem_req), .rem_long(rem_long), .rem_grant(rem_grant), .rem_timeout(rem_timeout),
    .cpu_seen(cpu_seen), .cpu_quiet(cpu_quiet), .cpu_seen_clr(cpu_seen_clr),
    .cyc_start(cyc_start), .cyc_wr(cyc_wr), .cyc_addr(cyc_addr),
    .cyc_wdata(cyc_wdata), .cyc_rdata(cyc_rdata),
    .cyc_busy(cyc_busy), .cyc_done(cyc_done),
    .diag_clr(diag_clr),
    .latch_gate(latch_gate), .vid_gate(vid_gate), .vid_pol(vid_pol),
    .ce_gate(ce_gate),
    .ce_by_cas(ce_by_cas),
    .vi53_off(vi53_off),
    .mouse_stb(ms_stb), .mouse_btn(ms_btn), .mouse_dx(ms_dx), .mouse_dy(ms_dy),
    .mouse_wh(ms_wh), .mouse_present(ms_present),
    .dev_off(dev_off), .fdc_rdy_sel(fdc_rdy_sel), .fdd_pc_cable(fdd_pc_cable),
    .fdc_pc(fdc_pc),
    .key_stb(key_stb), .key_cmd(key_cmd),
    .loader_stb(loader_stb), .loader_cmd(loader_cmd), .boot_kbd(boot_kbd), .boot_auto(boot_auto),
    .dm_start(dm_start), .dm_next(dm_next), .dm_byte(dm_byte), .vga60(vga60),
    .scr_req_stb(scr_req_stb), .scr_req_row(scr_req_row),
    .sr_start(sr_start), .sr_next(sr_next), .sr_byte(sr_byte),
    .pal_ra(pal_ra), .pal_rd(pal_rd), .pal_busy(pal_busy),
    .ch_poll(ch_poll), .ch_pop(ch_pop), .ch_push(ch_push), .ch_wdata(ch_wdata),
    .ch_head(ch_head), .ch_avail(ch_avail), .ch_e2v_empty(ch_e2v_empty),
    .ch_flushed(ch_flushed), .ch_alive(ch_alive),
    // Первый блок (0x38..0x3F) — квазидиск; второй (0x20..0x27) — измерения
    // наблюдателя. Порядок внутри слова: младший байт первым.
    // Блок 5 (0x68..0x6F): растр, прокрутка, дисковод, набор узлов битстрима.
    // Блок 4 (0x60..0x67): состояние новых узлов и их счётчики.
    // Блок 6 (0x70..0x77): сброс с клавиатуры (rom_sense.v).
    .diag(DIAG_PIPE ? diag_q : diag_all)
);

ide_cycle cyc (
    .clk(clk), .reset(reset),
    .start(cyc_start), .wr(cyc_wr), .addr(cyc_addr), .wdata(cyc_wdata),
    .rdata(cyc_rdata), .busy(cyc_busy), .done(cyc_done),
    .ide_da(cyc_da), .ide_cs0_n(cyc_cs0_n),
    .ide_dior_n(cyc_dior_n), .ide_diow_n(cyc_diow_n),
    .ide_dout(cyc_dout), .ide_dout_oe(cyc_dout_oe), .ide_din(ide_dd)
);

ide_arb #(.IDLE_TICKS(IDE_ARB_IDLE), .GRANT_TICKS(IDE_ARB_GRANT)) arb (
    .clk(clk), .reset(reset),
    .cpu_active(cpu_ide_acc), .cyc_busy(cyc_busy),
    .rem_req(rem_req), .rem_long(rem_long), .rem_grant(rem_grant), .rem_timeout(rem_timeout),
    .cpu_seen_clr(cpu_seen_clr), .cpu_seen(cpu_seen), .cpu_quiet(cpu_quiet),
    .sel_remote(sel_remote), .cpu_hold(cpu_hold)
);

// ---------------------------------------------- мультиплексор выводов привода

assign ide_da     = sel_remote ? cyc_da     : cpu_da;
assign ide_cs0_n  = sel_remote ? cyc_cs0_n  : cpu_cs0_n;
assign ide_dior_n = sel_remote ? cyc_dior_n : cpu_dior_n;
assign ide_diow_n = sel_remote ? cyc_diow_n : cpu_diow_n;

wire        dd_oe  = sel_remote ? cyc_dout_oe : ide_dd_out_oe;
wire [15:0] dd_out = sel_remote ? cyc_dout    : ide_dd_out;

assign ide_dd     = dd_oe ? dd_out : 16'hZZZZ;
assign ide_dd_dir = dd_oe;

// --------------------------------------------------------------------- AY
wire [7:0] ay_dout;
wire       ay_dout_oe;
wire [7:0] ch_a, ch_b, ch_c;

generate if (HAS_AY) begin : g_has_ay
ay_iface #(.CE_NUM(7), .CE_DEN(200), .AY_PORTS(AY_PORTS)) ay (   // 7/200 от 50 МГц = 1,75 МГц
    .clk(clk), .reset(reset),
    .port(bus_addr), .iord(iord & ~ay_off), .iowr(iowr & ~ay_off),
    .din(shd_in), .dout(ay_dout), .dout_oe(ay_dout_oe),
    .ch_a(ch_a), .ch_b(ch_b), .ch_c(ch_c)
);

end else begin : g_no_ay
    assign ay_dout = 8'h00; assign ay_dout_oe = 1'b0;
    assign ch_a = 8'd0; assign ch_b = 8'd0; assign ch_c = 8'd0;
end endgenerate
// -------------------------------------------------------------- автозапуск
// Сброс -- только по питанию: нажатый ВВОД опускает ~СБРОС, и сброс машины
// сбросил бы сам автозапуск.
// Нажать СБР может попросить и подставной загрузчик -- в конце загрузки.
wire       fr_sbr_req;
wire       as_stb = key_stb | fr_sbr_req;
wire [1:0] as_cmd = fr_sbr_req ? 2'd2 : key_cmd;

generate if (HAS_KEYS) begin : g_has_keys
autostart #(.HOLD_TICKS(KEY_HOLD_TICKS), .PRESS_TICKS(KEY_PRESS_TICKS),
            .GAP_TICKS(KEY_GAP_TICKS)) keys (
    .clk(clk), .por(por), .cmd_stb(as_stb), .cmd(as_cmd),
    .key_vvod(key_vvod), .key_sbr(key_sbr)
);

end else begin : g_no_keys
    assign key_vvod = 1'b0; assign key_sbr = 1'b0;
end endgenerate
// -------------------------------------------------------------- зеркало
generate if (HAS_MIRROR) begin : g_has_mirror
mirror mir (
    .clk(clk), .por(por), .en(~mir_off),
    .ras_n(ras_n), .chtzu_n(chtzu_n), .zpzu_n(zpzu_n), .memrd(memrd), .memwr(memwr),
    .kv_on(kv_on), .a_hi(bus_addr),
    .col_pulse(mir_col), .ce1(mir_ce1),
    .dm_start(dm_start), .dm_next(dm_next), .dm_byte(dm_byte), .miss(mir_miss)
);

end else begin : g_no_mirror
    assign mir_col = 1'b0; assign mir_ce1 = 1'b0; assign dm_byte = 8'h00;
    assign mir_miss = 8'd0;
end endgenerate
// ------------------------------------------------ подставной загрузчик
// Младший байт адреса -- с ~ШАП (shap_demux.v); перестановка разрядов
// выведена из схемы машины.
wire [7:0] shap_lo;
wire       shap_stb;
wire [7:0] fr_dout;
wire       fr_dout_oe;
generate if (HAS_LOADER) begin : g_loader
shap_demux demux (
    .clk(clk), .shap(shap), .ras_n(ras_n), .cas_n(cas_n),
    .a_lo(shap_lo), .lo_stb(shap_stb)
);

fakerom loader (
    .clk(clk), .por(por), .sys_reset(sys_reset), .strob(strob),
    .a_hi(bus_addr), .a_lo(shap_lo),
    .memrd(memrd), .chtzu_n(chtzu_n), .memwr(memwr), .zpzu_n(zpzu_n),
    .iord(iord), .iowr(iowr),
    .iowr_reg(iowr_reg), .din(shd_in), .kv_on(kv_on),
    .cmd_stb(loader_stb), .cmd(loader_cmd), .key_sbr(key_sbr), .auto_boot(kb_boot),
    .autostart(boot_auto), .rom_go(boot_auto & kb_rom_live),
    .sbr_req(fr_sbr_req),
    .blk(fr_blk), .dout(fr_dout), .dout_oe(fr_dout_oe), .on(fr_on), .armed(fr_armed)
);

end else begin : g_no_loader
    assign shap_lo = 8'h00; assign shap_stb = 1'b0;
    assign fr_dout = 8'h00; assign fr_dout_oe = 1'b0; assign fr_blk = 1'b0;
    assign fr_on = 1'b0; assign fr_sbr_req = 1'b0; assign fr_armed = 1'b0;
end endgenerate
// ------------------------------------------------- сброс с клавиатуры
// ВВОД или БЛК+СБР -- по первым чтениям после сброса: ПЗУ или ОЗУ; после ВВОД,
// если ESP включила режим (0x56), -- подставной загрузчик (rom_sense.v).
// Чтение, на которое отвечает сама плата (квазидиск, окно загрузчика), --
// не машины.
generate if (KBD_RST) begin : g_kbd_rst
rom_sense rsense (
    .clk(clk), .por(por), .sys_reset(sys_reset),
    .key_vvod(key_vvod), .key_sbr(key_sbr),
    .memrd(memrd), .din(shd_in), .ours(kv_on | fr_blk), .en(boot_kbd),
    .vvod_boot(kb_boot), .listen(kb_listen), .rom_live(kb_rom_live), .diag(kb_diag)
);
end else begin : g_no_kbd_rst
    assign kb_boot = 1'b0; assign kb_diag = 64'd0; assign kb_listen = 1'b0;
    assign kb_rom_live = 1'b0;
end endgenerate
// -------------------------------------------------------------------- ВИ53
// Таймер машины, собранный прослушиванием записей в порты 08h..0Bh (и разряд
// PC0 ВВ55): вывода `beep` с контакта 42 на ПЛИС нет. Чтения не
// обслуживаются -- на них отвечает ВИ53 самой машины.
// GATE всех трёх каналов у машины -- с контакта 41, «СБРОС», он у нас
// `sbros_n`. Такт -- от машины, два на видеоцикл
// (vi53_tick.v); без захвата различителя -- свой, 3/100 от 50 МГц. Подтверждения
// vid_phase на живой машине привязка не ждёт: процессорные спады ~RAS идут с
// тем же периодом, что видеоспады (vid_phase.v, «ПОЧЕМУ НЕ ПО ПЕРИОДУ»), так
// что захват даже не того потока даёт ту же частоту машины. `vi53_off`
// (команда 0x48 по SPI) глушит и таймер, и PC0: на машине с проводом на
// контакт 42 оба уже звучат с гнезда.
wire [2:0] vi_out;
wire       vi_pc0;
wire       vi_tick, vi_tick_en;
generate if (HAS_VI53) begin : g_vi53
vi53_tick vtick (
    .clk(clk), .reset(por), .vid_fall(ras_fall & vid_win), .vid_lock(vid_lock),
    .tick(vi_tick), .en(vi_tick_en)
);
vi53 #(.CE_NUM(3), .CE_DEN(100)) timer (    // 3/100 от 50 МГц = 1,5 МГц
    .clk(clk), .reset(reset),
    .port(bus_addr), .wr(iowr_reg), .din(shd_in),
    .gate(sbros_n), .tick_ext_en(vi_tick_en), .tick_ext(vi_tick),
    .out(vi_out), .pc0(vi_pc0)
);

end else begin : g_no_vi53
    assign vi_out = 3'b111; assign vi_pc0 = 1'b0;          // покой -- тишина
    assign vi_tick = 1'b0; assign vi_tick_en = 1'b0;
end endgenerate
// ------------------------------------------------------------------- звук
audio_out #(.SPLIT(AUDIO_SPLIT)) sound (
    .clk(clk), .reset(reset),
    .ch_a(ch_a), .ch_b(ch_b), .ch_c(ch_c), .beep(beep),
    .vi(vi53_off ? 3'b111 : vi_out), .pc0(vi_pc0 & ~vi53_off),   // 111 -- покой, тишина
    .pdm_l(audio_l), .pdm_r(audio_r)
);

// ------------------------------------------------------------------ ВГ93
// Ядро с платы (fdc.v, fdc_core.v, vendor/fdc1793) на портах 0x18..0x1C.
// Такт ядра -- 16 МГц прямо с ФАПЧ, без деления (CORE_CLK_DIV = 0).
// Готовность -- по индексу по умолчанию (fdc_rdy_sel, команда 0x4A),
// предкомпенсация записи -- по команде 0x4F (по сбросу выключена),
// выключатель -- разряд 0 dev_off.
//
// ШЛЕЙФ (fdd_pc_cable, разряд 2 байта 0x4A). XS10 разведён по Шугарту: DS0 на
// 10, DS1 на 12, мотор на 16, контакт 14 не подключён. У шлейфа ПК линии
// 10..16 между разъёмами перевёрнуты. На средней колодке (до перекрутки)
// Gotek на S1 слушает 12, на крайней 3,5" с перемычкой DS0 слушает 16, а его
// мотор (свой контакт 16) приходит с 10. Поэтому в режиме «шлейф ПК»:
//     контакт 12 -- выбор A:,  контакт 16 -- выбор B:,  контакт 10 -- мотор.
// Так Gotek -- A:, 3,5" -- B:. 3,5" с заводской DS1 за перекруткой ждёт 14 и
// не выберется ни в каком режиме.
wire [7:0] fdc_dout;
wire       fdc_dout_oe;
generate if (HAS_FDC) begin : g_has_fdc
wire c_ds0_n, c_ds1_n, c_mot_n;           // выходы ядра: A:, B:, мотор
fdc #(.BUSY_MIN(2500), .CORE_CLK_DIV(0), .RDY_TIMEOUT(50000000)) floppy (
    .clk(clk), .clk_fdc(clk_fdc), .clk_fdc_lock(clk_fdc_lock), .reset(reset),
    .enable(~dev_off[0]), .rdy_sel(fdc_rdy_sel),
    .precomp(fdc_pc[3:0]), .precomp_tg43(fdc_pc[4]),
    .port(bus_addr), .iord(iord), .iowr(iowr), .din(shd_in),
    .dout(fdc_dout), .dout_oe(fdc_dout_oe),
    .fdd_ds0_n(c_ds0_n), .fdd_ds1_n(c_ds1_n), .fdd_moton_n(c_mot_n),
    .fdd_dir_n(fdd_dir_n), .fdd_step_n(fdd_step_n), .fdd_wdata_n(fdd_wdata_n),
    .fdd_wgate_n(fdd_wgate_n), .fdd_side1_n(fdd_side1_n),
    .fdd_index_n(fdd_index_n), .fdd_trk00_n(fdd_trk00_n), .fdd_wpt_n(fdd_wpt_n),
    .fdd_rdata_n(fdd_rdata_n), .fdd_rdy_n(fdd_rdy_n),
    .drq(), .intrq(), .dbg(fdc_dbg)
);
assign fdd_ds0_n   = fdd_pc_cable ? c_mot_n : c_ds0_n;    // контакт 10
assign fdd_ds1_n   = fdd_pc_cable ? c_ds0_n : c_ds1_n;    // контакт 12
assign fdd_moton_n = fdd_pc_cable ? c_ds1_n : c_mot_n;    // контакт 16

end else begin : g_no_fdc
    assign fdc_dout = 8'h00; assign fdc_dout_oe = 1'b0;
    assign fdd_ds0_n = 1'b1; assign fdd_ds1_n = 1'b1; assign fdd_moton_n = 1'b1;
    assign fdd_dir_n = 1'b1; assign fdd_step_n = 1'b1; assign fdd_wdata_n = 1'b1;
    assign fdd_wgate_n = 1'b1; assign fdd_side1_n = 1'b1;
    assign fdc_dbg = 64'd0;
end endgenerate
// ------------------------------------------------------------------ мышь
// Интерфейс KTSerg на D4h, регистры в кристалле (mouse.v). Отчёты USB-мыши
// присылает ESP пакетом 0x42 по SPI, когда ему удобно; чтение с шины мгновенное.
wire [7:0] ms_dout;
wire       ms_dout_oe;
generate if (HAS_MOUSE) begin : g_has_mouse
mouse mouse (
    .clk(clk), .reset(reset),
    .port(bus_addr), .iord(iord), .iowr_reg(iowr_reg), .din(shd_in),
    .dout(ms_dout), .dout_oe(ms_dout_oe),
    .rep_stb(ms_stb), .rep_btn(ms_btn), .rep_dx(ms_dx), .rep_dy(ms_dy),
    .rep_wh(ms_wh), .present(ms_present)
);

end else begin : g_no_mouse
    assign ms_dout = 8'h00; assign ms_dout_oe = 1'b0;
end endgenerate
// ------------------------------------------------------------ канал
// Порты 28h-2Dh в раскладке UART 16550 для программ на «Векторе»: Wi-Fi
// модем ESP и настройки платы с самой машины (CL.COM). Выключателя в dev_off
// у канала нет: без ESP LSR читается 00h -- ни байта, ни места.
wire [7:0] ch_dout;
wire       ch_dout_oe;
generate if (HAS_CHAN) begin : g_has_chan
vchan chan (
    .clk(clk), .por(por),
    .port(bus_addr), .iord(iord), .iowr_reg(iowr_reg), .din(shd_in),
    .dout(ch_dout), .dout_oe(ch_dout_oe),
    .esp_alive(ch_alive), .esp_poll(ch_poll), .esp_push(ch_push), .esp_wdata(ch_wdata),
    .esp_pop(ch_pop), .v2e_head(ch_head), .v2e_avail(ch_avail), .e2v_empty(ch_e2v_empty),
    .flushed(ch_flushed)
);
end else begin : g_no_chan
    assign ch_dout = 8'h00; assign ch_dout_oe = 1'b0;
    assign ch_head = 8'h00; assign ch_avail = 1'b0; assign ch_e2v_empty = 1'b1;
    assign ch_flushed = 1'b0;
end endgenerate
// ------------------------------------------------------ генератор кадра
// Диагностика генератора кадра (блоки 4 и 5, см. ниже) -- снаружи генерации,
// чтобы без видео читались нулями.
wire [7:0] vd_underrun, vd_aborts, vd_ovf, vd_lockerr, vd_slipv, vd_sliph;
wire [7:0] vd_scroll_hw, vd_last_s, vd_line, vd_fetched;
wire [4:0] vd_last_v;
wire       vd_rast_ok, vd_m60, vd_scroll_ok;

generate if (VIDEO) begin : g_video
    wire [8:0]  r_line;  wire [7:0] r_pos;  wire r_lstb, r_fstb, r_ok;
    wire [7:0]  v_scroll, v_ovf, v_underrun, v_aborts, r_scroll_hw;
    wire        r_scroll_ok;
    wire        rd_stb;  wire [1:0] rd_kind; wire [7:0] rd_a, rd_b;  // растр -> журнал портов
    wire [12:0] ev_ra;   wire [17:0] ev_q;
    wire [12:0] sn_ra;   wire [8:0]  sn_q;
    wire [8:0]  j_line, j_fbl, j_fbp;  wire [15:0] j_vml, j_vmp;   // кольцо журнала
    wire        f_tog;   wire [7:0] f_row;  wire [8:0] f_done;
    wire        lb_we;   wire [11:0] lb_wa; wire [7:0] lb_wd;
    wire [7:0]  f_lbrow, f_scr;
    wire        p_m60;   wire [7:0] p_lockerr;
    wire        p_ce;                // разрешение такта проигрывателя (50 Гц -- через такт)
    wire [9:0]  p_vrem;              // строк показа до frame_tog (vid_fetch.v)
    // строка 0 машины -- в домен точки переключаемым признаком
    reg         mf_tog = 1'b0;
    always @(posedge clk) if (r_lstb && r_line == 9'd0) mf_tog <= ~mf_tog;
    // Прокрутка для выборки: какой её взяла машина (по адресу видеоцикла),
    // а без опоры по адресу -- порт A, снятый в строке FRAME_LINE.
    wire [7:0]  f_scroll = r_scroll_ok ? r_scroll_hw : v_scroll;

    vid_raster raster (
        .clk(clk), .reset(por), .ras_n(ras_n), .shap(shap), .vid_win(vid_win),
        .vid_lock(vid_lock), .strob(strob), .shd(shd_in),
        .line(r_line), .pos(r_pos), .line_stb(r_lstb), .frame_stb(r_fstb), .ok(r_ok),
        .scroll_hw(r_scroll_hw), .scroll_hw_ok(r_scroll_ok),
        .slip_v(vd_slipv), .slip_h(vd_sliph), .last_v(vd_last_v), .last_s(vd_last_s),
        .dbg_stb(rd_stb), .dbg_kind(rd_kind), .dbg_a(rd_a), .dbg_b(rd_b)
    );
    vid_rec rec (
        .clk(clk), .reset(por), .iowr_reg(iowr_reg), .port(bus_addr), .din(shd_in),
        .rast_ok(r_ok), .line_stb(r_lstb), .line_no(r_line), .pos(r_pos),
        .frame_stb(r_fstb), .scroll(v_scroll),
        .rec_line(j_line), .fb_last(j_fbl), .fb_prev(j_fbp), .vm_last(j_vml), .vm_prev(j_vmp),
        .rclk(clk_vga), .rce(p_ce), .ev_raddr(ev_ra), .ev_q(ev_q), .sn_raddr(sn_ra), .sn_q(sn_q),
        .ovf(v_ovf), .pal_ra(pal_ra), .pal_rd(pal_rd), .lg_hold(pal_busy),
        .rd_stb(rd_stb), .rd_kind(rd_kind), .rd_a(rd_a), .rd_b(rd_b)
    );
    vid_fetch fetch (
        .clk(clk), .reset(por),
        .ras_n(ras_n), .chtzu_n(chtzu_n), .zpzu_n(zpzu_n), .chtvv_n(chtvv_n),
        .zpvv_n(zpvv_n), .strob(strob), .shd(shd_in),
        .frame_tog(f_tog), .cur_row(f_row), .fetched(f_done), .scroll(f_scroll),
        .vrem(p_vrem), .m60(p_m60),
        .lb_we(lb_we), .lb_waddr(lb_wa), .lb_wdata(lb_wd),
        .addr_cpc(addr_cpc), .cpc_d0(addr_cpc_d0), .cpc_d1(addr_cpc_d1),
        .addr_mrc(addr_mrc), .addr_src(addr_src),
        .sram_oe_n(sram_oe_n), .rd_active(vid_rd), .aborts(v_aborts),
        .lb_row(f_lbrow), .scroll_now(f_scr)
    );
    vid_play play (
        .clk(clk_vga), .mode60(vga60), .mframe_tog(mf_tog),
        .ev_raddr(ev_ra), .ev_q(ev_q), .sn_raddr(sn_ra), .sn_q(sn_q),
        .rec_line(j_line), .fb_last(j_fbl), .fb_prev(j_fbp), .vm_last(j_vml), .vm_prev(j_vmp),
        .wclk(clk), .lb_we(lb_we), .lb_waddr(lb_wa), .lb_wdata(lb_wd),
        .frame_tog(f_tog), .cur_row(f_row), .fetched(f_done),
        .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs),
        .underrun(v_underrun), .m60_o(p_m60), .vrem_o(p_vrem), .ce_o(p_ce), .lock_err_o(p_lockerr)
    );
    scr_buf scr (
        .clk(clk), .por(por),
        .req_stb(scr_req_stb), .req_row(scr_req_row),
        .rd_start(sr_start), .rd_next(sr_next), .rd_byte(sr_byte),
        .f_we(lb_we), .f_row(f_lbrow), .f_addr(lb_wa[6:0]), .f_data(lb_wd), .f_scroll(f_scr)
    );

    // из домена точки -- через два триггера: меняются редко, это показания
    reg [7:0] u1 = 8'd0, u2 = 8'd0, e1 = 8'd0, e2 = 8'd0;
    reg [1:0] m6 = 2'b00;
    always @(posedge clk) begin
        u1 <= v_underrun; u2 <= u1;
        e1 <= p_lockerr;  e2 <= e1;
        m6 <= {m6[0], p_m60};
    end
    assign vd_underrun = u2;
    assign vd_lockerr  = e2;
    assign vd_m60      = m6[1];
    assign vd_aborts   = v_aborts;
    assign vd_ovf      = v_ovf;
    assign vd_rast_ok  = r_ok;
    assign vd_scroll_ok = r_scroll_ok;
    assign vd_scroll_hw = r_scroll_hw;
    assign vd_line     = r_line[7:0];
    assign vd_fetched  = f_done[7:0];
end else begin : g_novideo
    // Чёрный кадр и синхра в пассиве: монитор покажет «нет сигнала».
    assign vga_r = 3'd0; assign vga_g = 3'd0; assign vga_b = 2'd0;
    assign vga_hs = 1'b0; assign vga_vs = 1'b0;
    assign addr_cpc = 1'b0; assign addr_mrc = 1'b0; assign addr_src = 1'b0;
    assign addr_cpc_d0 = 1'b0; assign addr_cpc_d1 = 1'b0;
    assign sram_oe_n = 1'b1; assign vid_rd = 1'b0;
    assign sr_byte = 8'h00;
    assign vd_underrun = 8'd0; assign vd_aborts = 8'd0; assign vd_ovf = 8'd0;
    assign vd_lockerr = 8'd0; assign vd_slipv = 8'd0; assign vd_sliph = 8'd0;
    assign vd_scroll_hw = 8'd0; assign vd_last_s = 8'd0; assign vd_last_v = 5'd0;
    assign vd_line = 8'd0; assign vd_fetched = 8'd0;
    assign vd_rast_ok = 1'b0; assign vd_m60 = 1'b0; assign vd_scroll_ok = 1'b0;
    assign pal_rd = 8'h00;
end endgenerate

// -------------------------------------------------- арбитраж шины данных
// Одновременно претендовать не могут: порты IDE, AY, ВГ93, мыши и канала не пересекаются,
// а подставной загрузчик отвечает только на чтения памяти.
// Квазидиск здесь не участвует — ОЗУ отвечает на шину само.
wire       bus_oe   = ide_cpu_dout_oe | ay_dout_oe | ms_dout_oe | fdc_dout_oe | fr_dout_oe
                    | ch_dout_oe;
wire [7:0] bus_dout = ide_cpu_dout_oe ? ide_cpu_dout
                    : ay_dout_oe      ? ay_dout
                    : fdc_dout_oe     ? fdc_dout
                    : fr_dout_oe      ? fr_dout
                    : ch_dout_oe      ? ch_dout
                    :                   ms_dout;

assign shd     = bus_oe ? bus_dout : 8'hZZ;
assign shd_dir = bus_oe;

// -------------------------------------------------------- буфер данных
// SN74LVC8T245 режет ШД: сторона разъёма и сторона памяти (статика, DD3).
//   процессор читает квазидиск, подставное ПЗУ, наш порт -> память -> разъём;
//   процессор пишет (что угодно: квазидиск, зеркало, порты, палитру) -> к памяти;
//   под СТРОБ СОСТ. -> к памяти: слово состояния для опоры по INTA
//     (vid_raster.v) -- без этого ПЛИС видит подтяжки RN1, RN2, нули;
//   иначе выключен, в том числе в нашем чтении кадра (там стробов нет).
// НАПРАВЛЕНИЕ -- по ОЧИЩЕННЫМ стробам (memrd, bus_oe из iord): на ~ЧТЗУ замерен
// звон с провалами 30-70 нс, и DIR от сырого строба перекинулся бы посреди
// записи. ВКЛЮЧЕНИЕ -- по СЫРЫМ, комбинационно, как latch_clk:
// гаснет ровно со стробом машины. По очищенному хвост в 20-40 нс дрался бы с
// D19, который ведёт ШД всегда, кроме чтений процессора. Чтение включает буфер
// только при направлении «к разъёму», запись -- только «к памяти»: звон одного
// строба во время другого буфер не откроет навстречу.
// СТРОБ СОСТ.: слово состояния ведёт
// D19, сторона памяти в этот момент ничья -- статика не выбрана (её /OE только
// под ~ЧТЗУ), DD3 смотрит к ПЛИС (shd_dir = bus_oe, это наши чтения), наше
// чтение кадра под стробом отбито (vid_fetch.v). Открывается тоже по сырому.
// Чтение включает буфер через ~60 нс после спада ~ЧТЗУ: направление ждёт
// очищенного строба, и это нарочно -- ~ЧТЗУ держится от 580 нс.
// ПЕРВЫЕ ЧТЕНИЯ ПОСЛЕ СБРОСА (rom_sense.v) -- тоже к памяти: ПЛИС должна
// видеть байты ПЗУ или ОЗУ машины. Сторона памяти в этот момент ничья:
// квазидиск после сброса выключен, чтение кадра гаснет по сырому ~ЧТЗУ
// (vid_fetch.v), зеркало выбирает статику только на записи. В окне
// загрузчика не открываем: там на чтение отвечаем мы сами.
wire buf_rd = (memrd & (kv_on | fr_blk)) | bus_oe;   // процессор читает у нас
wire kb_rd  = kb_listen & ~fr_on & ~kv_on & ~chtzu_n;
assign buf_dir  = buf_rd;
assign buf_oe_n = ~((buf_rd & (~chtzu_n | ~chtvv_n)) |
                    (~buf_rd & (~zpzu_n | ~zpvv_n | strob | kb_rd)));

// ------------------------------------------------------------- SPI к ESP32
// ESP32 держит файловую систему, а сектора читает и пишет через нас --
// контроллер IDE уже здесь, второй ведущий на шине привода не нужен.
// Выход ведомого -- высокий импеданс, когда не выбран (esp_miso_oe от
// spi_slave): так линия не мешает, если на ней окажется кто-то ещё.
assign esp_miso = esp_miso_oe ? esp_miso_o : 1'bz;

// ------------------------------------------------------- светодиод привода
// Обращение к приводу длится микросекунды, глазом не увидеть, поэтому
// растягиваем.
reg [27:0] led_cnt;
always @(posedge clk) begin
    if (reset)
        led_cnt <= 28'd0;
    else if (~ide_cs0_n)
        led_cnt <= LED_TICKS;
    else if (led_cnt != 28'd0)
        led_cnt <= led_cnt - 1'b1;
end
assign ide_led_n = (led_cnt == 28'd0);

// ------------------------------------------------ необязательные выводы
// Логики за ними пока нет, и rev4_top.v наружу их не выводит. Держим их в
// отжатом состоянии явно, чтобы синтез не додумывал.
//
// `ide_cs1_n` — выборка блока управления ATA (контакт 38). Само ядро `ide`
// держит его на единице, «как в оригинале». Понадобится, когда будет нужен
// Device Control:
// программный сброс SRST и запрет прерывания nIEN, а также Alternate Status.

// `esp_attn` — «обслужи меня» модулю ESP. Пока SPI ведёт ESP и опрашивает
// ведомую ПЛИС, этого хватает для передачи файлов. Понадобится, когда
// появится виртуальная дискета: там задержка опроса это скорость дисковода.
assign esp_attn = 1'b0;

// ------------------------------------------- диагностика новых узлов по SPI
// Блок 4, 0x60..0x67:
//   0  {загрузчик: окно, взведён; клавиши: СБР, ВВОД; привязка 50 Гц точна,
//       VGA в 60 Гц, захват различителя, опора растра}
//   1  опозданий строки к показу      2  отбоев цикла выборки
//   3  событий журнала сверх 16       4  ошибка привязки 50 Гц, строк со знаком
//   5  поправка растра по вертикали   6  по горизонтали (калибровка vid_raster)
//   7  записей машины мимо зеркала (mirror.v, замер ~ЗПЗУ относительно ~RAS)
// Блок 5, 0x68..0x6F:
//   0  прокрутка, какой её взяла машина   1  S в строке калибровки
//   2  {опора по адресу, есть палитра для снимка (0x57), ФАПЧ держит захват с
//       первого захвата без срывов (pll_ok), колонка перед нулём}   3  строка растра
//   4  дисковод {готов 34, запись, защита, дорожка 0, индекс, мотор, B:, A:}
//   5  строк кадра выбрано
//   6  {версия раскладки 1, KBD_RST, AUDIO_SPLIT, HAS_KVAZ2, HAS_AY}
//   7  {VIDEO, HAS_WATCH, HAS_MIRROR, HAS_LOADER, HAS_KEYS, HAS_MOUSE, HAS_VI53, HAS_FDC}
// Счётчики насыщаются и сбрасываются только по питанию.
// ФАПЧ: захват есть и с первого захвата ни разу не срывался. Один разряд на
// оба признака: нуль и при «нет захвата», и при «был срыв» -- для разбора
// дрожания VGA этого хватает (pll_fdc.v).
reg [1:0] pll_s = 2'b00;
reg       pll_seen = 1'b0, pll_lost = 1'b0;
always @(posedge clk) begin
    pll_s <= {pll_s[0], clk_fdc_lock};
    if (pll_s[1])  pll_seen <= 1'b1;
    if (pll_seen & ~pll_s[1]) pll_lost <= 1'b1;
end
wire pll_ok = pll_s[1] & ~pll_lost;

wire lock_good = (vd_lockerr == 8'h00) | (vd_lockerr == 8'h01) | (vd_lockerr == 8'hFF);
assign diag4 = {mir_miss, vd_sliph, vd_slipv, vd_lockerr, vd_ovf, vd_aborts, vd_underrun,
                fr_on, fr_armed, key_sbr, key_vvod, lock_good, vd_m60, vid_lock, vd_rast_ok};
assign diag5 = {(VIDEO != 0), (HAS_WATCH != 0), (HAS_MIRROR != 0), (HAS_LOADER != 0),
                (HAS_KEYS != 0), (HAS_MOUSE != 0), (HAS_VI53 != 0), (HAS_FDC != 0),
                4'h1, (KBD_RST != 0), (AUDIO_SPLIT != 0), (HAS_KVAZ2 != 0), (HAS_AY != 0),
                vd_fetched,
                ~fdd_rdy_n, ~fdd_wgate_n, ~fdd_wpt_n, ~fdd_trk00_n, ~fdd_index_n,
                ~fdd_moton_n, ~fdd_ds1_n, ~fdd_ds0_n,
                vd_line,
                vd_scroll_ok, (VIDEO != 0), pll_ok, vd_last_v,
                vd_last_s,
                vd_scroll_hw};

endmodule

`default_nettype wire
