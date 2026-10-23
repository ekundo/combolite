// Ядро звукового синтезатора YM2149 (совместим с AY-3-8910).
//
// ПЕРЕВОД С VHDL. Исходник — svofski/vector06c-lesshadoks,
// quartus/src/ay/ym2149.vhd (автор и лицензия файла не указаны). Перевод сделан
// ради проверяемости: Diamond смешанный проект собрал бы, но Icarus VHDL не
// берёт вовсе.
//
// Перевод буквальный. Там, где оригинал вызывает сомнения, поведение сохранено,
// а сомнение помечено словом ОСОБЕННОСТЬ — менять молча хуже, чем оставить.
//
// Единственное отличие от оригинала: `env_reset` заведён в цепь сброса. В VHDL
// он не сбрасывался вовсе и до первой записи имел неопределённое значение, что
// в симуляции даёт X по всей огибающей.

`default_nettype none

module ym2149 #(
    // 0 -- без регистров портов ввода-вывода R14/R15 (у «Вектора» к выводам
    // портов AY ничего не подключено; читаются FF, как вход с подтяжкой)
    parameter HAS_PORTS = 1
) (
    input  wire       CLK,
    input  wire       ENA,      // разрешение: одно на каждый такт синтезатора
    input  wire       RESET,
    input  wire       BDIR,     // шина AY: направление
    input  wire       CS,
    input  wire       BC,       // 1 — защёлка адреса, 0 — данные
    input  wire [7:0] DI,
    output wire [7:0] DO,
    output reg  [7:0] OUT_A,
    output reg  [7:0] OUT_B,
    output reg  [7:0] OUT_C
);

// ---------------------------------------------------- таблица громкостей
// 32 значения, шаг примерно 1,5 дБ. Это таблица из оригинала, значение в
// значение; она НЕ линейная и не логарифмическая точно — такая в микросхеме.
function [7:0] vol_table;
    input [4:0] v;
    case (v)
        5'd0:  vol_table = 8'h00;   5'd1:  vol_table = 8'h01;
        5'd2:  vol_table = 8'h02;   5'd3:  vol_table = 8'h02;
        5'd4:  vol_table = 8'h03;   5'd5:  vol_table = 8'h03;
        5'd6:  vol_table = 8'h04;   5'd7:  vol_table = 8'h05;
        5'd8:  vol_table = 8'h06;   5'd9:  vol_table = 8'h07;
        5'd10: vol_table = 8'h08;   5'd11: vol_table = 8'h09;
        5'd12: vol_table = 8'h0B;   5'd13: vol_table = 8'h0D;
        5'd14: vol_table = 8'h10;   5'd15: vol_table = 8'h13;
        5'd16: vol_table = 8'h16;   5'd17: vol_table = 8'h1A;
        5'd18: vol_table = 8'h1F;   5'd19: vol_table = 8'h25;
        5'd20: vol_table = 8'h2C;   5'd21: vol_table = 8'h34;
        5'd22: vol_table = 8'h3D;   5'd23: vol_table = 8'h48;
        5'd24: vol_table = 8'h54;   5'd25: vol_table = 8'h63;
        5'd26: vol_table = 8'h74;   5'd27: vol_table = 8'h88;
        5'd28: vol_table = 8'h9F;   5'd29: vol_table = 8'hBA;
        5'd30: vol_table = 8'hD9;   5'd31: vol_table = 8'hFF;
        default: vol_table = 8'h00;
    endcase
endfunction

// ------------------------------------------------------------ регистры
reg [11:0] freq_a, freq_b, freq_c;
reg  [4:0] noise;
reg  [7:0] settings;
reg  [4:0] level_a, level_b, level_c;
reg [15:0] envelope;
reg  [3:0] shape;
reg  [7:0] port_a, port_b;
reg  [3:0] addr;
reg        env_reset;

always @(posedge CLK or posedge RESET) begin
    if (RESET) begin
        freq_a <= 12'd0; freq_b <= 12'd0; freq_c <= 12'd0;
        noise <= 5'd0;
        settings <= 8'hFF;
        level_a <= 5'd0; level_b <= 5'd0; level_c <= 5'd0;
        envelope <= 16'd0;
        shape <= 4'd0;
        port_a <= 8'd0; port_b <= 8'd0;
        addr <= 4'd0;
        env_reset <= 1'b0;      // отличие от оригинала, см. шапку
    end
    else if (CS && BDIR) begin
        if (BC)
            addr <= DI[3:0];
        else begin
            // ОСОБЕННОСТЬ. env_reset снимается ЛЮБОЙ записью данных и ставится
            // записью в регистр 13. Сбросом микросхемы он в оригинале не
            // снимался. Значит после записи формы огибающая держится в
            // начальном состоянии до следующей записи в любой регистр — на
            // практике программы туда что-нибудь пишут сразу, поэтому это не
            // всплывает. Поведение сохранено как в оригинале.
            env_reset <= 1'b0;
            case (addr)
                4'h0: freq_a[7:0]    <= DI;
                4'h1: freq_a[11:8]   <= DI[3:0];
                4'h2: freq_b[7:0]    <= DI;
                4'h3: freq_b[11:8]   <= DI[3:0];
                4'h4: freq_c[7:0]    <= DI;
                4'h5: freq_c[11:8]   <= DI[3:0];
                4'h6: noise          <= DI[4:0];
                4'h7: settings       <= DI;
                4'h8: level_a        <= DI[4:0];
                4'h9: level_b        <= DI[4:0];
                4'hA: level_c        <= DI[4:0];
                4'hB: envelope[7:0]  <= DI;
                4'hC: envelope[15:8] <= DI;
                4'hD: begin shape <= DI[3:0]; env_reset <= 1'b1; end
                4'hE: if (HAS_PORTS) port_a <= DI;
                4'hF: if (HAS_PORTS) port_b <= DI;
            endcase
        end
    end
end

// ------------------------------------------------------------ чтение
// Регистры 14 и 15 читаются только когда соответствующий порт настроен на
// ввод; иначе, как и у всех неописанных случаев, читается 0xFF.
assign DO = (addr == 4'h0) ? freq_a[7:0]
          : (addr == 4'h1) ? {4'b0000, freq_a[11:8]}
          : (addr == 4'h2) ? freq_b[7:0]
          : (addr == 4'h3) ? {4'b0000, freq_b[11:8]}
          : (addr == 4'h4) ? freq_c[7:0]
          : (addr == 4'h5) ? {4'b0000, freq_c[11:8]}
          : (addr == 4'h6) ? {3'b000, noise}
          : (addr == 4'h7) ? settings
          : (addr == 4'h8) ? {3'b000, level_a}
          : (addr == 4'h9) ? {3'b000, level_b}
          : (addr == 4'hA) ? {3'b000, level_c}
          : (addr == 4'hB) ? envelope[7:0]
          : (addr == 4'hC) ? envelope[15:8]
          : (addr == 4'hD) ? {4'b0000, shape}
          : (HAS_PORTS && addr == 4'hE && settings[6]) ? port_a
          : (HAS_PORTS && addr == 4'hF && settings[7]) ? port_b
          : 8'hFF;

// ------------------------------------------------------------ делитель
// Тональные счётчики тикают раз в 8 разрешений, шумовой — раз в 16.
// НАЧАЛЬНЫЕ ЗНАЧЕНИЯ ОБЯЗАТЕЛЬНЫ. В VHDL они заданы при объявлении
// (`cnt_div : unsigned := (others => '0')` и так далее), и на них всё держится:
// без них cnt_div стартует неопределённым, условие cnt_div == 0 не выполняется
// никогда, делитель не тикает и тона нет вовсе.
// Для Lattice это ещё и значение после конфигурации, то есть не только про
// симуляцию.
reg [2:0] cnt_div = 3'd0;
reg       noise_div = 1'b0;
reg       ena_div = 1'b0, ena_div_noise = 1'b0;

always @(posedge CLK) begin
    if (ENA) begin
        ena_div       <= 1'b0;
        ena_div_noise <= 1'b0;
        if (cnt_div == 3'd0) begin
            cnt_div   <= 3'd7;
            ena_div   <= 1'b1;
            noise_div <= ~noise_div;
            // Читается СТАРОЕ значение noise_div — так же, как в VHDL: там
            // сигнал в том же такте ещё не обновлён, здесь неблокирующее
            // присваивание вступит в силу только в конце такта.
            if (noise_div)
                ena_div_noise <= 1'b1;
        end
        else
            cnt_div <= cnt_div - 3'd1;
    end
end

// -------------------------------------------------------- генератор шума
// Сдвиговый регистр на 17 разрядов, отводы 0 и 2. Слагаемое poly17_zero
// выводит регистр из нулевого состояния, в котором он иначе залипнет навсегда.
reg [16:0] poly17 = 17'd0;
reg  [4:0] noise_gen_cnt = 5'd0;
wire [4:0] noise_gen_comp = (noise == 5'd0) ? 5'd0 : noise - 5'd1;
wire       poly17_zero    = (poly17 == 17'd0);

always @(posedge CLK) begin
    if (ENA && ena_div_noise) begin
        if (noise_gen_cnt >= noise_gen_comp) begin
            noise_gen_cnt <= 5'd0;
            poly17 <= {poly17[0] ^ poly17[2] ^ poly17_zero, poly17[16:1]};
        end
        else
            noise_gen_cnt <= noise_gen_cnt + 5'd1;
    end
end

wire noise_gen_op = poly17[0];

// ---------------------------------------------------- тональные генераторы
// Период 0 равен периоду 1 — так в микросхеме.
reg [11:0] tone_cnt_a = 12'd0, tone_cnt_b = 12'd0, tone_cnt_c = 12'd0;
reg        tone_op_a = 1'b0, tone_op_b = 1'b0, tone_op_c = 1'b0;
wire [11:0] comp_a = (freq_a == 12'd0) ? 12'd0 : freq_a - 12'd1;
wire [11:0] comp_b = (freq_b == 12'd0) ? 12'd0 : freq_b - 12'd1;
wire [11:0] comp_c = (freq_c == 12'd0) ? 12'd0 : freq_c - 12'd1;

always @(posedge CLK) begin
    if (ENA && ena_div) begin
        if (tone_cnt_a >= comp_a) begin tone_cnt_a <= 12'd0; tone_op_a <= ~tone_op_a; end
        else                            tone_cnt_a <= tone_cnt_a + 12'd1;
        if (tone_cnt_b >= comp_b) begin tone_cnt_b <= 12'd0; tone_op_b <= ~tone_op_b; end
        else                            tone_cnt_b <= tone_cnt_b + 12'd1;
        if (tone_cnt_c >= comp_c) begin tone_cnt_c <= 12'd0; tone_op_c <= ~tone_op_c; end
        else                            tone_cnt_c <= tone_cnt_c + 12'd1;
    end
end

// -------------------------------------------------- делитель огибающей
reg [15:0] env_gen_cnt = 16'd0;
reg        env_ena = 1'b0;
wire [15:0] env_comp = (envelope == 16'd0) ? 16'd0 : envelope - 16'd1;

always @(posedge CLK) begin
    if (ENA) begin
        env_ena <= 1'b0;
        if (ena_div) begin
            if (env_gen_cnt >= env_comp) begin
                env_gen_cnt <= 16'd0;
                env_ena <= 1'b1;
            end
            else
                env_gen_cnt <= env_gen_cnt + 16'd1;
        end
    end
end

// ------------------------------------------------------------ огибающая
// shape[3] — продолжение, shape[2] — атака, shape[1] — чередование,
// shape[0] — удержание. Логика ниже повторяет оригинал ветка в ветку.
reg [4:0] env_vol = 5'd0;
reg       env_inc = 1'b0, env_hold = 1'b0;

// ЗАГРУЗКА СИНХРОННАЯ, в отличие от оригинала, и это не вкусовщина.
// В VHDL env_reset стоял в списке асинхронного сброса, а загружаемое им значение
// зависит от shape, то есть непостоянно. Синтез такое не берёт: асинхронный вход
// триггера загружает только константу. Yosys прямо и сказал: «Async reset
// env_reset yields non-constant value 5'mmmmm for signal env_vol». Оригинал
// собирался в Quartus, который это проглатывал.
//
// Поведение сохраняется: env_reset ставится тактируемым процессом и держится до
// следующей записи данных, а огибающая двигается только по ENA и env_ena —
// значит задержка загрузки на один такт ни на что не влияет. Зато уходит
// асинхронный сброс, управляемый обычной логикой.
always @(posedge CLK) begin
    if (env_reset) begin
        // shape[2] — бит атаки: ноль означает спад, и тогда начинаем сверху.
        env_vol  <= shape[2] ? 5'b00000 : 5'b11111;
        env_inc  <= shape[2];
        env_hold <= 1'b0;
    end
    else if (ENA && env_ena) begin
        if (!env_hold)
            env_vol <= env_inc ? (env_vol + 5'd1) : (env_vol + 5'd31);
        if (!shape[3]) begin
            if (!env_inc) begin
                if (env_vol == 5'b00001) env_hold <= 1'b1;
            end
            else begin
                if (env_vol == 5'b11111) env_hold <= 1'b1;
            end
        end
        else if (shape[0]) begin
            if (!env_inc) begin
                if (shape[1]) begin
                    if (env_vol == 5'b00000) env_hold <= 1'b1;
                end
                else begin
                    if (env_vol == 5'b00001) env_hold <= 1'b1;
                end
            end
            else begin
                if (shape[1]) begin
                    if (env_vol == 5'b11111) env_hold <= 1'b1;
                end
                else begin
                    if (env_vol == 5'b11110) env_hold <= 1'b1;
                end
            end
        end
        else if (shape[1]) begin
            if (!env_inc) begin
                if (env_vol == 5'b00001) env_hold <= 1'b1;
                if (env_vol == 5'b00000) begin env_hold <= 1'b0; env_inc <= 1'b1; end
            end
            else begin
                if (env_vol == 5'b11110) env_hold <= 1'b1;
                if (env_vol == 5'b11111) begin env_hold <= 1'b0; env_inc <= 1'b0; end
            end
        end
    end
end

// ------------------------------------------------------------ смеситель
// settings[2:0] — запрет тона по каналам, settings[5:3] — запрет шума.
// Единица ЗАПРЕЩАЕТ, поэтому «или»: канал звучит, когда разрешённый источник
// даёт единицу. level[4] выбирает между постоянной громкостью и огибающей.
always @(posedge CLK or posedge RESET) begin
    if (RESET) begin
        OUT_A <= 8'd0; OUT_B <= 8'd0; OUT_C <= 8'd0;
    end
    else if (ENA) begin
        if (((settings[0] | tone_op_a) & (settings[3] | noise_gen_op)) == 1'b0)
            OUT_A <= 8'd0;
        else if (!level_a[4])
            OUT_A <= vol_table({level_a[3:0], 1'b1});
        else
            OUT_A <= vol_table(env_vol);

        if (((settings[1] | tone_op_b) & (settings[4] | noise_gen_op)) == 1'b0)
            OUT_B <= 8'd0;
        else if (!level_b[4])
            OUT_B <= vol_table({level_b[3:0], 1'b1});
        else
            OUT_B <= vol_table(env_vol);

        if (((settings[2] | tone_op_c) & (settings[5] | noise_gen_op)) == 1'b0)
            OUT_C <= 8'd0;
        else if (!level_c[4])
            OUT_C <= vol_table({level_c[3:0], 1'b1});
        else
            OUT_C <= vol_table(env_vol);
    end
end

endmodule

`default_nettype wire
