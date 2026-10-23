// Кодер MFM и запись -- наш, вместо vendor/fdc1793/MFMCDR.v: с настоящей
// предкомпенсацией записи.
//
// ЗАЧЕМ. У MFMCDR «предкомпенсация» -- растяжка ИНТЕРВАЛА на такт (62,5 нс) по
// шаблонам WD1793 (x110/0001 -- раньше, x011/1000 -- позже). Растянутый
// интервал сдвигает не один импульс, а все следующие, сдвиги копятся по ходу
// записи, а величина -- ровно такт, без настройки. Настоящая предкомпенсация
// сдвигает только сам импульс, сетка ячеек остаётся ровной. Так у кишинёвской
// платы (база P-CAD M.PCB, 1995): импульс WD грузит сдвиговый регистр К555ИР16
// в одну из трёх позиций -- по LATE, по «ни то ни другое», по EARLY, -- и он
// выходит на шаг сдвига раньше или позже. TG43 там не подключён -- сдвиг на
// всех дорожках. Пятидюймовым на внутренних дорожках это нужно: соседние
// переходы на носителе расталкиваются, и при чтении пик уезжает к дальнему
// соседу -- его заранее пишут ближе к ближнему.
//
// БАЙТОВАЯ ЧАСТЬ -- строка в строку как в MFMCDR: те же счётчики, тот же обмен
// с автоматом ядра (NEXT_BYTE в тот же такт), та же подстановка пропущенного
// такта для A1/C2 (iTRANSLATE). Убрана только растяжка интервала: интервал
// всегда TWO_mks + 1 тактов, ровно 2 мкс.
//
// ВЫДАЧА. Ячейки MFM (2 мкс, есть переход или нет) идут в окно из пяти: две до,
// текущая, две после. Текущая -- выдаётся с опозданием на две ячейки (4 мкс),
// чтобы видеть будущих соседей. Правило -- по расстоянию до соседей (минимум у
// MFM -- две ячейки):
//   сосед за две ячейки ДО, после -- дальше  -> импульс РАНЬШЕ на iPC тактов;
//   сосед за две ячейки ПОСЛЕ, до -- дальше  -> импульс ПОЗЖЕ на iPC тактов;
//   иначе -- в срок.
// Это то же, что шаблоны WD1793 по битам данных, только по самим ячейкам, с
// пропущенными тактами меток. WG наружу (oWG) запаздывает на те же две ячейки:
// данные и строб записи остаются согласованы. iPC = 0 -- без сдвига.
//
// ПОСЛЕДНИЙ БАЙТ -- ЦЕЛИКОМ. Автомат ядра снимает WG по запросу следующего
// байта, а запрос приходит, когда у текущего ещё не выдан бит 0 (две ячейки):
// у MFMCDR последний байт -- FF за CRC сектора -- выходил без последнего бита.
// Паспорт (с. 11): «followed by one byte of logic ones ... The WG output is then
// deactivated». Здесь снятый iWG дописывает начатый байт до конца (fin).
`default_nettype none

module mfm_wr (
    input  wire       iCLK,           // 16 МГц
    input  wire       iWG,            // WRITE GATE от автомата ядра
    input  wire [7:0] iMAIN_2_BYTE,   // следующий байт
    input  wire       iBYTE_2_WRITE,  // следующий байт подан
    input  wire       iTRANSLATE,     // пропуск такта (C2, A1)
    input  wire [3:0] iPC,            // предкомпенсация, тактов 16 МГц (62,5 нс): 0..8
    output reg        oNEXT_BYTE = 1'b0,
    output reg        oWDATA = 1'b0,  // импульс записи, активен единицей
    output wire       oWG             // WRITE GATE наружу, вровень с данными
);

localparam [5:0] TWO_mks = 6'd31,     // 2-мкс интервал (32 такта 16 МГц)
                 HLF_mks = 6'd8,      // 500 нс -- ширина импульса
                 NXT_byt = HLF_mks + 6'd2;

// ---------------------------------------------- байтовая часть (как MFMCDR)
reg [2:0] rBIT_CNT     = 3'd7;
reg [1:0] rMFM_BIT     = 2'b00;
reg [5:0] rWDATA_CNT   = TWO_mks;
reg       rMFM_CNT     = 1'b0;
reg       rLAST        = 1'b0;        // предыдущий бит данных (rLASTBITS[0] у MFMCDR)
reg [7:0] rMAIN_2_BYTE = 8'h4E;
// Выбитый такт: у A1 -- перед разрядом 2 (4489), у C2 -- перед разрядом 3
// (5224, паспорт: «missing clock transition between bits 3 and 4», счёт от
// старшего). У MFMCDR оба -- перед разрядом 2, и индексная метка C2 выходила
// 5284: её не узнавал ни наш детектор (ищет 5224), ни MB8877.
wire      wMFM_MSK = ~(iTRANSLATE &&
                       (((rBIT_CNT == 3'd2) && (rMAIN_2_BYTE == 8'hA1)) ||
                        ((rBIT_CNT == 3'd3) && (rMAIN_2_BYTE == 8'hC2))));

// fin: байт начат при взведённом WG и ещё не дописан. Конец байта -- перенос
// интервала после ячейки данных бита 0 (rMFM_CNT = 1, rBIT_CNT уже 7).
reg  fin = 1'b0;
wire wg_e = iWG | fin;                // WG для кодера: держится до конца байта
always @(posedge iCLK)
    if (iWG) fin <= 1'b1;
    else if (fin && (rWDATA_CNT == TWO_mks) && rMFM_CNT && (rBIT_CNT == 3'd7)) fin <= 1'b0;

always @(posedge iCLK)
    if (!wg_e) begin
        rBIT_CNT     <= 3'd7;
        rWDATA_CNT   <= TWO_mks;
        rMFM_BIT     <= 2'b10;
        rMFM_CNT     <= 1'b0;
        rMAIN_2_BYTE <= iMAIN_2_BYTE;
    end else if (rWDATA_CNT < TWO_mks)
        rWDATA_CNT <= rWDATA_CNT + 1'b1;
    else begin
        rWDATA_CNT <= 6'd0;
        rMFM_CNT   <= ~rMFM_CNT;
        if (rMFM_CNT) begin
            rBIT_CNT <= rBIT_CNT - 1'b1;
            if (rBIT_CNT == 3'd0) rMAIN_2_BYTE <= iMAIN_2_BYTE;
            rLAST <= rMAIN_2_BYTE[rBIT_CNT];
            case ({rMAIN_2_BYTE[rBIT_CNT], rLAST})
                2'b00:   rMFM_BIT <= {wMFM_MSK, 1'b0};
                2'b01:   rMFM_BIT <= 2'b00;
                default: rMFM_BIT <= 2'b01;
            endcase
        end
    end

always @(posedge iCLK)
    if (iBYTE_2_WRITE || !iWG)
        oNEXT_BYTE <= 1'b0;
    else if ((rBIT_CNT == 3'd0) && (rWDATA_CNT == NXT_byt) && rMFM_CNT)
        oNEXT_BYTE <= 1'b1;

// ------------------------------------------------- окно ячеек и выдача
// t -- фаза интервала, вровень с rWDATA_CNT, пока iWG; после спада iWG идёт
// дальше сама, пока не выйдут две последние ячейки.
reg  [5:0] t    = 6'd0;
reg        wg_d = 1'b0;
reg  [4:0] sh   = 5'd0;               // [0] -- через две ячейки, [2] -- текущая, [4] -- две до
reg  [2:0] wgsh = 3'd0;               // iWG по тем же ячейкам
reg  [3:0] pc   = 4'd0;
wire       run  = wg_e | (|wgsh);
wire       c_now = wg_e & rMFM_BIT[~rMFM_CNT];   // ячейка этого интервала

always @(posedge iCLK) begin
    wg_d <= iWG;
    if (iWG & ~wg_d)      t <= 6'd0;   // вровень с первым переносом rWDATA_CNT
    else if (run)         t <= (t == TWO_mks) ? 6'd0 : t + 1'b1;
    if (run && t == 6'd0) begin        // ячейка этого интервала уже выставлена
        sh   <= {sh[3:0], c_now};
        wgsh <= {wgsh[1:0], wg_e};
        pc   <= (iPC > 4'd8) ? 4'd8 : iPC;
    end
end

wire       early = sh[4] & ~sh[0];
wire       late  = ~sh[4] & sh[0];
wire [5:0] start = 6'd1 + {2'b00, pc} + (late ? {2'b00, pc} : 6'd0) - (early ? {2'b00, pc} : 6'd0);

assign oWG = wgsh[2];

always @(posedge iCLK)
    oWDATA <= wgsh[2] & sh[2] & (t >= start) & (t < start + HLF_mks);

endmodule

`default_nettype wire
