; Hardcore: a fourth difficulty on the options screen, after HARD (included at the end of
; boss_rush.asm, at its own address so nothing before it moves). It plays as Hard ($FFFF0D
; stays 1, so every Hard rule of the game applies), and also:
;   - every level starts with 1 power;
;   - one try for the whole game, and no continues;
;   - no extra tries: the items give nothing and the score awards none.
; Ticking BOSS RUSH switches it to HARD; choosing HARDCORE unticks BOSS RUSH.
; Also here: the options cursor wraps round from the top row to the bottom one and back.
;
; RAM (unused by the game):
;   $FFFFFF28  byte  HARDCORE chosen on the options screen (with Difficulty 1)
;   $FFFFFF29  byte  a Hardcore game is being played

HardcorePick	equ	$FFFFFF28
HardcoreActive	equ	$FFFFFF29
Power		equ	$FFFFF322
SetAnimation8	equ	$001D36		; the cursor's swing after a change back (B)
SetAnimation0	equ	$001D26		; ... and forward (C)
NameShown	equ	$00440E		; the value redraw, after the difficulty's name
DemoPlaying	equ	$FFFFF40A
NameRow		equ	3		; where the difficulty names are drawn (moved up by Boss Rush)
NameCol		equ	22

	org	$0D1000

; Entry points at fixed addresses (6 bytes each), for the patches in boss_rush.cpp.
	jmp	(HardcoreRow).l		; $0D1000
	jmp	(HardcoreRedraw).l	; $0D1006
	jmp	(HardcoreStart).l	; $0D100C
	jmp	(HardcoreLevel).l	; $0D1012
	jmp	(HardcoreTryItem).l	; $0D1018
	jmp	(OptionsWrap).l		; $0D101E

; The options cursor's state dispatch ($003FC2, before Boss Rush's): up on the top row
; swings Mickey down to the bottom one, and down on the bottom row up to the top, the way
; the game moves him between neighbouring rows (the row, the swing's target height, then
; state 4 to swing there). Everything else is Boss Rush's dispatch as before.
OptionsWrap:
	move.b	3(a0),d0
	cmpi.b	#1,d0			; on row 0
	bne.s	.notTop
	btst	#0,(PadHeld).w		; up
	beq.s	.asBefore
	move.b	#3,(OptionRow).w
	move.w	#RowY3,$36(a0)
	move.b	#4,3(a0)
	rts
.notTop:
	cmpi.b	#7,d0			; on row 3
	bne.s	.asBefore
	btst	#1,(PadHeld).w		; down
	beq.s	.asBefore
	clr.b	(OptionRow).w
	move.w	#RowY0,$36(a0)
	move.b	#4,3(a0)
	rts
.asBefore:
	jmp	(CursorMain).l

; The difficulty row's B and C ($00430A, in place of the game's own, which go round
; NORMAL, HARD, PRACTICE): NORMAL, HARD, HARDCORE, PRACTICE. d5: the buttons newly pressed
; (B goes back, C forward).
HardcoreRow:
	moveq	#0,d0
	move.b	(Difficulty).w,d0
	cmpi.b	#2,d0
	bne.s	.notPractice
	moveq	#3,d0
	bra.s	.choice
.notPractice:
	tst.b	(HardcorePick).w
	beq.s	.choice
	moveq	#2,d0
.choice:
	btst	#4,d5			; B: back
	beq.s	.forward
	subq.b	#1,d0
	bpl.s	.back
	moveq	#3,d0
.back:
	bsr.s	.apply
	jmp	(SetAnimation8).l
.forward:
	btst	#5,d5			; C: forward
	beq.s	.none
	addq.b	#1,d0
	cmpi.b	#3,d0
	ble.s	.forwardSet
	moveq	#0,d0
.forwardSet:
	bsr.s	.apply
	jmp	(SetAnimation0).l
.none:
	rts
; d0: 0 NORMAL, 1 HARD, 2 HARDCORE, 3 PRACTICE into Difficulty and HardcorePick, then the
; swing as the game starts it.
.apply:
	clr.b	(HardcorePick).w
	cmpi.b	#2,d0
	blt.s	.set
	beq.s	.hardcore
	moveq	#2,d0			; PRACTICE
	bra.s	.set
.hardcore:
	move.b	#1,(HardcorePick).w
	clr.b	(BossRush).w
	moveq	#1,d0			; played as Hard
.set:
	move.b	d0,(Difficulty).w
	move.b	#5,3(a0)
	bclr	#1,2(a0)
	rts

; The value redraw ($0043EC): HARDCORE in place of the difficulty's name (and the empty tick
; box), or Boss Rush's redraw as before. A tick in BOSS RUSH switches HARDCORE to HARD.
HardcoreRedraw:
	tst.b	(HardcorePick).w
	beq.s	.asBefore
	tst.b	(BossRush).w
	beq.s	.hardcore
	clr.b	(HardcorePick).w
.asBefore:
	jmp	(Redraw).l
.hardcore:
	lea	(VDP_Data).l,a2
	move.l	#cell(TickRow,TickCol),4(a2)
	move.w	#$000F,(a2)		; the tick box, empty
	move.l	#cell(NameRow,NameCol),4(a2)
	lea	HardcoreName(pc),a1
	moveq	#7,d7
.letter:
	move.w	(a1)+,(a2)
	dbra	d7,.letter
	jmp	(NameShown).l

HardcoreName:
	dc.w	$0021,$001A,$002B,$001D,$001C,$0028,$002B,$001E	; HARDCORE

; Start from the options screen ($009FCC): a Hardcore game when HARDCORE was chosen (never
; with a Boss Rush, which plays as Hard), then Boss Rush's start.
HardcoreStart:
	move.b	(HardcorePick).w,(HardcoreActive).w
	tst.b	(BossRush).w
	beq.s	.start
	clr.b	(HardcoreActive).w
.start:
	jmp	(StartGame).l

; Level start ($009618): Boss Rush's, then in a Hardcore game 1 power, one try, no
; continues and no extra tries from score. (The attract demo is never Hardcore.)
HardcoreLevel:
	jsr	(LevelStart).l
	tst.b	(DemoPlaying).w
	beq.s	.play
	clr.b	(HardcoreActive).w
.play:
	tst.b	(HardcoreActive).w
	beq.s	.done
	move.w	#1,(Power).w
	move.w	#1,(Tries).w
	move.b	#1,(Continues).w
	move.l	#$7FFFFFFF,(NextTryScore).w
	jsr	(DrawTries).l
.done:
	rts

; Extra-try items ($000C1C): nothing in a Hardcore game, else as Boss Rush has them.
HardcoreTryItem:
	tst.b	(HardcoreActive).w
	bne.s	.none
	jmp	(TryLimit).l
.none:
	move.w	#0,ccr			; carry clear: the item gives nothing
	rts
