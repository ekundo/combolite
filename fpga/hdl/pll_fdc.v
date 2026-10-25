// ФАПЧ платы: 50 МГц -> 16 МГц ядру ВГ93 и 53,333 МГц под VGA.
//
// Точка VGA -- CLKOS2 = 480 / 9 = 53,333 МГц: половина VESA DMT 1440x900@60
// (106,5 / 2 = 53,25, на 0,16 % быстрее: точнее из генератора 480, который
// обязан дать ровно 16 МГц, не выходит). CLKOP 40 МГц остаётся обратной связью
// и больше никуда не идёт.
//
// ЗАЧЕМ. Ядру ВГ93 нужно ровно 16 МГц (запас по частоте четыре процента,
// замерено), а из 50 нацело не выходит. С `CORE_CLK_DIV = 0` (fdc_core.v) ядро
// берёт 16 МГц прямо отсюда, без деления триггером -- порождённого такта на 318
// триггеров нет. 40 МГц -- точка VGA 800x600@60: одна ФАПЧ на всё.
//
// ЧИСЛА. Частота сравнения 50 / CLKI_DIV = 10 МГц; обратная связь по CLKOP:
// CLKOP = 10 x CLKFB_DIV = 40 МГц; генератор 40 x CLKOP_DIV = 480 МГц (окно
// MachXO2 -- 400..800); CLKOS = 480 / 30 = 16,000 МГц ровно. Опорная частота
// приходит первичной сетью с вывода 5 -- TN1199 это разрешает.
//
// ПЕТЛЯ -- ПО IPexpress. Ток насоса и резистор фильтра (ICP_CURRENT,
// LPF_RESISTOR) считает IPexpress под частоты, и стоят они атрибутами синтеза,
// а не параметрами. Без них строчная VGA шла с разбросом фронта около 0,4 мкс
// за строку при чистом генераторе G1: монитор терял захват, телевизор
// показывал рваные строки. Значения 7 и 8 -- от IPexpress Diamond 3.5
// (PLL 5.8) на CLKI 50, CLKOP 40, CLKOS 16,
// обратная связь CLKOP, с выходом LOCK; делители и фазы там те же, остальные
// параметры равны умолчаниям примитива.
//
// В симуляции (`SIM`) -- поведенческая модель: две частоты от своих генераторов.
`default_nettype none

module pll_fdc (
    input  wire clk50,
    output wire clk40,              // обратная связь ФАПЧ, наружу не нужен
    output wire clkv,               // точка VGA, 53,333 МГц
    output wire clk16,              // ядро ВГ93
    output wire lock
);

`ifdef SIM
reg r40 = 1'b0, r16 = 1'b0, rv = 1'b0;
always #12.5  r40 = ~r40;
always #31.25 r16 = ~r16;
always #9.375 rv  = ~rv;
assign clk40 = r40;
assign clk16 = r16;
assign clkv  = rv;
assign lock  = 1'b1;
`else
wire clkop;
// FREQUENCY_PIN_* -- для Map: без них он берёт вход за 100 МГц и меряет домен
// clk50 по 100. Только атрибутами синтеза после
// экземпляра: параметрами Synplify из Diamond 3.5 их не знает вовсе («CG596
// Parameter FREQUENCY_PIN_CLKOS / CLKOP cannot be found in module EHXPLLJ»).
EHXPLLJ #(
    .CLKI_DIV(5), .CLKFB_DIV(4), .CLKOP_DIV(12), .CLKOS_DIV(30),
    .CLKOS2_DIV(9), .CLKOS3_DIV(1),
    .CLKOP_ENABLE("ENABLED"), .CLKOS_ENABLE("ENABLED"),
    .CLKOS2_ENABLE("ENABLED"), .CLKOS3_ENABLE("DISABLED"),
    .CLKOP_CPHASE(11), .CLKOS_CPHASE(29), .CLKOP_FPHASE(0), .CLKOS_FPHASE(0),
    .CLKOS2_CPHASE(8), .CLKOS2_FPHASE(0),
    .FEEDBK_PATH("CLKOP"), .FRACN_ENABLE("DISABLED"),
    .PLL_LOCK_MODE(0), .STDBY_ENABLE("DISABLED"), .DPHASE_SOURCE("DISABLED"),
    .PLLRST_ENA("DISABLED"), .MRST_ENA("DISABLED"), .DCRST_ENA("DISABLED"),
    .DDRST_ENA("DISABLED"), .INTFB_WAKE("DISABLED")
) pll (
    .CLKI(clk50), .CLKFB(clkop),
    .PHASESEL1(1'b0), .PHASESEL0(1'b0), .PHASEDIR(1'b0), .PHASESTEP(1'b0),
    .LOADREG(1'b0), .STDBY(1'b0), .PLLWAKESYNC(1'b0),
    .RST(1'b0), .RESETM(1'b0), .RESETC(1'b0), .RESETD(1'b0),
    .ENCLKOP(1'b0), .ENCLKOS(1'b0), .ENCLKOS2(1'b0), .ENCLKOS3(1'b0),
    .PLLCLK(1'b0), .PLLRST(1'b0), .PLLSTB(1'b0), .PLLWE(1'b0),
    .PLLDATI7(1'b0), .PLLDATI6(1'b0), .PLLDATI5(1'b0), .PLLDATI4(1'b0),
    .PLLDATI3(1'b0), .PLLDATI2(1'b0), .PLLDATI1(1'b0), .PLLDATI0(1'b0),
    .PLLADDR4(1'b0), .PLLADDR3(1'b0), .PLLADDR2(1'b0), .PLLADDR1(1'b0), .PLLADDR0(1'b0),
    .CLKOP(clkop), .CLKOS(clk16), .CLKOS2(clkv), .CLKOS3(),
    .LOCK(lock), .INTLOCK(), .REFCLK(),
    .PLLDATO7(), .PLLDATO6(), .PLLDATO5(), .PLLDATO4(),
    .PLLDATO3(), .PLLDATO2(), .PLLDATO1(), .PLLDATO0(),
    .PLLACK(), .DPHSRC(), .CLKINTFB()
)
/* synthesis FREQUENCY_PIN_CLKI="50.000000" */
/* synthesis FREQUENCY_PIN_CLKOP="40.000000" */
/* synthesis FREQUENCY_PIN_CLKOS="16.000000" */
/* synthesis FREQUENCY_PIN_CLKOS2="53.333333" */
/* synthesis ICP_CURRENT="7" */
/* synthesis LPF_RESISTOR="8" */;
assign clk40 = clkop;
`endif

endmodule

`default_nettype wire
