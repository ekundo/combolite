// IanPo/zx-pk.ru, 2016
// Модуль восстановителя данных (ФАПЧ) для HDL-модели КР1818ВГ93/WD1793
// Основан на схеме от Andromeda Systems из документа WD Corp. FD179X Application Notes Fig.12
//
`default_nettype wire
//
module DPLL (
input				iCLK,
input				iRDDT,
output reg		oRCLK,
output			oRAWR,
input				iVFOE
);
//
reg				rRDDT1, rRDDT2;
reg		[4:0]	rPLL_CNT;
(* romstyle = "logic" *)	// (наше) Quartus: таблица ниже -- в логику, а не в блочную память M4K
reg		[4:0]	w288;
//
initial
begin
	oRCLK = 1'b0;
	w288 = 5'd0;	// (наше) индекс в таблицу: без этого mem[{~oRAWR, X}] = X
end
//
always @( posedge iCLK )
begin
	rRDDT1 <= iRDDT;
	rRDDT2 <= ~rRDDT1;
end
//
always @( posedge iCLK )
if ( iVFOE == 1'b1 )
	oRCLK <= 1'b0;
else
	if ( w288 == 5'd16 )
		oRCLK <= ~oRCLK;
//
assign oRAWR = rRDDT1 & rRDDT2 & ~iVFOE;
//

// (наше) Таблица -- прямо здесь, а не $readmemh ("DPLL.hex"): синтез ищет файл в
// своей рабочей папке, и Synplify в Diamond его не находит -- таблица выходит пустой,
// и весь тракт чтения дискеты молча выбрасывается. Значения -- те же,
// что в DPLL.hex.
always @(posedge iCLK)
  case ({ ~oRAWR, w288 })
  6'h00: w288 <= 5'h01;  6'h01: w288 <= 5'h01;  6'h02: w288 <= 5'h02;  6'h03: w288 <= 5'h03;
  6'h04: w288 <= 5'h03;  6'h05: w288 <= 5'h04;  6'h06: w288 <= 5'h05;  6'h07: w288 <= 5'h06;
  6'h08: w288 <= 5'h06;  6'h09: w288 <= 5'h07;  6'h0A: w288 <= 5'h08;  6'h0B: w288 <= 5'h09;
  6'h0C: w288 <= 5'h09;  6'h0D: w288 <= 5'h0A;  6'h0E: w288 <= 5'h0B;  6'h0F: w288 <= 5'h0C;
  6'h10: w288 <= 5'h15;  6'h11: w288 <= 5'h16;  6'h12: w288 <= 5'h17;  6'h13: w288 <= 5'h18;
  6'h14: w288 <= 5'h18;  6'h15: w288 <= 5'h19;  6'h16: w288 <= 5'h1A;  6'h17: w288 <= 5'h1B;
  6'h18: w288 <= 5'h1B;  6'h19: w288 <= 5'h1C;  6'h1A: w288 <= 5'h1D;  6'h1B: w288 <= 5'h1E;
  6'h1C: w288 <= 5'h1E;  6'h1D: w288 <= 5'h1F;  6'h1E: w288 <= 5'h00;  6'h1F: w288 <= 5'h01;
  6'h20: w288 <= 5'h01;  6'h21: w288 <= 5'h02;  6'h22: w288 <= 5'h03;  6'h23: w288 <= 5'h04;
  6'h24: w288 <= 5'h05;  6'h25: w288 <= 5'h06;  6'h26: w288 <= 5'h07;  6'h27: w288 <= 5'h08;
  6'h28: w288 <= 5'h09;  6'h29: w288 <= 5'h0A;  6'h2A: w288 <= 5'h0B;  6'h2B: w288 <= 5'h0C;
  6'h2C: w288 <= 5'h0D;  6'h2D: w288 <= 5'h0E;  6'h2E: w288 <= 5'h0F;  6'h2F: w288 <= 5'h10;
  6'h30: w288 <= 5'h11;  6'h31: w288 <= 5'h12;  6'h32: w288 <= 5'h13;  6'h33: w288 <= 5'h14;
  6'h34: w288 <= 5'h15;  6'h35: w288 <= 5'h16;  6'h36: w288 <= 5'h17;  6'h37: w288 <= 5'h18;
  6'h38: w288 <= 5'h19;  6'h39: w288 <= 5'h1A;  6'h3A: w288 <= 5'h1B;  6'h3B: w288 <= 5'h1C;
  6'h3C: w288 <= 5'h1D;  6'h3D: w288 <= 5'h1E;  6'h3E: w288 <= 5'h1F;  6'h3F: w288 <= 5'h00;
  endcase

//
endmodule
