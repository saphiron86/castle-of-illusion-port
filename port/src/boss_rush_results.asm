; Boss Rush: Mizrabel's last hit, the results screen and the fades (included by
; boss_rush.asm).

; Mizrabel's last hit ($010890, where it plays its sound): her clock stops there.
MizrabelHit:
	move.b	#MizrabelSound,(SoundRequest).w
	st	(MizrabelDown).w
	rts

; Her end controller starting the fade to white ($010E48, in place of moveq #1,d0 and the
; jsr to sub_00A5C0 that spawns its objects): in a Boss Rush the music fades out with it,
; so the results music doesn't cut in over it. The game's own fade ($D0) for its music,
; and for an MD+ pack (whose patch passes $D0 on to nothing) a pause with a fade of
; MdPlusFade CD frames, sent the way the pack's patch talks to the cartridge.
MizrabelFade:
	tst.b	(RushActive).w
	beq.s	.spawn
	movem.l	d0-d7/a0-a6,-(a7)
	move.b	#$D0,(SoundRequest).w
	jsr	(PlaySound).l			; the game's own driver; MD+ gets its own fade below
	move.w	#$CD54,(MdPlusOpen).l
	move.w	#$1300|MdPlusFade,(MdPlusCommand).l
	move.w	#$0000,(MdPlusOpen).l
	movem.l	(a7)+,d0-d7/a0-a6
.spawn:
	moveq	#1,d0
	jmp	($00A5C0).l

; ---------------------------------------------------------------------------
; The results screen, in place of the ending (GameState_2C, $00A110, and GameState_2E,
; $00A158, which a Boss Rush reaches only after Mizrabel). By then her defeat has faded
; the screen to white; the results fade in from white, built like the level-complete
; screen: its border, the COMPLETED half of its logo, its font (letters orange, digits
; blue) and its counting tick. Each line counts up from zero; a button shows them all at
; once, and once they are all shown one fades to black and goes back to the Sega logo.
; ---------------------------------------------------------------------------

ResultsStart:
	tst.b	(RushActive).w
	bne.s	.rush
	move.w	($FFFFFF08).w,(VDP_Ctrl).l
	jmp	($00A118).l
.rush:
	move.w	($FFFFFF08).w,(VDP_Ctrl).l	; display off
	move.w	#$8B00,(VDP_Ctrl).l		; whole-screen scrolling
	move.w	#$9100,(VDP_Ctrl).l		; no window
	move.w	#$9200,(VDP_Ctrl).l
	jsr	(ClearObjects).l
	jsr	(ClearVRAM).l			; (this clears the palette buffer to black too, and
	lea	(Palette).w,a1			; turns off VInt, which copies it to CRAM), so
	move.l	#$C0000000,(VDP_Ctrl).l		; white again in both, as her fade left it: the
	moveq	#63,d7				; screen shows CRAM's backdrop colour while the
.white:						; display is off
	move.w	#$0EEE,(a1)+
	move.w	#$0EEE,(VDP_Data).l
	dbra	d7,.white
	move.b	#1,(PaletteDirty).w
	jsr	(LoadSolidTiles).l
	move.l	#$2726250A,d0			; the level-complete screen's graphics
	jsr	(LoadGraphics).l
	move.l	#$2B131F,d0
	jsr	(LoadGraphics).l
	moveq	#$22,d0				; and the tally's font (blue digits) over set $0A's
	jsr	(LoadGraphics).l
	lea	($68D04).l,a1			; the palettes go straight to the fade's target, so
	jsr	(LoadTarget).l			; the screen stays white from Mizrabel's fade on
	lea	($68D24).l,a1
	jsr	(LoadTarget).l
	lea	(PaletteTarget).w,a1		; as the tally: line 0 to line 3, line 1 to 2
	lea	$60(a1),a2
	moveq	#15,d7
.line3:
	move.w	(a1)+,(a2)+
	dbra	d7,.line3
	lea	$20(a1),a2
	moveq	#15,d7
