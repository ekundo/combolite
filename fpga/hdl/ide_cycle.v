// Генератор одиночного цикла обращения к регистру IDE.
//
// ЗАЧЕМ. ide.v — прозрачный мост: стробы привода выводятся прямо
// из обращения процессора к порту (`ide_dior_n = ~(block_acc & iord)`), то есть
// их длительность задаёт шина «Вектора». Для доступа со стороны ESP нужен
// источник циклов, не зависящий от процессора, — вот он.
//
// Модуль умеет одно: по команде выполнить ОДНО чтение или запись регистра
// командного блока. Всю последовательность ATA (ожидание BSY, DRDY, DRQ,
// перенос 256 слов) собирает тот, кто им управляет. Так задумано: автомат ATA
// в прошивке ESP пишется и отлаживается несравнимо дешевле, чем в Verilog.
//
// О ВЫБОРЕ ЗАДЕРЖЕК. Числа взяты С ЗАПАСОМ, а не по минимуму спецификации,
// и это сознательно. Для обращения к регистру у стробов ATA есть минимальная
// длительность, но НЕ максимальная: длиннее — всегда безопасно, короче —
// всегда опасно. Поэтому вместо того чтобы полагаться на точность цифр из
// спецификации PIO mode 0, взяты значения заметно больше, а плата за это —
// 700 нс на цикл вместо 600, то есть проценты пропускной способности.
//
// Значения по умолчанию при 50 МГц (такт 20 нс):
//
//   T_SETUP   =  5 тактов = 100 нс   (минимум по PIO 0: t1  = 70 нс)
//   T_STROBE  = 20 тактов = 400 нс   (минимум по PIO 0: t2  = 290 нс)
//   T_HOLD    =  5 тактов = 100 нс   (минимум по PIO 0: t9  = 20 нс)
//   T_RECOVER =  5 тактов = 100 нс
//   итого     = 35 тактов = 700 нс   (минимум по PIO 0: t0  = 600 нс)
//
// Если привод окажется быстрым и захочется ускориться — уменьшать T_STROBE,
// но не ниже 15 тактов (300 нс), и проверять в симуляции на модели привода,
// которая следит за соблюдением минимумов.

`default_nettype none

module ide_cycle #(
    parameter T_SETUP   = 5,
    parameter T_STROBE  = 20,
    parameter T_HOLD    = 5,
    parameter T_RECOVER = 5
) (
    input  wire        clk,
    input  wire        reset,          // активный высокий

    // команда
    input  wire        start,          // запрос, достаточно одного такта
    input  wire        wr,             // 1 = запись, 0 = чтение
    input  wire [2:0]  addr,           // номер регистра командного блока
    input  wire [15:0] wdata,          // что записать
    output reg  [15:0] rdata,          // что прочитано, держится до следующего цикла
    output reg         busy,           // цикл идёт
    output reg         done,           // один такт по завершении

    // привод
    output reg  [2:0]  ide_da,
    output reg         ide_cs0_n,
    output reg         ide_dior_n,
    output reg         ide_diow_n,
    output reg  [15:0] ide_dout,
    output reg         ide_dout_oe,    // 1 = мы ведём шину данных привода
    input  wire [15:0] ide_din
);

localparam S_IDLE = 3'd0, S_SETUP = 3'd1, S_STROBE = 3'd2,
           S_HOLD = 3'd3, S_RECOVER = 3'd4;

reg [2:0] state;
reg [7:0] cnt;
reg       cyc_wr;

always @(posedge clk) begin
    done <= 1'b0;

    if (reset) begin
        state       <= S_IDLE;
        cnt         <= 8'd0;
        busy        <= 1'b0;
        cyc_wr      <= 1'b0;
        rdata       <= 16'h0000;
        ide_da      <= 3'd0;
        ide_cs0_n   <= 1'b1;
        ide_dior_n  <= 1'b1;
        ide_diow_n  <= 1'b1;
        ide_dout    <= 16'h0000;
        ide_dout_oe <= 1'b0;
    end
    else case (state)

    S_IDLE:
        if (start) begin
            busy      <= 1'b1;
            cyc_wr    <= wr;
            ide_da    <= addr;
            ide_cs0_n <= 1'b0;          // адрес и выбор — раньше строба
            if (wr) begin
                ide_dout    <= wdata;   // данные тоже раньше строба
                ide_dout_oe <= 1'b1;
            end
            cnt   <= T_SETUP[7:0] - 8'd1;
            state <= S_SETUP;
        end

    S_SETUP:
        if (cnt != 0) cnt <= cnt - 8'd1;
        else begin
            if (cyc_wr) ide_diow_n <= 1'b0;
            else        ide_dior_n <= 1'b0;
            cnt   <= T_STROBE[7:0] - 8'd1;
            state <= S_STROBE;
        end

    S_STROBE:
        if (cnt != 0) begin
            cnt <= cnt - 8'd1;
            // Данные привода забираем на ПОСЛЕДНЕМ такте строба: к этому
            // времени они стоят почти всю его длительность, и мы не гонимся
            // со снятием строба.
            if (cnt == 8'd1 && !cyc_wr) rdata <= ide_din;
        end
        else begin
            ide_dior_n <= 1'b1;
            ide_diow_n <= 1'b1;
            cnt   <= T_HOLD[7:0] - 8'd1;
            state <= S_HOLD;
        end

    S_HOLD:                             // адрес, выбор и данные ещё держатся
        if (cnt != 0) cnt <= cnt - 8'd1;
        else begin
            ide_cs0_n   <= 1'b1;
            ide_dout_oe <= 1'b0;
            cnt   <= T_RECOVER[7:0] - 8'd1;
            state <= S_RECOVER;
        end

    S_RECOVER:                          // добираем полное время цикла
        if (cnt != 0) cnt <= cnt - 8'd1;
        else begin
            busy  <= 1'b0;
            done  <= 1'b1;
            state <= S_IDLE;
        end

    default: state <= S_IDLE;
    endcase
end

endmodule

`default_nettype wire
