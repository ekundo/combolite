// USB-мышь «Вектора»: интерфейс KTSerg на порту D4h, регистры держит ПЛИС.
//
// ИНТЕРФЕЙС -- ЧУЖОЙ, И ЭТО НАРОЧНО. Контроллер KTSerg (карта регистров -- в
// шапке его теста mous_r12.asm) уже понимают
// arkanoid, mines_m, COLLINES и эмулятор VV. Свой был бы только хуже.
//
// АРХИТЕКТУРА -- НАША. У KTSerg ПЛИС -- мост к соседнему STM32, и программа
// читает `OUT 0D4h / IN 0D4h` без признака готовности: не успел контроллер --
// прочлось старое, молча. У нас на месте STM32 -- ESP32 с FreeRTOS и Wi-Fi,
// он опаздывал бы на десятки микросекунд. Поэтому регистры здесь, в кристалле:
// ESP присылает отчёты мыши когда ему удобно (пакет 0x42 по SPI, ide_remote.v),
// а чтение с шины отвечает мгновенно.
//
// ПОРТ D4h. Запись -- управляющее слово: разряд 5 = 1 выключает контроллер и
// ставит курсор в 128/128, разряды 3:0 -- номер регистра. Чтение -- выбранный
// регистр. По сбросу слово FFh, то есть выключен (как у KTSerg). Отвечаем, только
// когда включён И ESP сообщил, что мышь есть: иначе порт молчит, на шине FFh, и
// игры переходят на обычное управление, как без контроллера.
//
// РЕГИСТРЫ (шапка mous_r12.asm):
//   0  кнопки: 7 левая, 6 правая, 5 средняя; разряд 0 -- младший разряд X для
//      режима 512, ОБНОВЛЯЕТСЯ ТОЛЬКО ПРИ ЧТЕНИИ РЕГИСТРА 1
//   1  X курсора 0..255       2  Y курсора 0..255 (у «Вектора» вверх -- плюс)
//   3  колесо: смещение между чтениями этого регистра, -128..127
//   4  смещение X между чтениями ЭТОГО регистра, -128..127
//   5  смещение Y, так же (вверх -- плюс)
//   6  смещение X 0..127, разряд 7 -- «влево»
//   7  смещение Y 0..127, разряд 7 -- «вниз»
//   8  кнопки: 0 левая, 1 правая, 2 средняя
//   9  сырое смещение X, -127..+127     10  сырое Y, так же
//   15 идентификатор 6Dh («m»)          прочие -- 00h
//
// СКОРОСТЬ ПОДЕЛЕНА НА ВОСЕМЬ, как у KTSerg: иначе курсор пробегал экран за
// сантиметр коврика. Координаты и смещения 4..7 считаются в восьмых долях точки;
// 9 и 10 -- сырые, без деления. Остаток не теряется: чтение вычитает из
// накопителя ровно отданное, и то, что не влезло в байт, отдастся следующим
// чтением (предложение Славинского в теме, «копить и отдавать по частям»).
// Знак Y: USB считает вниз плюсом, «Вектор» -- вверх (у теста верх экрана --
// строка FFh), поэтому Y отчёта переворачивается.
//
// ЧТЕНИЕ. Отдаваемый байт и то, что вычесть, защёлкиваются на ВЕСЬ цикл чтения
// (замораживаются по первому такту строба), вычитание -- по его концу: отчёт,
// пришедший посреди цикла, не портит байт и не теряется.
`default_nettype none

module mouse #(
    parameter [7:0] PORT = 8'hD4
) (
    input  wire       clk,
    input  wire       reset,

    // шина машины
    input  wire [7:0] port,             // адрес ввода-вывода, прямой
    input  wire       iord,             // чтение порта, уровнем на весь цикл
    input  wire       iowr_reg,         // один такт на цикл записи
    input  wire [7:0] din,
    output wire [7:0] dout,
    output wire       dout_oe,

    // от ESP, через ide_remote.v
    input  wire       rep_stb,          // пришёл отчёт
    input  wire [7:0] rep_btn,          // 0 левая, 1 правая, 2 средняя (как в USB HID)
    input  wire [7:0] rep_dx,           // со знаком, вправо -- плюс
    input  wire [7:0] rep_dy,           // со знаком, ВНИЗ -- плюс (USB)
    input  wire [7:0] rep_wh,           // со знаком
    input  wire       present           // мышь подключена
);

// ЭКОНОМИЯ. Семь накопителей со своими сумматорами «+отчёт -отданное» и
// параллельное оформление всех регистров стоили бы 1 192 eq. Здесь ОДИН
// сумматор с насыщением на всё: отчёт раскладывается по
// накопителям за девять тактов (180 нс, а отчёты идут раз в миллисекунды), вычет
// после чтения -- ещё одна операция на нём же. И оформление одно: сначала
// выбирается накопитель нужного регистра, потом один сдвиг и одно ограничение.

// Ширина накопителей: 11 разрядов, +-1023. Для смещений 4..7 это +-127 точек --
// столько и отдаётся за одно чтение; программа, опрашивающая мышь хоть раз в
// кадр, упора не увидит. Сырые 9, 10 и колесо -- +-1023 отсчёта.
localparam integer AW = 11;
localparam signed [AW-1:0] A_MAX = (1 << (AW-1)) - 1;

reg  [7:0]  ctrl;
wire        off  = ctrl[5];
wire [3:0]  rsel = ctrl[3:0];
wire        wr_me = iowr_reg & (port == PORT);

reg  [10:0] px, py;                     // курсор в восьмых долях точки, 0..2047
reg         xlsb;                       // разряд 0 регистра 0
reg  [2:0]  btn;                        // левая, правая, средняя

// Накопители: 0 X(4)  1 Y(5)  2 X(6)  3 Y(7)  -- восьмые доли точки;
//             4 X(9)  5 Y(10) 6 колесо(3)    -- сырые отсчёты.
reg  signed [AW-1:0] acc0, acc1, acc2, acc3, acc4, acc5, acc6;
function signed [AW-1:0] acc_of(input [2:0] i,
    input signed [AW-1:0] a0, a1, a2, a3, a4, a5, a6);
    case (i) 3'd0: acc_of = a0; 3'd1: acc_of = a1; 3'd2: acc_of = a2; 3'd3: acc_of = a3;
             3'd4: acc_of = a4; 3'd5: acc_of = a5; default: acc_of = a6; endcase
endfunction

// ---- регистр -> накопитель и оформление, одно на всех
reg  [2:0]  ai;                         // какой накопитель у регистра rsel
reg         has_acc, scaled, sgnmag;
always @(*) begin
    has_acc = 1'b1; scaled = 1'b0; sgnmag = 1'b0; ai = 3'd0;
    case (rsel)
        4'd3:  ai = 3'd6;
        4'd4:  begin ai = 3'd0; scaled = 1'b1; end
        4'd5:  begin ai = 3'd1; scaled = 1'b1; end
        4'd6:  begin ai = 3'd2; scaled = 1'b1; sgnmag = 1'b1; end
        4'd7:  begin ai = 3'd3; scaled = 1'b1; sgnmag = 1'b1; end
        4'd9:  ai = 3'd4;
        4'd10: ai = 3'd5;
        default: has_acc = 1'b0;
    endcase
end
wire signed [AW-1:0] a_sel = acc_of(ai, acc0, acc1, acc2, acc3, acc4, acc5, acc6);
wire               raw   = (rsel == 4'd9) | (rsel == 4'd10);
// Ступень чтения. В один такт номер регистра -> выбор накопителя -> модуль и
// насыщение -> байт не влезал в 20 нс (по Diamond 19 нс, 13 уровней, запас
// clk50 4 %). Выбранный и масштабированный накопитель
// и признаки регистра берутся в регистр, байт оформляется тактом позже.
// Регистр выбирается OUT задолго до IN, а показанное и вычитаемое (take) --
// из одного снимка, так что лишний такт ничего не меняет.
reg  [3:0]          rsel_r = 4'd0;
reg  [2:0]          ai_r = 3'd0;
reg                 has_acc_r = 1'b0, scaled_r = 1'b0, sgnmag_r = 1'b0, raw_r = 1'b0;
reg  signed [AW-1:0] a_v = 0;
always @(posedge clk) begin
    rsel_r <= rsel; ai_r <= ai; has_acc_r <= has_acc;
    scaled_r <= scaled; sgnmag_r <= sgnmag; raw_r <= raw;
    a_v <= scaled ? (a_sel >>> 3) : a_sel;
end
wire               a_neg = a_v[AW-1];
wire signed [AW-1:0] a_abs = a_neg ? -a_v : a_v;
// знаковые: -128..127 (сырые 9, 10 -- -127..127); модуль: 0..127
wire signed [AW-1:0] a_lo  = raw_r ? -127 : -128;
wire [7:0]  q_sgn = (a_v < a_lo) ? a_lo[7:0] : (a_v > 127) ? 8'd127 : a_v[7:0];
wire [6:0]  q_mag = (a_abs > 127) ? 7'd127 : a_abs[6:0];
wire signed [AW-1:0] q_val = sgnmag_r ? (a_neg ? -{{(AW-7){1'b0}}, q_mag} : {{(AW-7){1'b0}}, q_mag})
                                  : {{(AW-8){q_sgn[7]}}, q_sgn};

reg  [7:0]  cur;
always @(*) case (rsel_r)
    4'd0:  cur = {btn[0], btn[1], btn[2], 4'b0000, xlsb};
    4'd1:  cur = px[10:3];
    4'd2:  cur = py[10:3];
    4'd8:  cur = {5'b00000, btn[2], btn[1], btn[0]};
    4'd15: cur = 8'h6D;
    default: cur = has_acc_r ? (sgnmag_r ? {a_neg, q_mag} : q_sgn) : 8'h00;
endcase
wire signed [AW-1:0] cur_take = has_acc_r ? (scaled_r ? (q_val <<< 3) : q_val) : 0;

// ---- цикл чтения: заморозить по началу, вычесть по концу
wire        rd_me = iord & (port == PORT);
reg         rd_d;
reg  [7:0]  q;
reg  signed [AW-1:0] take;
reg  [2:0]  take_ai;
reg         take_has;
always @(posedge clk)
    if (reset) begin rd_d <= 1'b0; q <= 8'h00; take <= 0; take_ai <= 3'd0; take_has <= 1'b0; end
    else begin
        rd_d <= rd_me;
        if (!rd_me) begin q <= cur; take <= cur_take; take_ai <= ai_r; take_has <= has_acc_r; end
    end
wire rd_end = rd_d & ~rd_me;
wire rd_beg = rd_me & ~rd_d;

assign dout    = rd_beg ? cur : q;          // первый такт -- прямо, дальше защёлка
assign dout_oe = rd_me & ~off & present;

// ---- один сумматор с насыщением, по очереди
// Операции: вычет отданного (tk_pend) -- в первую очередь; раскладка отчёта
// (rp_step 1..9): семь накопителей, потом X и Y курсора (упор 0..2047).
// Конвейер в две ступени: в первой снимаются операнды и заявка считается
// принятой, во второй -- сумма, насыщение и запись. В один такт выбор
// накопителя из семи, сумма и насыщение не влезали в 20 нс (по Diamond 22 нс,
// 14 уровней). Соседние шаги отчёта пишут в разные накопители, и он
// раскладывается по шагу за такт. Если же
// снимаемая операция трогает то, что вторая ступень пишет сейчас (вычет
// отданного и шаг отчёта в один накопитель), съём ждёт такт (hz).
reg                tk_pend;
reg  signed [AW-1:0] tk_val;
reg  [2:0]         tk_ai;
reg  [3:0]         rp_step;             // 0 -- отчёта нет
reg  signed [AW-1:0] rdx, rdy, rdw;       // отчёт, Y уже перевёрнут

wire        do_tk  = tk_pend;
wire        do_rp  = ~tk_pend & (rp_step != 4'd0);
wire [3:0]  st     = rp_step;
// операнды
wire [2:0]  op_i   = do_tk ? tk_ai : (st - 4'd1);        // накопитель шага 1..7
wire signed [AW-1:0] op_a = acc_of(op_i, acc0, acc1, acc2, acc3, acc4, acc5, acc6);
wire signed [AW-1:0] op_d = (st == 4'd1 || st == 4'd3 || st == 4'd5 || st == 4'd8) ? rdx
                        : (st == 4'd7) ? rdw : rdy;
wire        on_xy  = do_rp & (st >= 4'd8);                // курсор, а не накопитель
// Сумма -- 13 разрядов при любой ширине накопителей: курсор px + dx доходит до
// 2047 + 127, и в AW+1 = 12 разрядах со знаком это заворачивало бы в минус
// (курсор у правого края прыгал бы в нуль).
wire signed [12:0] ea  = {{(13-AW){op_a[AW-1]}}, op_a};
wire signed [12:0] ed  = {{(13-AW){op_d[AW-1]}}, op_d};
wire signed [12:0] et  = {{(13-AW){tk_val[AW-1]}}, tk_val};
reg                xv = 1'b0;           // во второй ступени есть операция
reg  signed [12:0] x_a, x_d;
reg                x_sub, x_xy, x_x, x_acc;
reg  [2:0]         x_i;
wire signed [12:0] sum = x_sub ? (x_a - x_d) : (x_a + x_d);
wire signed [12:0] amax = A_MAX;
wire signed [AW-1:0] sum_acc = (sum >  amax) ?  A_MAX
                             : (sum < -amax) ? -A_MAX : sum[AW-1:0];
wire        [10:0] sum_xy  = (sum < 0) ? 11'd0 : (sum > 13'sd2047) ? 11'd2047 : sum[10:0];
wire        wr_acc = do_tk | (do_rp & (st <= 4'd7));
wire        hz     = xv & ((x_acc & wr_acc & (x_i == op_i)) |
                           (x_xy & on_xy & (x_x == (st == 4'd8))));
wire        cap    = (do_tk | do_rp) & ~hz;

always @(posedge clk) begin
    if (reset) begin
        ctrl <= 8'hFF;
        px <= 11'd1024; py <= 11'd1024; xlsb <= 1'b0; btn <= 3'b000;
        acc0 <= 0; acc1 <= 0; acc2 <= 0; acc3 <= 0; acc4 <= 0; acc5 <= 0; acc6 <= 0;
        tk_pend <= 1'b0; tk_val <= 0; tk_ai <= 0; rp_step <= 4'd0;
        rdx <= 0; rdy <= 0; rdw <= 0;
        xv <= 1'b0;
    end else if (wr_me) begin
        ctrl <= din;
        if (din[5]) begin                       // выключить и сбросить: 128/128
            px <= 11'd1024; py <= 11'd1024; xlsb <= 1'b0; btn <= 3'b000;
            acc0 <= 0; acc1 <= 0; acc2 <= 0; acc3 <= 0; acc4 <= 0; acc5 <= 0; acc6 <= 0;
            tk_pend <= 1'b0; rp_step <= 4'd0;
            xv <= 1'b0;                         // снятая операция -- уже не нужна
        end
    end else begin
        // разряд 0 регистра 0 -- по чтению регистра 1, тем же значением X
        if (rd_beg && rsel == 4'd1) xlsb <= px[2];
        // заявки
        if (rd_end && take_has) begin tk_pend <= 1'b1; tk_val <= take; tk_ai <= take_ai; end
        if (rep_stb && !off) begin
            btn <= rep_btn[2:0];
            rdx <=  {{(AW-8){rep_dx[7]}}, rep_dx};
            rdy <= -{{(AW-8){rep_dy[7]}}, rep_dy};
            rdw <=  {{(AW-8){rep_wh[7]}}, rep_wh};
            rp_step <= 4'd1;                    // новый отчёт; недоразложенный старый
        end                                     // уже почти весь разложен -- 10 тактов
        // ступень 2: сумма и запись
        if (xv) begin
            if (x_acc) case (x_i)
                3'd0: acc0 <= sum_acc; 3'd1: acc1 <= sum_acc; 3'd2: acc2 <= sum_acc;
                3'd3: acc3 <= sum_acc; 3'd4: acc4 <= sum_acc; 3'd5: acc5 <= sum_acc;
                default: acc6 <= sum_acc;
            endcase
            if (x_xy) begin
                if (x_x) px <= sum_xy; else py <= sum_xy;
            end
        end
        // ступень 1: операнды
        xv <= cap;
        if (cap) begin
            x_a   <= on_xy ? {2'b00, (st == 4'd8) ? px : py} : ea;
            x_d   <= do_tk ? et : ed;
            x_sub <= do_tk;
            x_xy  <= on_xy;
            x_x   <= (st == 4'd8);
            x_acc <= wr_acc;
            x_i   <= op_i;
            if (do_tk && !(rd_end && take_has)) tk_pend <= 1'b0;
            if (do_rp && !(rep_stb && !off)) rp_step <= (st == 4'd9) ? 4'd0 : st + 4'd1;
        end
    end
end

endmodule

`default_nettype wire