.line2:
	move.w	(a1)+,(a2)+
	dbra	d7,.line2
	jsr	(DrawFrame).l
	lea	($FF0800).l,a1			; its logo's map: 24 x 3 at $FF083C
	lea	($7D80A).l,a0
	move.w	#$43B2,d0
	jsr	(Decompress).l
	lea	($FF083C+11*2).l,a1		; COMPLETED: the logo's columns 11-23
	move.l	#cell(3,13),d1
	moveq	#2,d6
.logo:
	move.l	d1,(VDP_Ctrl).l
	movea.l	a1,a3
	moveq	#12,d7
.logoword:
	move.w	(a3)+,(VDP_Data).l
	dbra	d7,.logoword
	lea	48(a1),a1
	addi.l	#$00800000,d1			; next row
	dbra	d6,.logo
	lea	ResultsText(pc),a1
	bsr.w	DrawStrings
	lea	NormalText(pc),a1
	cmpi.b	#1,(Difficulty).w
	bne.s	.difficulty
	lea	HardText(pc),a1
.difficulty:
	bsr.w	DrawStrings
	clr.b	(ResultPhase).w
	clr.b	(ResultLine).w
	clr.w	(ResultTimer).w
	clr.w	(ResultShown).w
	clr.w	(VIntMode).w
	move.b	#ResultsMusic,(SoundRequest).w
	bsr.w	CallSound
	move.w	($FFFFFF0A).w,(VDP_Ctrl).l	; display on
	move.w	#$2E,(GameState).w
	rts

ResultsFrame:
	tst.b	(RushActive).w
	bne.s	.rush
	move.w	#4,(VIntMode).w
	jmp	($00A15E).l
.rush:
	jsr	(RunObjects).l			; keeps the sprite list empty
	addq.w	#1,(ResultTimer).w
	moveq	#0,d0
	move.b	(ResultPhase).w,d0
	add.w	d0,d0
	move.w	.phases(pc,d0.w),d0
	jmp	.phases(pc,d0.w)
.phases:
	dc.w	ResultsFadeIn-.phases, ResultsCount-.phases, ResultsWait-.phases, ResultsFadeOut-.phases

ResultsFadeIn:
	move.w	(ResultTimer).w,d0
	move.w	d0,d1
	andi.w	#7,d1				; a level every 8 frames
	bne.s	.done
	bsr.w	FadeToTarget
	cmpi.w	#7*8,d0
	bcs.s	.done
	bra.s	NextPhase
.done:
	rts

NextPhase:
	addq.b	#1,(ResultPhase).w
	clr.w	(ResultTimer).w
	rts

; Counts the line up by 1/64 of its value a frame (so about a second), ticking every 4
; frames, then holds it for 20 frames before the next.
ResultsCount:
	move.b	(PadPressed).w,d0
	andi.b	#$F0,d0
	bne.s	.all
	moveq	#0,d0
	move.b	(ResultLine).w,d0
	bsr.w	LineValue
	move.w	(ResultShown).w,d2
	cmp.w	d1,d2
	bcc.s	.hold
	move.w	d1,d3
	lsr.w	#6,d3
	bne.s	.step
	moveq	#1,d3
.step:
	add.w	d3,d2
	bcs.s	.clamp
	cmp.w	d1,d2
	bls.s	.shown
.clamp:
	move.w	d1,d2
.shown:
	move.w	d2,(ResultShown).w
	clr.w	(ResultTimer).w
	bsr.w	DrawLine
	move.b	(ResultShown+1).w,d0
	andi.b	#3,d0
	bne.s	.done
	move.b	#TickSound,(SoundRequest).w
	bsr.w	CallSound
.done:
	rts
.hold:
	cmpi.w	#20,(ResultTimer).w
	bcs.s	.done
	clr.w	(ResultShown).w
	clr.w	(ResultTimer).w
	addq.b	#1,(ResultLine).w
	cmpi.b	#7,(ResultLine).w
	bcs.s	.done
	bra.s	NextPhase
.all:
	moveq	#0,d0
.line:
	move.w	d0,-(a7)
	bsr.w	LineValue
	move.w	d1,d2
	bsr.w	DrawLine
	move.w	(a7)+,d0
	addq.w	#1,d0
	cmpi.w	#7,d0
	bcs.s	.line
	bra.w	NextPhase

