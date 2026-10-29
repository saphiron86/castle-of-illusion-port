; Boss Rush: the new 68000 code, placed past the end of the 512 KB ROM (see boss_rush.cpp,
; which patches the game to call it). Assemble with python tools/boss_rush_asm.py, which
; writes boss_rush_code.h.
;
; RAM (unused by the game):
;   $FFFFFF10  byte  Boss Rush ticked on the options screen
;   $FFFFFF11  byte  the boss being fought, 0-5
;   $FFFFFF12  6 words: each boss's time in game frames
;   $FFFFFF1E  byte  a Boss Rush game is being played
;   $FFFFFF20  word  the wait after a boss is defeated
;   $FFFFFF1F  byte  Mizrabel has taken her last hit
;   $FFFFFF22  byte  results screen: phase (0 fade in, 1 counting, 2 waiting, 3 fade out)
;   $FFFFFF23  byte  results screen: the line being counted (0-5 bosses, 6 total)
;   $FFFFFF24  word  results screen: frame counter for the phase
;   $FFFFFF26  word  results screen: the frames shown so far on the counting line

	cpu	68000
	padding	off

VDP_Data	equ	$C00000
VDP_Ctrl	equ	$C00004
BossRush	equ	$FFFFFF10
BossIndex	equ	$FFFFFF11
BossTimes	equ	$FFFFFF12
RushActive	equ	$FFFFFF1E
GameState	equ	$FFFFF300
ExitCountdown	equ	$FFFFF340	; non-zero while a stage's end plays out
Tries		equ	$FFFFF324
Continues	equ	$FFFFFF40	; continues + 1
NextTryScore	equ	$FFFFF282	; score for the next extra try
PowerRefill	equ	$FFFFF414	; set by the level-complete tally: refill at the next level start
BossDefeated	equ	$FFFFF451	; set by each boss as its death sequence ends
BossWait	equ	$FFFFFF20	; word: frames left before the next boss + 1, 0 = not waiting
WaitFrames	equ	300		; 5 seconds
FadeFrames	equ	60		; the fade to black at the end of it
GemObject	equ	$004D
ObjectsStart	equ	$FFFFD000	; object slots, $40 bytes each
ObjectsEnd	equ	$FFFFDC00	; Mickey's slot
StageIndex	equ	$FFFFFF42
LevelReset	equ	$0096CC		; level start: tries, power, flags
LevelLoad	equ	$0098F2		; called after it
StartStage	equ	$009FF0		; sets the level and act from StageIndex
Sub98DA		equ	$0098DA		; GameState_0E's calls replaced at $009808
Sub9834		equ	$009834
ClearObjects	equ	$001CC0
DrawTries	equ	$009BE4		; puts Tries in the HUD
Difficulty	equ	$FFFFFF0D	; 0 Normal, 1 Hard, 2 Practice
OptionRow	equ	$FFFFF41E	; options cursor row: 0-3
PadHeld		equ	$FFFFF308
DrawLayout	equ	$00A762		; draws layout pieces, one index per byte of d0
ObjectStates	equ	$001C74		; jumps to the routine for the object's state $3(a0)
CursorStates	equ	$003FCA		; the options cursor's state table
CursorValues	equ	$0042FE		; left/right on rows 0-2
CursorRedraw	equ	$0043F4		; the value redraw, after its first two instructions
SwingRight	equ	$001D26		; the cursor's swing after a change
MizrabelDown	equ	$FFFFFF1F
ResultPhase	equ	$FFFFFF22
ResultLine	equ	$FFFFFF23
ResultTimer	equ	$FFFFFF24
ResultShown	equ	$FFFFFF26
Palette		equ	$FFFFF780	; the palette VInt copies to CRAM when PaletteDirty is set
PaletteTarget	equ	$FFFFF800	; the palette fades move towards
PaletteDirty	equ	$FFFFF403
SoundRequest	equ	$FFFFF40E
PlaySound	equ	$079F86
MdPlusSound	equ	$080750		; an MD+ pack's hook in place of PlaySound at the game's calls
PadPressed	equ	$FFFFF309
VIntMode	equ	$FFFFF302
ClearVRAM	equ	$00AD0A		; clears VRAM and the scroll buffers
LoadSolidTiles	equ	$00ADB4		; LoadSolidColorTiles
LoadGraphics	equ	$00A78E		; LoadGraphicsSets: Nemesis sets, one index per byte of d0
LoadPalette	equ	$00CF3E		; palette (a1) into PaletteTarget
DrawFrame	equ	$07C162		; the level-complete screen's border
Decompress	equ	$07FCEE		; Enigma: (a0) into (a1), tile base d0
RunObjects	equ	$00187C		; also rebuilds the sprite list
MizrabelSound	equ	$9B		; the sound Mizrabel's last hit plays
TickSound	equ	$BD		; the tally's counting tick
ResultsMusic	equ	$89		; the level-complete music
UpdateScore	equ	$009C42		; score into the HUD buffer, HudDirty bit 3 when it changed
DrawHud		equ	$009B4A		; writes the HUD parts flagged in HudDirty
HudDirty	equ	$FFFFF409
HudLabel	equ	$7CBC0002	; VDP command: the HUD (window plane, $B000) row 25, column 30
HudDigits	equ	$7D3C0002	; row 26, column 30, where the game writes the score
MdPlusOpen	equ	$03F7FA		; MD+ (mdplus.cpp): $CD54 opens the registers, 0 closes
MdPlusCommand	equ	$03F7FE
MdPlusFade	equ	$64		; 100 CD frames: 1.3 s, as long as the white fade's end

