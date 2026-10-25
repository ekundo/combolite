// Верхний уровень платы: то, что увидит корпус LCMXO2-7000HE в TQFP-144.
// Логика вся в ядре (top.v), здесь выводы корпуса и ФАПЧ.
//
// Выводы ядра, которые наружу не выходят:
//   - esp_attn -- не используется (top.v, assign esp_attn = 1'b0), ESP
//     опрашивает;
//   - ide_led_n -- светодиодов на плате нет, индикацию обращения показывает
//     морда ESP;
//   - ide_cs1_n -- блок управления ATA программам «Вектора» не виден.
// Выводы для генератора кадра и буфера данных:
//   + addr_cpc, addr_mrc, addr_src, sram_oe_n -- счётчики адреса статики и её
//     /OE, генератор кадра перечитывает зеркало (vid_fetch.v);
//   + buf_dir, buf_oe_n -- буфер данных SN74LVC8T245 между ШД разъёма и
//     стороной памяти.
// Итого 105 сигнальных выводов плюс 5 SSPI -- 110 из 110.
// Генератор кадра включён (VIDEO = 1).
//
// Такты: 50 МГц с генератора, 40 МГц точке VGA и 16 МГц ядру ВГ93 -- с одной
// ФАПЧ (pll_fdc.v).
`default_nettype none

module rev4_top (
    input  wire        clk50,

    // ---------------------------------------------- шина «Вектора», через трансляторы
    input  wire [7:0]  shavv_n,
    inout  wire [7:0]  shd,
    output wire        shd_dir,
    input  wire [7:0]  shap,
    input  wire        ras_n,
    input  wire        cas_n,
    input  wire        chtzu_n,
    input  wire        zpzu_n,
    input  wire        chtvv_n,
    input  wire        zpvv_n,
    input  wire        stack,
    input  wire        sbros_n,
    input  wire        strob,          // СТРОБ СОСТ. = SYNC процессора
    output wire        blk_n,
    output wire        xlat_oe_n,

    // ---------------------------------------------- кнопки машины
    output wire        key_sbr,
    output wire        key_vvod,

    // ---------------------------------------------- статика: квазидиски и зеркало
    output wire [2:0]  sram_page,
    output wire        latch_clk,
    output wire        latch_clk2,
    output wire        sram_ce0_n,
    output wire        sram_ce1_n,
    // адрес от счётчиков 74HC590PW (Nexperia) и /OE статики
    output wire        addr_cpc,
    output wire        addr_mrc,
    output wire        addr_src,
    output wire        sram_oe_n,
    // буфер данных SN74LVC8T245: сторона A -- память, сторона B -- разъём
    output wire        buf_dir,        // 1 -- память -> разъём (A -> B)
    output wire        buf_oe_n,

    // ---------------------------------------------- IDE
    inout  wire [15:0] ide_dd,
    output wire        ide_dd_dir,
    output wire [2:0]  ide_da,
    output wire        ide_cs0_n,
    output wire        ide_dior_n,
    output wire        ide_diow_n,
    output wire        ide_reset_n,

    // ---------------------------------------------- дисковод
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

    // ---------------------------------------------- VGA 3-3-2
    output wire [2:0]  vga_r,
    output wire [2:0]  vga_g,
    output wire [1:0]  vga_b,
    output wire        vga_hs,
    output wire        vga_vs,

    // ---------------------------------------------- звук и ESP
    output wire        audio_l,
    output wire        audio_r,
    input  wire        esp_sck,
    input  wire        esp_mosi,
    input  wire        esp_cs,
    output wire        esp_miso
);

wire clk16, clk40, clkv, pll_lock;
wire cpc_d0, cpc_d1;                   // такт 590-го половинами, на ODDRXE (ниже)
pll_fdc pll (.clk50(clk50), .clk40(clk40), .clkv(clkv), .clk16(clk16), .lock(pll_lock));

top #(
    .IDE_RESET_TICKS (12'd2500),
    .LED_TICKS       (28'd2500000),
    .VIDEO           (1)
) core (
    .clk(clk50),
    .shavv_n(shavv_n), .shap(shap), .shd(shd), .shd_dir(shd_dir),
    .ras_n(ras_n), .cas_n(cas_n),
    .chtzu_n(chtzu_n), .zpzu_n(zpzu_n),
    .chtvv_n(chtvv_n), .zpvv_n(zpvv_n),
    .stack(stack), .sbros_n(sbros_n), .beep(1'b0), .blk_n(blk_n),
    .xlat_oe_n(xlat_oe_n),
    .sram_page(sram_page), .latch_clk(latch_clk),
    .latch_clk2(latch_clk2), .sram_ce0_n(sram_ce0_n), .sram_ce1_n(sram_ce1_n),
    .ide_dd(ide_dd), .ide_dd_dir(ide_dd_dir), .ide_da(ide_da),
    .ide_cs0_n(ide_cs0_n),
    .ide_dior_n(ide_dior_n), .ide_diow_n(ide_diow_n),
    .ide_reset_n(ide_reset_n), .ide_led_n(),
    .ide_cs1_n(), .esp_attn(),
    .audio_l(audio_l), .audio_r(audio_r),
    .esp_sck(esp_sck), .esp_mosi(esp_mosi), .esp_cs(esp_cs),
    .esp_miso(esp_miso),
    .clk_fdc(clk16), .clk_fdc_lock(pll_lock),
    .fdd_ds0_n(fdd_ds0_n), .fdd_ds1_n(fdd_ds1_n), .fdd_moton_n(fdd_moton_n),
    .fdd_dir_n(fdd_dir_n), .fdd_step_n(fdd_step_n), .fdd_wdata_n(fdd_wdata_n),
    .fdd_wgate_n(fdd_wgate_n), .fdd_side1_n(fdd_side1_n),
    .fdd_index_n(fdd_index_n), .fdd_trk00_n(fdd_trk00_n), .fdd_wpt_n(fdd_wpt_n),
    .fdd_rdata_n(fdd_rdata_n), .fdd_rdy_n(fdd_rdy_n),
    .key_vvod(key_vvod), .key_sbr(key_sbr),
    .clk_vga(clkv), .strob(strob),      // 53,333 МГц: 720x900
    .vga_r(vga_r), .vga_g(vga_g), .vga_b(vga_b), .vga_hs(vga_hs), .vga_vs(vga_vs),
    .addr_cpc(), .addr_cpc_d0(cpc_d0), .addr_cpc_d1(cpc_d1),
    .addr_mrc(addr_mrc), .addr_src(addr_src), .sram_oe_n(sram_oe_n),
    .buf_dir(buf_dir), .buf_oe_n(buf_oe_n)
);

// Такт счётчиков адреса статики (74HC590) -- через DDR-регистр вывода: подвод
// идёт импульсами 30/30 нс, полутактами 50 МГц (vid_fetch.v, «ПОДВОД -- ВДВОЕ
// ЧАЩЕ»). D0 -- высокая половина такта, D1 -- низкая; на одинаковых D0 и D1
// вывод ведёт себя как обычный выходной регистр.
ODDRXE cpc_oddr (.SCLK(clk50), .RST(1'b0), .D0(cpc_d0), .D1(cpc_d1), .Q(addr_cpc));

endmodule

`default_nettype wire