ResultsWait:
	move.b	(PadPressed).w,d0
	andi.b	#$F0,d0
	beq.s	.done
	move.b	#$D0,(SoundRequest).w		; the music fades, as when leaving the tally
	bsr.w	CallSound
	bra.w	NextPhase
.done:
	rts

ResultsFadeOut:
	move.w	(ResultTimer).w,d0
	move.w	d0,d1
	andi.w	#3,d1
	bne.s	.done
	bsr.w	FadeToBlack
	cmpi.w	#8*4,d0
	bcs.s	.done
	; The Sega logo clears RAM up to $FFFAFF only, so reset what a new game expects in
	; the page above, as declining a continue does: the stage (else the next game starts
	; at Mizrabel) and the continues flag (else it gets the rush's none).
	clr.b	(RushActive).w
	clr.w	(StageIndex).w
	clr.b	($FFFFFF41).w
	move.w	#$00,(GameState).w		; the Sega logo
.done:
	rts

; Plays SoundRequest as the game's own calls do: through an MD+ pack's hook when there is
; one (its code at MdPlusSound; past the 512 KB ROM there is $FF padding or nothing).
CallSound:
	cmpi.w	#$FFFF,(MdPlusSound).l
	beq.s	.game
	jmp	(MdPlusSound).l
.game:
	jmp	(PlaySound).l

; d0: line (0-5 a boss, 6 the total) -> d1: its time in frames.
LineValue:
	lea	(BossTimes).w,a1
	cmpi.w	#6,d0
	beq.s	.total
	add.w	d0,d0
	move.w	(a1,d0.w),d1
	lsr.w	#1,d0
	rts
.total:
	moveq	#0,d1
	moveq	#5,d2
.add:
	add.w	(a1)+,d1
	bcs.s	.full
	dbra	d2,.add
	rts
.full:
	moveq	#-1,d1
	rts

; Draws line d0's time, d2 frames, as M:SS.CC (minutes from one to two digits), right
; after its name.
DrawLine:
	move.l	#cell(11,25),d1
	cmpi.w	#6,d0
	bne.s	.boss
	move.l	#cell(24,25),d1
	bra.s	.at
.boss:
	move.w	d0,d3
	lsl.w	#8,d3				; two rows ($100 bytes) a line
	swap	d3
	clr.w	d3
	add.l	d3,d1
.at:
	move.l	d1,(VDP_Ctrl).l
	move.w	#$6000,d6
	moveq	#0,d7

; Writes d2 frames to the VDP as MM:SS.CC, each cell tile d6 + its font tile; d7 = 0
; leaves the tens of minutes blank when 0, 1 draws a 0 there (the HUD's zero-padded look).
; A frame is 1/59.92 s (NTSC): hundredths = frames * 27342 / 16384.
WriteTime:
	moveq	#0,d3
	move.w	d2,d3
	mulu.w	#27342,d3
	lsr.l	#7,d3
	lsr.l	#7,d3				; hundredths
	divu.w	#6000,d3			; minutes, and hundredths within the minute
	moveq	#0,d4
	move.w	d3,d4
	clr.w	d3
	swap	d3
	divu.w	#10,d4				; tens of minutes, units in the high word
	move.w	d4,d5
	bne.s	.tens
	tst.b	d7
	beq.s	.blank
.tens:
	add.w	d6,d5
	addi.w	#$10,d5
	bra.s	.put
.blank:
	moveq	#0,d5
.put:
	move.w	d5,(VDP_Data).l
	swap	d4
	add.w	d6,d4
	addi.w	#$10,d4
	move.w	d4,(VDP_Data).l
	move.w	d6,d5
	addi.w	#$3B,d5				; :
	move.w	d5,(VDP_Data).l
	divu.w	#100,d3				; seconds, and hundredths in the high word
	move.l	d3,d4
	clr.w	d4
	swap	d4
	ext.l	d3
	divu.w	#10,d3
	bsr.s	.pair
	move.w	d6,d5
	addi.w	#$36,d5				; .
	move.w	d5,(VDP_Data).l
	move.l	d4,d3
	divu.w	#10,d3
.pair:
	add.w	d6,d3				; tens
	addi.w	#$10,d3
	move.w	d3,(VDP_Data).l
	swap	d3
	add.w	d6,d3			; units
	addi.w	#$10,d3
	move.w	d3,(VDP_Data).l
	rts

; Steps every colour of the palette one level towards black.
FadeToBlack:
	movem.l	d0-d3/d7/a1,-(a7)
	lea	(Palette).w,a1
	moveq	#63,d7
.colour:
	move.w	(a1),d0
	move.w	#$000E,d1			; channel mask
	moveq	#2,d2				; one level
	moveq	#2,d3
.channel:
	move.w	d0,-(a7)
	and.w	d1,(a7)
	tst.w	(a7)+
	beq.s	.next
	sub.w	d2,d0
.next:
	lsl.w	#4,d1
	lsl.w	#4,d2
	dbra	d3,.channel
	move.w	d0,(a1)+
	dbra	d7,.colour
	move.b	#1,(PaletteDirty).w
	movem.l	(a7)+,d0-d3/d7/a1
	rts

; Steps every colour of the palette one level towards PaletteTarget.
FadeToTarget:
	movem.l	d0-d7/a1-a2,-(a7)
	lea	(Palette).w,a1
	lea	(PaletteTarget).w,a2
	moveq	#63,d7
.colour:
	move.w	(a1),d0
	move.w	(a2)+,d4
	move.w	#$000E,d1
	moveq	#2,d2
	moveq	#2,d3
.channel:
	move.w	d0,d5
	and.w	d1,d5
	move.w	d4,d6
	and.w	d1,d6
	cmp.w	d6,d5
	beq.s	.next
	bhi.s	.down
	add.w	d2,d0
	bra.s	.next
.down:
	sub.w	d2,d0
.next:
	lsl.w	#4,d1
	lsl.w	#4,d2
	dbra	d3,.channel
	move.w	d0,(a1)+
	dbra	d7,.colour
	move.b	#1,(PaletteDirty).w
	movem.l	(a7)+,d0-d7/a1-a2
	rts

; Draws (a1): VDP command, text, 0, padded to even; ends at a 0 command.
DrawStrings:
	move.l	(a1)+,d0
	beq.s	.end
	move.l	d0,(VDP_Ctrl).l
.char:
	moveq	#0,d0
	move.b	(a1)+,d0
	beq.s	.next
	bsr.s	Glyph
	move.w	d0,(VDP_Data).l
	bra.s	.char
.next:
	move.l	a1,d0
	addq.l	#1,d0
	andi.b	#$FE,d0
	movea.l	d0,a1
	bra.s	DrawStrings
.end:
	rts

; d0: character -> its tile in the font (palette 3), 0 for a space.
Glyph:
	cmpi.b	#' ',d0
	beq.s	.space
	cmpi.b	#'-',d0
	beq.s	.dash
	cmpi.b	#'A',d0
	bcc.s	.letter
	subi.b	#'0'-$10,d0
	bra.s	.palette
.letter:
	subi.b	#'A'-$1A,d0
	bra.s	.palette
.dash:
	moveq	#$3A,d0
.palette:
	ori.w	#$6000,d0
	rts
.space:
	moveq	#0,d0
	rts

; Names end at column 21, times start at column 25.
ResultsText:
	dc.l	cell(8,13)
	dc.b	"BOSS RUSH",0
	align	2
	dc.l	cell(11,10)
	dc.b	"OLD OAK TREE",0
	align	2
	dc.l	cell(13,7)
	dc.b	"JACK-IN-THE-BOX",0
	align	2
	dc.l	cell(15,13)
	dc.b	"STORM MEN",0
	align	2
	dc.l	cell(17,7)
	dc.b	"LICORICE DRAGON",0
	align	2
	dc.l	cell(19,15)
	dc.b	"THE OAF",0
	align	2
	dc.l	cell(21,14)
	dc.b	"MIZRABEL",0
	align	2
	dc.l	cell(24,12)
	dc.b	"TOTAL TIME",0
	align	2
	dc.l	0
NormalText:
	dc.l	cell(8,27)
	dc.b	"NORMAL",0
	align	2
	dc.l	0
HardText:
	dc.l	cell(8,29)
	dc.b	"HARD",0
	align	2
	dc.l	0
