// IanPo/zx-pk.ru, 2016
// Модуль декодера MFM для HDL-модели КР1818ВГ93/WD1793
//
`default_nettype wire
//
module MFMDEC (
input					iCLK,
input					iRCLK,
input					iVFOE,
input					iSTART,
input					iSYNC,
input					iRDTRK,		// (наше) идёт Read Track: синхрометка A1 -- как у живой микросхемы
input			[47:0]	i3WORDS,
output reg	[7:0]	oBYTE_2_MAIN,
output reg			oBYTE_2_READ
);
//
reg					rMFMBIT;
reg					rRCLK1;
reg			[2:0]	rBIT_CNT, rBIT_CNT1;
reg			[7:0]	rCURR_BYTE;
// (наше) Read Track, синхрометка A1 A1 A1. Декодер идёт на 47 ячеек позади
// детектора: тот видит тройку в конце третьего A1, декодер в этот миг -- в
// начале первого, и без поправки отдавал бы все три. Живая микросхема (MB8877,
// клон WD1793, на той же дорожке) ловит метку на выбитом такте первого A1
// и сам этот байт не отдаёт: после 12 нулей -- «A1 A1 FB». Байт старого кадра,
// успевший собраться до выбитого такта (5 бит A1), уходит как есть: перед
// полем адреса со сбитым кадром -- «00 14 A1 A1 FE», 14 = 000 + 10100.
reg					rSYNC1;			// iSYNC тактом раньше: начало метки
reg					rDROP;			// не отдавать следующий собранный байт (первый A1)
reg					rOLD;			// дособираем байт старого кадра
reg					rOLD_PH;		// его фаза (как rMFMBIT)
reg			[3:0]	rOLD_CNT;		// его битов
reg			[3:0]	rOLD_CELLS;		// ячеек после метки: до выбитого такта их 10
reg			[7:0]	rOLD_BYTE;
reg					rOLD_OUT;		// старый байт собран -- отдать
wire				wA1 = ( i3WORDS[46:31] == 16'h4489 );	// в окне тройка A1, а не C2
//
initial
begin
	rMFMBIT = 1'b0;
	rBIT_CNT = 3'b0;
	oBYTE_2_READ = 1'b0;
	rSYNC1 = 1'b0;		// (наше)
	rDROP = 1'b0;
	rOLD = 1'b0;
	rOLD_OUT = 1'b0;
end
//
// (наше) метка A1 в Read Track: старый недособранный байт и пропуск первого A1
always @( posedge iCLK )
begin
	rSYNC1 <= iSYNC;
	rOLD_OUT <= 1'b0;
	if ( ( iSTART == 1'b0 ) || ( iRDTRK == 1'b0 ) )
		begin
			rOLD <= 1'b0;
			rDROP <= 1'b0;
		end
	else if ( ( iSYNC == 1'b1 ) && ( rSYNC1 == 1'b0 ) && wA1 )
		begin
			rDROP <= 1'b1;
			rOLD <= ( rBIT_CNT >= 3'd3 );		// 8 - k бит успеют до выбитого такта
			rOLD_CNT <= { 1'b0, rBIT_CNT };
			rOLD_BYTE <= rCURR_BYTE;
			rOLD_PH <= rMFMBIT;
			rOLD_CELLS <= 4'd0;
		end
	else
		begin
			if ( rOLD && ( rRCLK1 != iRCLK ) )
				begin
					if ( rOLD_PH == 1'b0 )
						begin
							rOLD_BYTE <= { rOLD_BYTE[6:0], i3WORDS[46] };
							rOLD_CNT <= rOLD_CNT + 1'b1;
							if ( rOLD_CNT == 4'd7 )
								begin
									rOLD <= 1'b0;
									rOLD_OUT <= 1'b1;
								end
						end
					rOLD_PH <= ~rOLD_PH;
					rOLD_CELLS <= rOLD_CELLS + 1'b1;
					if ( rOLD_CELLS == 4'd9 )
						rOLD <= 1'b0;		// дошли до выбитого такта -- не собрался
				end
			if ( rDROP && ( rBIT_CNT == 3'd0 ) && ( rBIT_CNT1 == 3'd7 ) && ( iSYNC != 1'b1 ) )
				rDROP <= 1'b0;				// первый A1 собран и проглочен
		end
end
//
always @( posedge iCLK )
begin
	rBIT_CNT1 <= rBIT_CNT;
	rRCLK1 <= iRCLK;
end
//
always @( posedge iCLK )
if ( ( iSTART == 1'b0 ) || ( iSYNC == 1'b1 ) )
	rBIT_CNT <= 3'b0;
else
	if ( rRCLK1 != iRCLK )
		begin
			if ( rMFMBIT == 1'b0 )
				begin
					if ( i3WORDS[46] == 1'b1 )
						rCURR_BYTE <= { rCURR_BYTE[6:0], 1'b1 };
					else
						rCURR_BYTE <= { rCURR_BYTE[6:0], 1'b0 };
					rBIT_CNT <= rBIT_CNT + 1'b1;
				end
		end
//
always @( posedge iCLK )
if ( ( iSTART == 1'b0 ) || ( iSYNC == 1'b1 ) )
	rMFMBIT <= 1'b0;
else
	if ( rRCLK1 != iRCLK )
		rMFMBIT <= ~rMFMBIT;
//
always @( posedge iCLK )
if ( iSTART == 1'b0 )
	oBYTE_2_READ <= 1'b0;
else
	if ( rOLD_OUT == 1'b1 )					// (наше) старый байт перед меткой
		begin
			oBYTE_2_READ <= 1'b1;
			oBYTE_2_MAIN <= rOLD_BYTE;
		end
	else if ( ( rBIT_CNT == 3'd0 ) && ( rBIT_CNT1 == 3'd7 ) && ( iSYNC != 1'b1 ) && ( rDROP == 1'b0 ) )	// (наше) rDROP
		begin
			oBYTE_2_READ <= 1'b1;
			oBYTE_2_MAIN <= rCURR_BYTE;
		end
	else
		oBYTE_2_READ <= 1'b0;
//
endmodule
