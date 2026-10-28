; ---------------------------------------------------------------------------
; 68000 vector table ($000000-$0000FF)
; ---------------------------------------------------------------------------
; EntryPoint, VInt and HInt are defined in src/boot.asm and src/vint.asm.
ErrorTrap	equ	EntryPoint	; every unused vector restarts the game

Vectors:
	dc.l	$00FFFF00		; initial stack pointer
	dc.l	EntryPoint		; reset
	rept	26			; bus error .. spurious, IRQ 1-3
	dc.l	ErrorTrap
	endm
	dc.l	HInt			; IRQ 4: horizontal blank
	dc.l	ErrorTrap		; IRQ 5
	dc.l	VInt			; IRQ 6: vertical blank
	rept	33			; IRQ 7, TRAP #0-15, reserved
	dc.l	ErrorTrap
	endm

; ---------------------------------------------------------------------------
; Cartridge header ($000100-$0001FF)
; ---------------------------------------------------------------------------
Header:
	; Console name, copyright, game names, serial, checksum, I/O, ROM and RAM ranges,
	; SRAM, modem, notes and region: taken from your ROM as they are, so the game's
	; name and copyright line are not copied into this repository.
	binclude	"build/baserom.bin",$100,$100
