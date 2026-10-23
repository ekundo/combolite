// Строка экрана для ESP: заявка по SPI, копия строки при её выборке, выдача по
// SPI. Скриншот и стрим экрана в браузер.
//
// ОТКУДА СТРОКА. Выборка (vid_fetch.v) и так читает каждую строку картинки из
// зеркала раз в кадр VGA и пишет её байт за байтом в кольцо проигрывателя.
// Здесь эти же записи ловятся для заявленной строки -- с её первого байта, 128
// подряд, -- в свой буфер. Первый байт строки -- тот, перед которым выборка
// писала другую строку: адрес его не годится, слои пишутся в обратном порядке
// (PLANE_XOR у vid_fetch.v). Ни отдельных чтений зеркала, ни порта кольца, ни
// перехода между тактами: выборка живёт в том же такте 50 МГц. Заявка
// исполняется за один кадр VGA, не дольше.
//
// Строка -- в порядке показа: строка картинки y -- это строка памяти
// (прокрутка - y), прокрутку отдаём вместе с ней.
//
// ПРОТОКОЛ (ide_remote.v):
//   0x52 + y        -- заявить строку картинки y (0..255);
//   0x53, дальше 131 байт ответа:
//     [0]       A5h -- строка готова, 00 -- ещё нет (спросить снова);
//     [1]       y готовой строки;
//     [2]       прокрутка кадра, из которого строка;
//     [3..130]  128 байт: слой 0 (8000h) колонки 0..31, слой 1, 2, 3;
//               в байте левая точка -- старший разряд.
`default_nettype none

module scr_buf (
    input  wire       clk,            // 50 МГц
    input  wire       por,
    // от ide_remote
    input  wire       req_stb,
    input  wire [7:0] req_row,
    input  wire       rd_start,       // комбинационные, в такт приёма байта
    input  wire       rd_next,
    output wire [7:0] rd_byte,
    // от выборки (vid_fetch.v)
    input  wire       f_we,
    input  wire [7:0] f_row,          // строка картинки
    input  wire [6:0] f_addr,         // {слой 2, колонка 5}, слой уже машинный
    input  wire [7:0] f_data,
    input  wire [7:0] f_scroll
);

reg [7:0] row = 8'd0, scroll_l = 8'd0;
reg       pend = 1'b0;                // заявка ждёт своей строки
reg       cap  = 1'b0;                // идут её 128 байт
reg       ready = 1'b0;
reg [7:0] n = 8'd0;
reg [7:0] last_row = 8'd0;            // строка прошлой записи выборки

wire      hit = f_we && f_row == row;
wire      mem_we = !req_stb && pend && hit && (cap || last_row != row);
always @(posedge clk) begin
    if (req_stb) begin
        row <= req_row; pend <= 1'b1; cap <= 1'b0; ready <= 1'b0;
    end
    else if (mem_we) begin
        if (!cap) begin cap <= 1'b1; n <= 8'd1; scroll_l <= f_scroll; end
        else if (n == 8'd127) begin pend <= 1'b0; cap <= 1'b0; ready <= 1'b1; end
        else n <= n + 8'd1;
    end
    if (f_we) last_row <= f_row;
    if (por) begin pend <= 1'b0; cap <= 1'b0; ready <= 1'b0; end
end

// выдача: номер байта ответа и чтение памяти тем же тактом
reg  [7:0] idx = 8'd0;
wire [7:0] q;
wire [7:0] idx_n = rd_start ? 8'd0 : (idx == 8'd130) ? idx : idx + 8'd1;
always @(posedge clk)
    if (rd_start | rd_next) idx <= idx_n;
// строка -- один блок EBR (bram.v)
bram_sdp #(.AW(7), .DW(8)) mem (
    .wclk(clk), .we(mem_we), .wa(f_addr), .wd(f_data),
    .rclk(clk), .re(rd_start | rd_next), .ra(idx_n[6:0] - 7'd3), .q(q));

assign rd_byte = (idx == 8'd0) ? (ready ? 8'hA5 : 8'h00)
               : (idx == 8'd1) ? row
               : (idx == 8'd2) ? scroll_l
               :                 q;

endmodule

`default_nettype wire