; VDP write command for plane A ($C000) at a cell.
cell	function row,col,($40000003|((((row*64+col)*2)&$3FFF)<<16))

; Swing heights for the four rows (the cursor's $36 target and starting $14).
RowY0	equ	$30
RowY1	equ	$60
RowY2	equ	$A0
RowY3	equ	$D8		; a little higher than the rows' spacing, centred on the small sign
TickRow	equ	24
TickCol	equ	22
ExitText	equ	$00A65C		; the exit line's 25 tile words (layout piece $1D)
ExitLength	equ	25
ExitRow		equ	27
ExitCol		equ	7
ExitTiles	equ	$300		; free on this screen (tiles $264-$53F are unused)

	org	$0D0000

; Entry points at fixed addresses (6 bytes each), for the patches in boss_rush.cpp.
	jmp	(DrawOptions).l		; $0D0000
	jmp	(CursorMain).l		; $0D0006
	jmp	(SetRowState).l		; $0D000C
	jmp	(Redraw).l		; $0D0012
	jmp	(StartGame).l		; $0D0018
	jmp	(LevelStart).l		; $0D001E
	jmp	(RushFrame).l		; $0D0024
	jmp	(TryLimit).l		; $0D002A
	jmp	(MizrabelHit).l		; $0D0030
	jmp	(ResultsStart).l	; $0D0036
	jmp	(ResultsFrame).l	; $0D003C
	jmp	(MizrabelFade).l	; $0D0042
	jmp	(HudFrame).l		; $0D0048
	jmp	(HudLevelStart).l	; $0D004E

; The end of every frame of play (GameState_0E, $009812, in place of jsr sub_009C42 and
; jmp sub_009B4A, the score's update and the HUD's): in a Boss Rush the HUD shows TIME and
; the boss's clock where SCORE and the score are, in the HUD's font and zero-padded like
; the score (00:41.23). The score still counts; it is only not drawn.
HudFrame:
	jsr	(UpdateScore).l
	tst.b	(RushActive).w
	beq.s	.hud
	bclr	#3,(HudDirty).w		; the score's digits
	bsr.s	DrawHudTime
.hud:
	jmp	(DrawHud).l

; The end of the level start (GameState_0C, $009680, in place of jmp sub_07C3D8): the
; same, so TIME is there from the fade-in on.
HudLevelStart:
	tst.b	(RushActive).w
	beq.s	.done
	bsr.s	DrawHudTime
.done:
	jmp	($07C3D8).l

DrawHudTime:
	lea	(VDP_Data).l,a1
	move.l	#HudLabel,4(a1)
	move.l	#$802D8022,(a1)		; TIME
	move.l	#$8026801E,(a1)
	move.w	#$800F,(a1)
	move.l	#HudDigits,4(a1)
	moveq	#0,d0
	move.b	(BossIndex).w,d0
	add.w	d0,d0
	lea	(BossTimes).w,a1
	move.w	(a1,d0.w),d2
	move.w	#$8000,d6		; high priority, palette 0, as the score
	moveq	#1,d7
	bra.w	WriteTime

; Start from the options screen ($009FCC). A Boss Rush begins at the first boss.
StartGame:
	move.b	(BossRush).w,(RushActive).w
	beq.s	.start
	move.w	#4,(StageIndex).w	; stage 5
	clr.b	(BossIndex).w
	clr.w	(BossWait).w
	clr.b	(MizrabelDown).w
	lea	(BossTimes).w,a1
	moveq	#5,d0
.clear:
	clr.w	(a1)+
	dbra	d0,.clear
.start:
	jsr	(StartStage).l
	move.b	(Difficulty).w,($FFFFFF0F).w
	rts

; Level start (GameState_0C, $009618), after the game's own tries and power: one try, no
; continues and no extra tries from score.
LevelStart:
	jsr	(LevelReset).l
	tst.b	($FFFFF40A).w		; the attract demo is never a Boss Rush (after a rush's
	beq.s	.play			; game over, nothing has cleared RushActive yet)
	clr.b	(RushActive).w
.play:
	tst.b	(RushActive).w
	beq.s	.done
	move.w	#1,(Tries).w
	move.b	#1,(Continues).w
	move.l	#$7FFFFFFF,(NextTryScore).w
	jsr	(DrawTries).l
.done:
	jmp	(LevelLoad).l

; Every frame of play while not paused (GameState_0E, $009808): count the boss's time until
; it is defeated (BossDefeated, set as its death sequence ends, which is when its gem would
; appear). The gem is removed, so there is nothing to collect; 5 seconds later the game
; goes straight to the next boss's level start, instead of the gem, its message, the tally
; and the walk to the next level. On Normal power refills as after a tally; on Hard it
; carries over. After the last boss the game carries on as usual.
RushFrame:
	jsr	(Sub98DA).l
	jsr	(Sub9834).l
	tst.b	(RushActive).w
	beq.s	.done
	cmpi.w	#$0E,(GameState).w
	bne.s	.done
	moveq	#0,d0
	move.b	(BossIndex).w,d0
	tst.b	(BossDefeated).w
	bne.s	.defeated
	cmpi.b	#5,d0
	bne.s	.count
	tst.b	(MizrabelDown).w	; Mizrabel's clock stops at her last hit
	bne.s	.done
.count:
	add.w	d0,d0
	lea	(BossTimes).w,a1
	cmpi.w	#$FFFF,(a1,d0.w)
	beq.s	.done
	addq.w	#1,(a1,d0.w)
.done:
	rts
.defeated:
	cmpi.b	#5,d0
	beq.s	.done
	bsr.s	RemoveGem
	move.w	(BossWait).w,d1
	bne.s	.wait
	move.w	#WaitFrames+1,(BossWait).w
	rts
.wait:
	subq.w	#1,d1
	move.w	d1,(BossWait).w
	cmpi.w	#FadeFrames+1,d1	; the last second: fade to black, a level every 8 frames
	bhi.s	.wait2
	move.w	d1,d2
	andi.w	#7,d2
	bne.s	.wait2
	bsr.w	FadeToBlack
.wait2:
	cmpi.w	#1,d1
	bne.s	.done
	clr.w	(BossWait).w
	addq.b	#1,(BossIndex).w
	moveq	#0,d1
	move.b	BossStages+1(pc,d0.w),d1
	move.w	d1,(StageIndex).w
	cmpi.b	#1,(Difficulty).w
	beq.s	.hard
	move.b	#1,(PowerRefill).w
.hard:
	move.w	#$0C,(GameState).w
	jmp	(ClearObjects).l

; Deletes the boss's gem (object $4D) wherever it is among the objects.
RemoveGem:
	lea	(ObjectsStart).w,a1
.object:
	cmpi.w	#GemObject,(a1)
	bne.s	.next
	movea.l	a1,a2
	moveq	#15,d1
.clear:
	clr.l	(a2)+
	dbra	d1,.clear
.next:
	lea	$40(a1),a1
	cmpa.w	#ObjectsEnd&$FFFF,a1
	bne.s	.object
	rts

; Game stage indexes of the bosses: stages 5, 9, 13, 23, 27 and 28.
BossStages:
	dc.b	4, 8, 12, 22, 26, 27

; Extra-try items ($000C1C, in place of cmpi.w #10,Tries before a bcc that skips the item):
; none in a Boss Rush.
TryLimit:
	tst.b	(RushActive).w
	bne.s	.none
	cmpi.w	#10,(Tries).w
	rts
.none:
	move.w	#0,ccr			; carry clear: the item gives nothing
	rts

; GameState_08 ($009EEE): draws the labels as before, then BOSS RUSH and its tick box.
DrawOptions:
	clr.b	(RushActive).w
	move.l	#$1D1C1B1A,d0
	jsr	(DrawLayout).l
	lea	(VDP_Data).l,a2
	lea	OptionsText(pc),a1
.piece:
	move.l	(a1)+,d0
	beq.s	.done
	move.l	d0,4(a2)
	move.w	(a1)+,d7
.word:
	move.w	(a1)+,(a2)
	dbra	d7,.word
	bra.s	.piece
.done:

; The exit line goes in front of the sprites, so it shows over Mickey when he swings down to
; Boss Rush. The font's letters are drawn on an opaque black (colour 15), which would cut
; boxes out of him, so each letter is copied to tile ExitTiles + its index with colour 15
; made transparent, and the line is redrawn from the copies with the priority bit; its
; blanks (tile $0F) become empty cells.
	lea	(ExitText).l,a3
	move.l	#cell(ExitRow,ExitCol),d6
	moveq	#ExitLength-1,d5
.letter:
	move.w	(a3)+,d1
	moveq	#0,d0			; blank: an empty cell
	cmpi.w	#$000F,d1
	beq.s	.cell
	bsr.s	CopyTransparent
	move.w	d1,d0
	addi.w	#$8000|ExitTiles,d0
.cell:
	move.l	d6,4(a2)
	move.w	d0,(a2)
	addi.l	#$00020000,d6		; next cell
	dbra	d5,.letter
	rts

; Copies tile d1 to tile ExitTiles + d1 with colour 15 replaced by 0 (transparent).
CopyTransparent:
	lea	-32(a7),a7
	move.w	d1,d2
	lsl.w	#5,d2			; VRAM address of the tile
	move.w	d2,d0
	andi.w	#$3FFF,d0
	swap	d0
	clr.w	d0
	rol.w	#2,d2
	andi.w	#3,d2
	move.w	d2,d0			; read command
	move.l	d0,4(a2)
	movea.l	a7,a4
	moveq	#15,d4
.read:
	move.w	(a2),(a4)+
	dbra	d4,.read
	move.w	d1,d2
	addi.w	#ExitTiles,d2
	lsl.w	#5,d2
	move.w	d2,d0
	andi.w	#$3FFF,d0
	ori.w	#$4000,d0
	swap	d0
	clr.w	d0
	rol.w	#2,d2
	andi.w	#3,d2
	move.w	d2,d0			; write command
	move.l	d0,4(a2)
	movea.l	a7,a4
	moveq	#15,d4
.write:
	move.w	(a4)+,d2
	moveq	#3,d3
.nibble:
	move.w	d2,d0
	andi.w	#$F,d0
	cmpi.w	#$F,d0
	bne.s	.keep
	andi.w	#$FFF0,d2
.keep:
	ror.w	#4,d2
	dbra	d3,.nibble
	move.w	d2,(a2)
	dbra	d4,.write
	lea	32(a7),a7
	rts

; VDP command, word count - 1, then the words.
OptionsText:
	dc.l	cell(24,5)
	dc.w	8, $001B,$0028,$002C,$002C,$0000,$002B,$002E,$002C,$0021	; BOSS RUSH
	dc.l	cell(23,21)
	dc.w	2, $417B,$417C,$4186
	dc.l	cell(24,21)
	dc.w	2, $4187,$000F,$4189
	dc.l	cell(25,21)
	dc.w	2, $517B,$517C,$5186
	dc.l	0

; The cursor object's state dispatch ($003FC2). States 1-3 are rows 0-2, 4 moving, 5-6 the
; swing after a change; the new state 7 is row 3. Row 2 now also goes down to it.
CursorMain:
	move.b	3(a0),d0
	cmpi.b	#3,d0
	beq.s	Row2
	cmpi.b	#7,d0
	beq.s	Row3
	lea	(CursorStates).l,a1
	jmp	(ObjectStates).l

Row2:
	move.b	(PadHeld).w,d6
	btst	#0,d6			; up
	beq.s	.down
	move.b	#1,(OptionRow).w
	move.w	#RowY1,$36(a0)
	move.b	#4,3(a0)
	bra.s	Values
.down:
	btst	#1,d6
	beq.s	Values
	move.b	#3,(OptionRow).w
	move.w	#RowY3,$36(a0)
	move.b	#4,3(a0)
Values:
	cmpi.b	#3,(OptionRow).w
	beq.s	Toggle
	jmp	(CursorValues).l

Row3:
	move.b	(PadHeld).w,d6
	btst	#0,d6
	beq.s	Toggle
	move.b	#2,(OptionRow).w
	move.w	#RowY2,$36(a0)
	move.b	#4,3(a0)
	jmp	(CursorValues).l

; The same presses that change the other rows' values (d5: newly pressed) tick or untick
; it, with the same swing. Not on Practice, which has no bosses.
Toggle:
	move.b	d5,d0
	andi.b	#$30,d0
	beq.s	.none
	cmpi.b	#2,(Difficulty).w
	beq.s	.none
	bchg	#0,(BossRush).w
	move.b	#5,3(a0)
	bclr	#1,2(a0)
	jmp	(SwingRight).l
.none:
	rts

; Where the cursor settles on a row ($004158, $0041A8, $0041D2): state row + 1 as before,
; but 7 for row 3.
SetRowState:
	move.l	d0,-(a7)
	move.b	(OptionRow).w,d0
	addq.b	#1,d0
	cmpi.b	#4,d0
	bne.s	.set
	moveq	#7,d0
.set:
	move.b	d0,3(a0)
	move.l	(a7)+,d0
	rts

; The value redraw ($0043EC) also draws the tick; Practice clears it first.
Redraw:
	cmpi.b	#2,(Difficulty).w
	bne.s	.draw
	clr.b	(BossRush).w
.draw:
	move.l	#cell(TickRow,TickCol),(VDP_Ctrl).l
	move.w	#$000F,d0		; blank
	tst.b	(BossRush).w
	beq.s	.put
	move.w	#$0031,d0		; X
.put:
	move.w	d0,(VDP_Data).l
	move.b	(Difficulty).w,d0
	cmpi.b	#1,d0
	jmp	(CursorRedraw).l

	include	"boss_rush_results.asm"
