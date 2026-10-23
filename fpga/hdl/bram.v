// Блочная память MachXO2 в форме, которую оба синтезатора кладут в EBR как есть.
//
// ЗАЧЕМ ОТДЕЛЬНО. yosys клеит память по точной глубине, а Synplify округляет
// глубину до степени двойки и выбирает узкий режим блока: журнал палитры
// vid_rec 4992 x 18 становился 18 блоками 8192 x 1, снимки 2808 x 18 --
// восемью 4096 x 2, и на них уходили все 26 блоков кристалла. Строчный буфер,
// буферы сектора и загрузочное ПЗУ ехали в LUT, и Map насчитывал 175 %. Поэтому крупная память режется на банки,
// каждый ровно в один блок в его родном режиме, а выбор банка делается снаружи.
//
// bram_sdp -- один порт пишет, другой читает, такты разные (DP8KC/PDPW8KC);
// bram_tdp -- два порта, оба пишут и читают, такт один (DP8KC). Чтение с
// триггера, на такт позже адреса; при записи в тот же адрес порт отдаёт
// прежнее содержимое (read-before-write).
`default_nettype none

module bram_sdp #(
    parameter AW = 10,
    parameter DW = 9
) (
    input  wire          wclk,
    input  wire          we,
    input  wire [AW-1:0] wa,
    input  wire [DW-1:0] wd,
    input  wire          rclk,
    input  wire          re,          // чтение: без него q держится
    input  wire [AW-1:0] ra,
    output reg  [DW-1:0] q
);
(* syn_ramstyle = "block_ram" *) reg [DW-1:0] m [0:(1 << AW) - 1];
always @(posedge wclk) if (we) m[wa] <= wd;
always @(posedge rclk) if (re) q <= m[ra];
endmodule

module bram_tdp #(
    parameter AW = 10,
    parameter DW = 9
) (
    input  wire          clk,
    input  wire          we_a,
    input  wire [AW-1:0] a_a,
    input  wire [DW-1:0] d_a,
    output reg  [DW-1:0] q_a,
    input  wire          we_b,
    input  wire [AW-1:0] a_b,
    input  wire [DW-1:0] d_b,
    output reg  [DW-1:0] q_b
);
(* syn_ramstyle = "block_ram" *) reg [DW-1:0] m [0:(1 << AW) - 1];
always @(posedge clk) begin
    if (we_a) m[a_a] <= d_a;
    q_a <= m[a_a];
end
always @(posedge clk) begin
    if (we_b) m[a_b] <= d_b;
    q_b <= m[a_b];
end
endmodule

`default_nettype wire
