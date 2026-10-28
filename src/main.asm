; Castle of Illusion (US, serial "Gk 00001015-00") disassembly
; Assembler: Macroassembler AS (asl -xx -q -U -A -L main.asm, then p2bin)
;
; Everything after the header is still a binary include from build/baserom.bin.
; As code and data are identified, they are split out into their own files and
; the binclude ranges below shrink. The build must always match the original ROM.

	cpu	68000
	supmode	on		; allow privileged instructions (move to sr, rte)
	padding	off		; never pad odd-length dc.b data
; AS keeps explicit instruction sizes as written, so disassembled code must
; always spell out .b/.w/.l and .s/.w branch sizes to reproduce the original.

	include	"src/hardware.asm"	; generated: hardware register and Z80 mailbox names
	include	"src/ram.asm"		; generated: RAM variable names
	include	"src/z80names.asm"	; generated: Z80-side address names for the sound driver
	include	"src/symbols.asm"	; generated: equates for code not yet disassembled
	include	"src/header.asm"

; ---------------------------------------------------------------------------
; $000200-$07FFFF: disassembled regions (config/regions.txt) interleaved with
; binclude ranges of the original ROM. Regenerate with python tools/disasm.py.
; ---------------------------------------------------------------------------
	include	"src/layout.asm"
