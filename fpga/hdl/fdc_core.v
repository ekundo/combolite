// Верхний уровень чужого ядра ВГ93 -- наш, вместо fdc_emul.v исходного ядра.
//
// Зачем свой. fdc_emul.v написан под гнездо настоящей КР1818ВГ93: байт идёт
// через двунаправленный FDC_DATA, WF/VFOE тоже двунаправленный. Внутри ПЛИС
// трёхстабильной шины нет, синтезатор разворачивает её в мультиплексор как
// сумеет. Занятость с того же вывода брать нельзя: когда чтения нет, там
// стоит НАШ байт записи, и «занятость» взводилась бы от разряда 0 любого
// записанного байта. Здесь вход и выход
// раздельные, а занятость идёт прямо из автомата (oBUSY, наша правка ядра).
//
// Всё остальное -- строка в строку как в fdc_emul.v: те же подмодули, те же
// сбросы признаков по обращению к регистрам данных и состояния. Подмодули ядра
// не тронуты, кроме правок, перечисленных в vendor/fdc1793/README.md. Кодер
// записи -- наш (mfm_wr.v) вместо MFMCDR.v: с настоящей предкомпенсацией; WG
// наружу идёт от него, вровень с данными.
//
// ТАКТ. Автомат ядра живёт на 16 МГц: от них посчитаны константы задержек, и
// запас по частоте -- четыре процента (замерено). CLK_DIV = 1 (по умолчанию,
// как в fdc_emul.v): на clk подают 32 МГц, внутри делим надвое. CLK_DIV = 0:
// на clk уже 16 МГц, деления нет -- так на плате, где 16 МГц даёт ФАПЧ.
`default_nettype none

module fdc_core #(
    parameter integer CLK_DIV = 1
) (
    input  wire       clk,          // 32 МГц при CLK_DIV = 1, 16 МГц при CLK_DIV = 0
    input  wire       nres,

    // ---- шина процессора, уровни активные низкие, как у ВГ93 ----
    input  wire [1:0] a,            // номер регистра по-вэдэшному: 00 состояние/команда ... 11 данные
    input  wire [7:0] din,
    output wire [7:0] dout,
    input  wire       ncs,
    input  wire       nwr,
    input  wire       nrd,

    output wire       busy,         // разряд ЗАНЯТО, в такте clk_16
    output wire       drq,
    output wire       intrq,

    // ---- дисковод, уровни как у настоящей ВГ93 ----
    input  wire       rawr_n,       // сырые данные с головки
    input  wire       wprt_n,
    input  wire       tr00_n,
    input  wire       index_n,
    input  wire       hrdy,         // голова опущена и успокоилась
    output wire       hld,
    output wire       wg,
    output wire       wr_data,
    output wire       step,
    output wire       dir,
    output wire       tg43,
    input  wire [3:0] precomp,      // предкомпенсация записи, тактов 16 МГц (62,5 нс)
    output wire       dbg_sync      // диагностика: декодер видит синхрослово метки (A1/C2), такт clk_16
);

wire clk_16;
generate if (CLK_DIV) begin : g_div
    reg r_clk_16 = 1'b0;            // без начального значения ~X = X
    always @(posedge clk) r_clk_16 <= ~r_clk_16;
    assign clk_16 = r_clk_16;
end else begin : g_nodiv
    assign clk_16 = clk;
end endgenerate

wire vfoe, wg_i, rawr, rclk, sync, start, rdtrk, byte_2_read, byte_2_write, translate,
     reset_crc, next_byte, bdi_drq, bdi_intrq, WDATA;
wire [3:0]  ip_cnt;
wire [47:0] words;
wire [7:0]  byte_2_main, main_2_byte;
wire [10:0] byte_cnt;
wire [15:0] crc16_d8;

wire bdi_wr_en = ~(ncs | nwr);

// ---- признаки обращения к регистрам состояния и данных (как в fdc_emul.v) ----
reg r_bdi_drq0, r_bdi_drq, r_bdi_intrq0, r_bdi_intrq;
reg r_intrq_r_sreg, r_drq_r_dreg;
always @(posedge clk) begin
    r_bdi_drq0   <= bdi_drq;   r_bdi_drq   <= r_bdi_drq0;
    r_bdi_intrq0 <= bdi_intrq; r_bdi_intrq <= r_bdi_intrq0;
end

always @(posedge clk)
    if (~nres)
        r_intrq_r_sreg <= 1'b0;
    else if (~r_intrq_r_sreg) begin
        if (~nrd && ~ncs && a == 2'b00) r_intrq_r_sreg <= 1'b1;   // чтение состояния
    end else if (~r_bdi_intrq)
        r_intrq_r_sreg <= 1'b0;

always @(posedge clk)
    if (~nres)
        r_drq_r_dreg <= 1'b0;
    else if (~r_drq_r_dreg) begin
        if (~(nwr & nrd) && ~ncs && a == 2'b11) r_drq_r_dreg <= 1'b1;  // обращение к данным
    end else if (~r_bdi_drq)
        r_drq_r_dreg <= 1'b0;

assign drq     = bdi_drq;
assign dbg_sync = sync;
assign intrq   = bdi_intrq;
assign wr_data = ~WDATA;

Main_CTRL U14 (
    .iCLK(clk_16), .iRESETn(nres), .iWR_EN(bdi_wr_en), .iADR(a),
    .iDATA(din), .oDATA(dout),
    .oTG43(tg43), .oSTEP(step), .oDIRC(dir), .oHLD(hld), .iHRDY(hrdy),
    .iTR00(~tr00_n), .iIP(~index_n), .iWRPT(~wprt_n),
    .oWG(wg_i), .oDRQ(bdi_drq), .oINTRQ(bdi_intrq), .oBUSY(busy), .oRDTRK(rdtrk),
    .iSYNC(sync), .iBYTE_CNT(byte_cnt), .iCRC16_D8(crc16_d8),
    .oRESET_CRC(reset_crc), .oVFOE(vfoe), .oIP_CNT(ip_cnt),
    .iBYTE_2_MAIN(byte_2_main), .iBYTE_2_READ(byte_2_read),
    .oMAIN_2_BYTE(main_2_byte), .oBYTE_2_WRITE(byte_2_write),
    .oTRANSLATE(translate), .iNEXT_BYTE(next_byte),
    .iDRQ_R_DREG(r_drq_r_dreg), .i2RQ_R_SREG(r_intrq_r_sreg));

DPLL U15 (.iCLK(clk_16), .iRDDT(~rawr_n), .oRCLK(rclk), .oRAWR(rawr), .iVFOE(vfoe));

AMD U16 (.iCLK(clk_16), .iRCLK(rclk), .iRAWR(rawr), .iVFOE(vfoe), .iIP_CNT(ip_cnt),
         .o3WORDS(words), .oSTART(start), .oSYNC(sync));

// Read Track: декодер собирает байты от индекса, а не с первой метки, -- как
// WD1793, которая отдаёт и промежутки (кадр до первой метки произвольный,
// на каждой метке подстраивается). Без этого 28 байт от индекса до поля
// адреса пропадали.
MFMDEC U17 (.iCLK(clk_16), .iRCLK(rclk), .iVFOE(vfoe), .iSTART(start | rdtrk), .iSYNC(sync), .iRDTRK(rdtrk),
            .i3WORDS(words), .oBYTE_2_MAIN(byte_2_main), .oBYTE_2_READ(byte_2_read));

CRC16_D8 U19 (.iCLK(clk_16), .iRESET_CRC(reset_crc), .iBYTE_2_MAIN(byte_2_main),
              .iMAIN_2_BYTE(main_2_byte), .iBYTE_2_READ(byte_2_read),
              .iBYTE_2_WRITE(byte_2_write), .oBYTE_CNT(byte_cnt), .oCRC16_D8(crc16_d8));

mfm_wr U20 (.iCLK(clk_16), .iWG(wg_i), .iMAIN_2_BYTE(main_2_byte),
            .iBYTE_2_WRITE(byte_2_write), .iTRANSLATE(translate), .iPC(precomp),
            .oNEXT_BYTE(next_byte), .oWDATA(WDATA), .oWG(wg));

endmodule

`default_nettype wire
