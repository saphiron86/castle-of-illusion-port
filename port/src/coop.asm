; Co-op, the game's side (included at the end of boss_rush.asm, at its own address). Work
; in progress: see docs/notes/coop.md.
;
; Player 2 is a second Mickey run by Mickey's own code: each frame of play, after Mickey
; (object $01 in the slot at $FFDC00) has run, Player 2's copy of everything that is
; Mickey's own (his slot and the globals his code keeps, CoopSwapList) is swapped in, the pad
; bytes are set from Player 2's buttons, Mickey's code runs once more, and everything is
; swapped back. Player 2's animation frame goes to VRAM $B000 (tile $580: rows 0-23 of the
; window plane's map, which play never shows), and his sprites use that tile base.
;
; RAM:
;   $FFFFFF30  byte  Player 2's buttons, written by the port (coop.cpp), Start left out
;   $FFFFFF31  byte  Player 2 has joined (the port)
;   $FFFFFC00  byte  Player 2 is in the level (his saved state is valid)
;   $FFFFFC01  byte  Player 2's buttons last frame, for the newly pressed ones
;   $FFFFFC02  word  Player 1's pad bytes ($FFF308-9) while Player 2 runs
;   $FFFFFC10  Player 2's saved state, in CoopSwapList's order

P2Buttons	equ	$FFFFFF30
P2Joined	equ	$FFFFFF31
P2Active	equ	$FFFFFC00
P2Previous	equ	$FFFFFC01
P1Pad		equ	$FFFFFC02	; word: Player 1's pad bytes while Player 2 runs
P2Running	equ	$FFFFFC04	; byte: Player 2's turn is running
CameraA_X	equ	$FFFFFA84
CoopTest	equ	$FFFFFC06	; long: the test CoopAlsoPlayer2 repeats
CoopResult	equ	$FFFFFC0A	; byte: its result for Mickey
CoopResult2	equ	$FFFFFC0B	; byte: ... and for Player 2
CoopPower	equ	$FFFFFC0C	; word: Player 2's power before it
CoopHurt	equ	$FFFFFC0E	; byte: it hurt Player 2
P2PowerBefore	equ	$FFFFFCF6	; word: his power at the start of his turn
P2UploadFrame	equ	$FFFFFCF8	; byte: his frame for VInt to upload (0: none)
CoopObjSwapped	equ	$FFFFFCF9	; byte: Player 2 swapped in for the object running (CoopForObject)
P1PadObj	equ	$FFFFFCFA	; word: Player 1's pad bytes meanwhile
CoopWide	equ	$FFFFFC0F	; byte: the view's extra width each side (the port: 40 in widescreen)
CoopCamX	equ	$FFFFFD40	; word: the point the camera follows, moving at most 8 pixels a frame
CoopCamY	equ	$FFFFFD42
P2Holding	equ	$FFFFFC89	; his $F447 (holding a vine) in his saved state
P2SwingWait	equ	$FFFFFC8A	; his $F448
P2Vine		equ	$FFFFFC98	; his $F230 (the vine he holds)
P2Ground	equ	$FFFFFC83	; his $F410 (MickeyGround: bit 0 on the ground)
MickeyGround	equ	$FFFFF410
CoopTargetX	equ	$FFFFFD50	; long: the X (with its fraction) of the player the bosses aim at
CoopTargetTime	equ	$FFFFFD54	; word: frames until they pick again
CoopTargetP2	equ	$FFFFFD56	; byte: they aim at Player 2
CoopRandom	equ	$FFFFFD58	; word: co-op's own random numbers (the game's left alone)
CoopQuick	equ	$FFFFFD60	; long: the hitbox loader of the test CoopAlsoPlayer2 repeats
CoopQuickRect	equ	$FFFFFD64	; word: his rectangle it uses, in his saved state
P2Rect		equ	$FFFFFC62	; his $F330 (P2State+82)
P2BounceRect	equ	$FFFFFC6A	; his $F338
MickeyToStand	equ	$00CAA6
MickeyFlashing	equ	$FFFFF446
ShowPower	equ	$009BB0		; Power into the HUD
P2State		equ	$FFFFFC10
P2Power		equ	P2State+72	; his $F322
P2Tries		equ	P2State+140	; his $F324
SetAnimation	equ	$001CE8
MickeySetAnimation equ	$00CBE0
AnimateMickey	equ	$00C5DC
UpdateScrolling	equ	$00F6E4
BlockAt		equ	$009454
FollowWindow	equ	$FFFF9820	; words: the camera's right and left thresholds (CameraFollow)
; Each player's own tries, deaths and bubbles (CoopAfterBoth):
CoopStatus	equ	$FFFFFD68	; 2 bytes, Player 1's then Player 2's: St* below
CoopTimer	equ	$FFFFFD6A	; 2 words: frames before his bubble comes (StWait)
CoopDieTimer	equ	$FFFFFD6E	; word: the dying player's frames left (each his own: CoopSwapList)
CoopFlags	equ	$FFFFFD70	; 2 bytes: bit 0 his bubble pose is set, bit 1 he keeps his power,
				; bit 2 his sprites had priority before the bubble
CoopRestart	equ	$FFFFFD72	; byte: both down: 1 restart the level, 2 game over
CoopGameOver	equ	$FFFFFD73	; byte: a game over: Player 2's tries come back with the continue
CoopKnown	equ	$FFFFFD74	; byte: Player 2 has had his first spawn since joining
CoopArtDone	equ	$FFFFFD75	; byte: the bubble's and the HUD labels' tiles are in VRAM
CoopPrevBtn	equ	$FFFFFD76	; 2 bytes: the bubble buttons last frame
CoopPinned	equ	$FFFFFD78	; byte: bit 0 a player held at the left edge by a wall, bit 1 the right
CoopLastStage	equ	$FFFFFD7A	; word: the stage of the last level start (+1; 0: none yet)
CoopOffView	equ	$FFFFFD7C	; 2 bytes: frames off the view above or below
CoopVineGrab	equ	$FFFFFD44	; byte: a vine's grab is testing hands (CoopVineGrab1)
P2Squash	equ	$FFFFFD46	; byte: his $F444 (squashed by a boulder, TouchHurtsMickey2)
CoopFinale	equ	$FFFFFD47	; byte: the final boss's last hit: bit 7 the script has begun,
				; bit 6 the boss released (MickeyFinaleWalk's $F420), bit 5 the orbs'
				; tiles copied (CoopOrbTiles), bits 0-1 each
				; player's start still to come (Mickey's, Player 2's)
CoopFinaleIn	equ	$FFFFFD48	; byte: the players in the script (bit 0 Mickey, bit 1 Player 2)
CoopFinaleAt	equ	$FFFFFD49	; byte: ... and those at their place, waiting
CoopMusic	equ	$FFFFFD7E	; byte: the last music asked for (not a jingle: CoopPlaySound)
CoopMusicWait	equ	$FFFFFD7F	; byte: frames until it is asked for again (after a death's jingle)
CoopBubbles	equ	$FFFFFD80	; $40: Player 1's bubble, drawn like an object
CoopBubble2	equ	$FFFFFCA0	; $40: Player 2's (after his saved state; MD+ uses $FFFDE1)
CoopSoundRoutine equ	$0D7FFC		; the port: the routine the game calls for a sound (PlaySound, or an MD+ pack's)
P1BubbleBtn	equ	$FFFFFF32	; the port: the bubble button held (Player 1's, then Player 2's)
P1Continue	equ	$FFFFFF34	; the port: Start held 2 s while out of tries (Player 1's, Player 2's)
ReviveP1	equ	$FFFFFF36	; the port: Player 2 is leaving, Player 1 back in play now
CoopSettled	equ	$FFFFFF37	; for the port: Player 2 leaving, the camera is on Mickey alone
P2Leaving	equ	$FFFFFF38	; the port: Player 2 is leaving (as ReviveP1, but never cleared here)
StPlay		equ	0		; playing
StDying		equ	1		; his death (the fall, or the power's) is running
StWait		equ	2		; dead, hidden: his bubble comes when CoopTimer runs out
StBubble	equ	3		; in his bubble
StOut		equ	4		; out of tries, hidden, until he holds Start (P1Continue)
StPop		equ	5		; his bubble popped: back in play on his next turn (CoopRevive)
StDown		equ	6		; dead with the other down too: the level restarts
BubbleTiles	equ	$85A0		; VRAM $B400, palette 0, in front
LabelTiles	equ	$85BC		; VRAM $B780: P and 1 in reds, P and 2 in blues
CoopArt		equ	$0D8000		; the port: the bubble's 28 tiles, decoded and recoloured
FinaleArt	equ	$0D8400		; the port: the finale's orb and burst, 49 tiles (graphics set $2B's first)
QueuedFront	equ	$FFFFFCF0	; word: the draw queues' ends before he runs
QueuedNormal	equ	$FFFFFCF2
CameraSaved	equ	$FFFFFD00	; $40 bytes: CameraA while he runs
CameraA		equ	$FFFFFA80
DrawFront	equ	$FFFFF28A	; word: end of the draw queue drawn in front (from $E800)
DrawQueue	equ	$FFFFF28E	; word: end of the other draw queue (from $E882)
RunObject	equ	$001904
MickeySlot	equ	$FFFFDC00
UploadFlags	equ	$FFFFF404	; bit 0: Mickey's frame changed, upload it (VInt)
MickeyFrames	equ	$016B80		; Mickey's animation frames' tiles, a long each (frame 1 first)
CopyLongs32	equ	$00954C		; CopyLongsToPort: 32 longs (a1)+ to (a2); 8 calls: a frame, 32 tiles
P2Tiles		equ	$0580		; tile index of VRAM $B000
P2VramWrite	equ	$70000002	; VDP command: write VRAM $B000

	supmode	on			; the game runs in supervisor mode (the SR writes below)
	org	$0D2000

; Entry points at fixed addresses (6 bytes each), for the patches in boss_rush.cpp.
	jmp	(CoopObjects).l		; $0D2000
	jmp	(CoopCameraTarget).l	; $0D2006
	jmp	(CoopTouchHurts).l	; $0D200C
	jmp	(CoopTouchHurts2).l	; $0D2012
	jmp	(CoopBouncedOn).l	; $0D2018
	jmp	(CoopTouches).l		; $0D201E
	jmp	(CoopScrolling).l	; $0D2024
	jmp	(CoopVIntUpload).l	; $0D202A
	jmp	(CoopTouchesSlot).l	; $0D2030
	jmp	(CoopAfterObject).l	; $0D2036
	jmp	(CoopVineHeld).l	; $0D203C

; $0D2042: TouchHurtsMickey2's first 6 bytes, which its patch replaces, copied here by
; apply_coop from the image (so with the Invincibility cheat, its rts at $0092B4), then the rest.
CoopHurts2Replay:
	nop
	nop
	nop
	jmp	($0092B8).l
	jmp	(CoopStandClear).l	; $0D204E
	jmp	(CoopStandSet).l	; $0D2054
	jmp	(CoopBossPush).l	; $0D205A
	jmp	(CoopBossArm27).l	; $0D2060
	jmp	(CoopBossBlockR).l	; $0D2066
	jmp	(CoopBossBlockL).l	; $0D206C
	jmp	(CoopPlayerEntry).l	; $0D2072
	jmp	(CoopDieStart).l	; $0D2078
	jmp	(CoopDyingTick).l	; $0D207E
	jmp	(CoopDeathEnd).l	; $0D2084
	jmp	(CoopDrawHud).l		; $0D208A
	jmp	(CoopLevelInit).l	; $0D2090
	jmp	(CoopPlaySound).l	; $0D2096
	jmp	(CoopPlaySoundMd).l	; $0D209C
	jmp	(CoopVineGrab1).l	; $0D20A2
	jmp	(CoopBoulderR).l	; $0D20A8
	jmp	(CoopBoulderL).l	; $0D20AE
	jmp	(CoopFinaleCheck).l	; $0D20B4
	jmp	(CoopFinaleX).l		; $0D20BA
	jmp	(CoopFinaleArrive).l	; $0D20C0
	jmp	(CoopFinaleWaiting).l	; $0D20C6
	jmp	(CoopOrb).l		; $0D20CC
	jmp	(CoopOrbBurst).l	; $0D20D2
	jmp	(CoopDrawFlip).l	; $0D20D8
	jmp	(CoopOrbsGone).l	; $0D20DE
	jmp	(CoopOrbTiles).l	; $0D20E4

; MickeySolidObject ($00C142, in place of bclr #0,$E(a1); $00C200 and $00C33C, in place of
; bset #0,$E(a1)): the solid object's "stood on" bit is bit 0 for Mickey and bit 4 for Player 2
; (the game never uses bit 4 of $E): each player's run clears and sets only his own, so the
; second run no longer wipes the first's.
CoopStandClear:
	tst.b	(P2Running).w
	bne.s	.p2
	bclr	#0,$E(a1)
	rts
.p2:
	bclr	#4,$E(a1)
	rts
CoopStandSet:
	tst.b	(P2Running).w
	bne.s	.p2
	bset	#0,$E(a1)
	rts
.p2:
	bset	#4,$E(a1)
	rts

; The tests an object makes against Mickey, from its own code (a0: the object): each runs
; for Mickey as before, then, with Player 2 in the level, once more with Player 2's state
; swapped in, so what Mickey's own globals say (his rectangles at $F330 and $F338, his
; flashing, his power, his slot) is Player 2's. d7 is the result: set if either matched.
; Player 2 is skipped while his own turn is running (he is the one in place then).

; TouchHurtsMickey ($0092D2, in place of moveq #0,d7 and tst.b (MickeyFlashing).w).
CoopTouchHurts:
	move.l	#.mickey,(CoopTest).w
	move.l	#$00926A,(CoopQuick).w	; LoadHitbox, his rectangle at $F330
	move.w	#P2Rect&$FFFF,(CoopQuickRect).w
	bsr.w	CoopP1Out
	bne.w	CoopAlsoPlayer2
	bsr.s	.mickey
	bra.w	CoopAlsoPlayer2
.mickey:
	moveq	#0,d7
	tst.b	(MickeyFlashing).w
	jmp	($0092D8).l

; TouchHurtsMickey2 ($0092B2, the same).
CoopTouchHurts2:
	move.l	#CoopHurts2Replay,(CoopTest).w
	move.l	#$00928E,(CoopQuick).w	; LoadHitbox2, his rectangle at $F330
	move.w	#P2Rect&$FFFF,(CoopQuickRect).w
	bsr.w	CoopP1Out
	bne.w	CoopAlsoPlayer2
	bsr.w	CoopHurts2Replay
	bra.w	CoopAlsoPlayer2

; BouncedOn ($009318, in place of tst.b ($FFFFF40B).w and beq.s to its rts): an object
; Mickey bounced on is not tested again for Player 2.
CoopBouncedOn:
	move.l	#.mickey,(CoopTest).w
	move.l	#$00928E,(CoopQuick).w	; LoadHitbox2, his bounce rectangle at $F338
	move.w	#P2BounceRect&$FFFF,(CoopQuickRect).w
	bsr.w	CoopP1Out
	bne.w	CoopAlsoPlayer2
	bsr.s	.mickey
	tst.b	d7
	beq.w	CoopAlsoPlayer2
	rts
.mickey:
	moveq	#0,d7
	tst.b	($FFFFF40B).w
	beq.s	.none
	jmp	($00931E).l
.none:
	rts

; TouchesMickey ($009264, in place of bsr.s LoadHitbox and jmp sub_00935E): items, platforms,
; doors... An object that touches Player 2 (and not Mickey) acts on Mickey's slot and globals
; afterwards (stage 3's leaves move him), so for the rest of that object's code Player 2 is
; swapped into Mickey's place (CoopForObject), and swapped back after it (CoopAfterObject).
CoopTouches:
	move.l	#.mickey,(CoopTest).w
	move.l	#$00926A,(CoopQuick).w	; LoadHitbox, his rectangle at $F330
	move.w	#P2Rect&$FFFF,(CoopQuickRect).w
	bsr.w	CoopP1Out
	bne.s	.p2
	bsr.s	.mickey
	bne.s	.done
.p2:
	bsr.w	CoopAlsoPlayer2
	beq.s	.done
	bra.w	CoopForObject
.done:
	rts
.mickey:
	jsr	($00926A).l		; LoadHitbox
	jmp	($00935E).l

; The test of an object against Mickey's slot itself ($009258, in place of lea ($FFFFDC00).w,a1
; and bsr.s LoadHitbox: his hands' box, used by vines and ropes): tried on Player 2's saved
; slot too, and as above, an object that touches only him has him in Mickey's place.
CoopTouchesSlot:
	bsr.w	CoopP1Out
	bne.s	.p2
	tst.b	(CoopVineGrab).w	; a vine's grab (CoopVineGrab1): not just after letting go
	beq.s	.mickey
	tst.b	($FFFFF448).w
	bne.s	.p2
.mickey:
	lea	(MickeySlot).w,a1
	bsr.s	.test
	bne.s	.done
.p2:
	bsr.w	CoopCanTest
	bne.s	.no
	tst.b	(CoopVineGrab).w
	beq.s	.player2
	tst.b	(P2SwingWait).w
	bne.s	.no
.player2:
	lea	(P2State).w,a1
	bsr.s	.test
	beq.s	.done
	sf	(CoopVineGrab).w	; (Scc: the flags kept)
	bra.w	CoopForObject
.no:
	moveq	#0,d7
.done:
	sf	(CoopVineGrab).w
	rts
.test:
CoopHandsTest:				; a0's box against the hands of the player slot at a1
	jsr	($00926A).l		; LoadHitbox
	lea	($00924E).l,a6		; the hands' box
	jmp	($0091F2).l

; A vine or rope not held ($000AB2, its routine 1, in place of tst.b ($FFFFF448).w and bne.s
; past the grab): $F448 is Mickey's swing (8 while he holds a vine, then counting down after
; he lets go), so while he held one, the next vine was refused to Player 2 too. Each player's
; own: the grab goes on if either may take hold, and the hands' test (CoopTouchesSlot, told by
; CoopVineGrab) tries only those who may.
CoopVineGrab1:
	tst.b	($FFFFF448).w
	beq.s	.can
	tst.b	(P2Active).w
	beq.s	.cannot
	tst.b	(P2SwingWait).w
	beq.s	.can
.cannot:
	addq.l	#4,a7
	jmp	($000AE4).l
.can:
	st	(CoopVineGrab).w
	rts

; A vine or rope held ($000AD0, its routine 2, in place of bsr.s VineAnimate and tst.b
; ($FFFFF448).w): the other player can take hold too (the original lets one Mickey hold it,
; and lets go of it once he no longer touches it), and the vine stays held while either holds
; it. A player takes hold as in its routine 1: not holding or just let go ($F448 clear), and
; his hands on it.
CoopVineHeld:
	jsr	($000B04).l		; VineAnimate
	tst.b	($FFFFF448).w		; Mickey
	bne.s	.p2
	bsr.w	CoopP1Out
	bne.s	.p2
	lea	(MickeySlot).w,a1
	bsr.s	CoopHandsTest
	beq.s	.p2
	move.b	#1,($FFFFF447).w
	move.l	a0,($FFFFF230).w
.p2:
	bsr.s	CoopCanTest
	bne.s	.held
	tst.b	(P2SwingWait).w
	bne.s	.held
	lea	(P2State).w,a1
	bsr.s	CoopHandsTest
	beq.s	.held
	move.b	#1,(P2Holding).w
	move.l	a0,(P2Vine).w
.held:
	tst.b	($FFFFF447).w		; still held by Mickey or by Player 2?
	beq.s	.notMickey
	cmpa.l	($FFFFF230).w,a0
	beq.s	.keep
.notMickey:
	tst.b	(P2Active).w
	beq.s	.free
	tst.b	(P2Holding).w
	beq.s	.free
	cmpa.l	(P2Vine).w,a0
	beq.s	.keep
.free:
	subq.b	#1,3(a0)		; routine 1 again
.keep:
	jmp	($000AE4).l

; Z clear when Mickey is out of play (dying, dead, in his bubble or out of tries): objects
; neither hurt him nor act on him; d7 = 0 (no match), for the callers' "Player 2 too" part.
CoopP1Out:
	moveq	#0,d7
	tst.b	(CoopStatus).w
	rts

; Z clear when Player 2 can't be tested now: not in the level, or swapped in already (his turn,
; or an object's, CoopForObject).
CoopCanTest:
	tst.b	(P2Active).w
	beq.s	.cannot
	tst.b	(P2Running).w
	bne.s	.cannot
	tst.b	(CoopStatus+1).w	; dying, dead, in his bubble or out of tries
	bne.s	.cannot
	cmp.b	d0,d0			; Z set
	rts
.cannot:
	moveq	#1,d0			; Z clear (d0 is free: the tests set it again)
	rts

; Player 2 into Mickey's place for the rest of the object's code (a test matched him only):
; d7 = 1, Z clear, every register but d7 as it was.
CoopForObject:
	movem.l	d0-d2/a1-a3,-(a7)
	bsr.w	CoopSwap
	move.w	(PadHeld).w,(P1PadObj).w
	move.b	(P2Buttons).w,(PadHeld).w
	clr.b	(PadHeld+1).w
	move.b	#1,(P2Running).w
	move.b	#1,(CoopObjSwapped).w
	movem.l	(a7)+,d0-d2/a1-a3
	moveq	#1,d7
	rts

; RunAllObjects' loop, each object ($0018F4, in place of bsr.s RunObject and movem.l
; (a7)+,d7/a0; its lea $40(a0),a0 is left, being a branch target): the object runs, then Mickey
; is back in his place if Player 2 was swapped in for it, then the loop's registers come back.
CoopAfterObject:
	; Stood on by Player 2 only (bit 4 of $E, CoopStandSet) and not by Mickey (bit 0): it runs
	; with Player 2 in Mickey's place and bit 0 set, so what it does to the one standing on it
	; (a spring's launch, through Mickey's slot) goes to Player 2.
	btst	#5,2(a0)		; solid objects only (ObjFlags bit 5, as MickeySolidObjects)
	beq.s	.run
	btst	#4,$E(a0)
	beq.s	.run
	btst	#0,$E(a0)
	bne.s	.run
	bsr.w	CoopCanTest
	bne.s	.run
	move.w	(a0),d0			; (CoopCanTest used d0: RunObject takes the ID in it)
	bset	#0,$E(a0)
	bsr.w	CoopForObject
	jsr	(RunObject).l
	movea.l	8(a7),a1		; the object (the loop's a0)
	bclr	#0,$E(a1)
	bra.s	.ran
.run:
	move.w	(a0),d0			; its ID again: CoopCanTest sets d0 when he can't be tested
					; (out of play with his bit still set, the object ran ID 1,
					; Mickey's code: stage 26's wheel, the camera on it)
	jsr	(RunObject).l
.ran:
	tst.b	(CoopObjSwapped).w
	beq.s	.next
	bsr.s	CoopObjectDone
.next:
	movem.l	4(a7),d7/a0		; the loop's d7 and a0, under the return address
	move.l	(a7),8(a7)
	addq.l	#8,a7
	rts

CoopObjectDone:
	clr.b	(CoopObjSwapped).w
	clr.b	(P2Running).w
	move.w	(P1PadObj).w,(PadHeld).w
	bra.w	CoopSwap

; The test at CoopTest once more for Player 2, every register as the object had it; d7: set
; if Mickey's or his matched. If it hurt him, the HUD's power is Mickey's again afterwards.
CoopAlsoPlayer2:
	move.b	d7,(CoopResult).w	; each player's result, kept for what the object does next
	clr.b	(CoopResult2).w
	tst.b	(P2Active).w
	beq.w	.out
	tst.b	(P2Running).w
	bne.w	.out
	tst.b	(CoopStatus+1).w	; out of play: objects leave him alone
	bne.w	.out
	move.w	$10(a0),d0		; far from his rectangle ($F330's, in his saved state):
	sub.w	(P2State+86).w,d0	; nothing to test
	cmpi.w	#-96,d0
	blt.w	.out
	move.w	$10(a0),d0
	sub.w	(P2State+88).w,d0
	cmpi.w	#96,d0
	bgt.w	.out
	move.b	d7,(CoopResult).w
	movem.l	d0-d6/a0-a6,-(a7)
	; Every one of these tests needs the object's box on his rectangle: tried first on his
	; saved rectangle, without the swap (a swap for every enemy part near him, the ? box
	; clown's spring neck, cost frames).
	movea.l	(CoopQuick).w,a6
	jsr	(a6)			; the object's box (a5)
	movea.w	(CoopQuickRect).w,a1
	jsr	($009362).l		; against the rectangle at a1: d7
	tst.b	d7
	bne.s	.near
	movem.l	(a7)+,d0-d6/a0-a6
	moveq	#0,d7
	move.b	(CoopResult).w,d7
	tst.b	d7
	rts
.near:
	movem.l	(a7),d0-d6/a0-a6
	bsr.w	CoopSwapTests
	movem.l	(a7),d0-d6/a0-a6
	move.w	(Power).w,(CoopPower).w
	movea.l	(CoopTest).w,a6
	jsr	(a6)
	move.b	d7,(CoopResult2).w
	move.w	(Power).w,d0
	cmp.w	(CoopPower).w,d0
	sne	(CoopHurt).w
	bsr.w	CoopSwapTests
	tst.b	(CoopHurt).w
	beq.s	.shown
	jsr	(ShowPower).l
.shown:
	movem.l	(a7)+,d0-d6/a0-a6
	moveq	#0,d7
	move.b	(CoopResult).w,d7
	or.b	(CoopResult2).w,d7
.out:
	tst.b	d7			; the callers test the flags (EnemyContact: jsr BouncedOn, bne)
	rts

; RunAllObjects, after Mickey ($0018E0, in place of lea ($FFFFDC40).w,a0 and moveq #$E,d7
; before the slots from $FFDC40 run): Player 2, then those registers as before.
CoopObjects:
	tst.b	(CoopObjSwapped).w	; (never left set by an object, but just in case)
	beq.s	.unswapped
	bsr.w	CoopObjectDone
.unswapped:
	tst.b	(P2Joined).w
	bne.s	.joined
	clr.b	(P2Active).w
	bra.w	.done
.joined:
	cmpi.w	#$0E,(GameState).w	; level play only (a level's start takes him out of it,
	beq.s	.inPlay			; CoopLevelInit; taken out here, the flip of stage 8,
					; which starts in Mickey's turn, dropped him in on Mickey)
	cmpi.w	#$10,(GameState).w	; stage 8's flip shows the window plane over the screen:
	bne.w	.done			; its rows 0-23 blank, as alone (the stage loads no tiles
	tst.b	(CoopArtDone).w		; there), and the art back after it
	beq.w	.done
	bsr.w	CoopClearArt
	clr.b	(CoopArtDone).w
	clr.b	(P2UploadFrame).w
	bra.w	.done
.inPlay:
	tst.w	(MickeySlot).w
	beq.w	.done
	tst.b	(CoopArtDone).w
	bne.s	.art
	bsr.w	CoopLoadArt
	move.b	#1,(CoopArtDone).w
	tst.b	(P2Active).w		; his frame again too (cleared by CoopClearArt)
	beq.s	.art
	move.b	(P2State+$30).w,(P2UploadFrame).w
.art:
	tst.b	(P2Active).w
	bne.s	.alive
	bsr.w	CoopLevelStart
.alive:
	bsr.w	CoopSwap
	move.w	(Power).w,(P2PowerBefore).w
	move.w	(PadHeld).w,(P1Pad).w	; Player 1's held and newly pressed buttons
	move.b	(P2Buttons).w,d0
	move.b	(P2Previous).w,d1
	not.b	d1
	and.b	d0,d1
	move.b	d0,(P2Previous).w
	move.b	d0,(PadHeld).w
	move.b	d1,(PadHeld+1).w
	move.w	(MickeySlot+4).w,d0	; his tiles
	andi.w	#$F800,d0
	ori.w	#P2Tiles,d0
	move.w	d0,(MickeySlot+4).w
	lea	(CameraA).w,a1		; the camera follows Mickey from his code: keep it on
	lea	(CameraSaved).w,a2	; Player 1 (for now; the pair's camera comes later)
	moveq	#15,d1
.save:
	move.l	(a1)+,(a2)+
	dbra	d1,.save
	move.w	(DrawFront).w,(QueuedFront).w
	move.w	(DrawQueue).w,(QueuedNormal).w
	lea	(MickeySlot).w,a0
	move.w	(a0),d0			; RunObject takes the ID in d0
	move.b	#1,(P2Running).w
	jsr	(RunObject).l
	clr.b	(P2Running).w
	bclr	#0,(UploadFlags).w	; his frame changed: VInt uploads it (CoopVIntUpload)
	beq.s	.uploaded
	move.b	(MickeySlot+$30).w,(P2UploadFrame).w
.uploaded:
	lea	(CameraSaved).w,a1
	lea	(CameraA).w,a2
	moveq	#15,d1
.restore:
	move.l	(a1)+,(a2)+
	dbra	d1,.restore
	; His draw queue entries name $FFDC00, which will hold Mickey again when the sprites are
	; built (after every object has run): point them at his saved slot instead.
	movea.w	(QueuedFront).w,a1
	movea.w	(DrawFront).w,a2
	bsr.s	.repoint
	movea.w	(QueuedNormal).w,a1
	movea.w	(DrawQueue).w,a2
	bsr.s	.repoint
	move.w	(P1Pad).w,(PadHeld).w
	bsr.w	CoopSwap
	bsr.w	CoopAfterBoth
	bra.w	.done
.repoint:
	cmpa.l	a2,a1
	bhs.s	.repointed
	cmpi.w	#MickeySlot&$FFFF,(a1)+
	bne.s	.repoint
	move.w	#P2State&$FFFF,-2(a1)
	bra.s	.repoint
.repointed:
	rts
.done:
	bsr.w	CoopBossTarget
	lea	($FFFFDC40).w,a0
	moveq	#$E,d7
	rts

; CameraFollow ($00F76E, in place of move.w $10(a0),d0 and move.w $14(a0),d1, the player's
; position it steers the camera by): with Player 2 in the level, the point between the two
; players while Mickey runs. (While Player 2 runs, the camera is put back afterwards.)
CoopCameraTarget:
	move.w	$10(a0),d0
	move.w	$14(a0),d1
	cmpa.w	#MickeySlot&$FFFF,a0
	bne.w	.done
	tst.b	(P2Running).w
	bne.w	.done
	move.b	(P2Leaving).w,(CoopSettled).w	; (Player 2 not in the level: nothing to wait for)
	tst.b	(P2Active).w
	beq.w	.own
	clr.b	(CoopSettled).w
	; Player 2 leaving (the port's P2Leaving), Mickey in play (ReviveP1 brings him back
	; first): Mickey alone, and once the point is on him the port switches the image
	; (CoopSettled; switched at once, the original camera jumped to him and skipped the
	; planes' columns). (ReviveP1 is cleared after both turns, which can come before
	; Mickey's turn in the port's frame.)
	tst.b	(P2Leaving).w
	beq.s	.staying
	tst.b	(CoopStatus).w
	beq.s	.leaving
.staying:
	tst.b	(CoopStatus+1).w	; Player 2 out of play: Mickey alone
	bne.w	.limit2
	tst.b	(CoopStatus).w
	beq.s	.both
.p2Alone:
	move.w	(P2State+$10).w,d0	; Mickey out of play: Player 2 alone
	move.w	(P2State+$14).w,d1
	bra.w	.limit2
.leaving:
	move.w	d1,d3
	swap	d3
	move.w	d0,d3
	bsr.w	.limitXY
	cmp.w	d3,d0
	bne.w	.own
	swap	d3
	cmp.w	d3,d1
	bne.w	.own
	move.b	#1,(CoopSettled).w
	bra.w	.own
.both:
	; One carried through a tunnel by its current (stage 10, states $29 and $2A, on a set
	; path): the camera follows him alone; the other, left behind out of the view, comes in
	; his bubble (CoopPlayerStep).
	move.b	3(a0),d3
	subi.b	#$29,d3
	cmpi.b	#1,d3
	bhi.s	.p1Free
	move.b	(P2State+3).w,d3
	subi.b	#$29,d3
	cmpi.b	#1,d3
	bhi.w	.limit2			; Mickey carried, Player 2 not: Mickey alone
	bra.s	.pair
.p1Free:
	move.b	(P2State+3).w,d3
	subi.b	#$29,d3
	cmpi.b	#1,d3
	bls.s	.p2Alone		; Player 2 carried, Mickey not
.pair:
	add.w	(P2State+$10).w,d0
	asr.w	#1,d0
	add.w	(P2State+$14).w,d1
	asr.w	#1,d1
	; The view keeps both players at least 16 pixels inside it while they fit: the camera
	; X stays between cmin (the right-hand player 16 pixels inside the right edge) and cmax
	; (the left-hand one 16 inside the left edge). The camera moves to keep the point between
	; the thresholds at FollowWindow (right, then left; in the original view's pixels): it
	; goes right only past the point - right, left only below the point - left, so the
	; point is moved to make it go. When they no longer fit, the camera follows the one
	; walking away and the edge drags the other (CoopDrag), unless a wall holds that one
	; (CoopPinned): then the view stays, and the edge holds the one walking away instead.
	movem.l	d4-d7/a1-a2,-(a7)	; (CameraFollow's thresholds, in use after this)
	lea	(MickeySlot).w,a1	; a1 the left-hand player, a2 the right-hand one
	lea	(P2State).w,a2
	move.w	$10(a2),d2
	cmp.w	$10(a1),d2
	bge.s	.ordered
	exg	a1,a2
.ordered:
	moveq	#0,d3
	move.b	(CoopWide).w,d3
	move.w	$10(a2),d4		; cmin
	sub.w	d3,d4
	subi.w	#320-16,d4
	move.w	$10(a1),d5		; cmax
	add.w	d3,d5
	subi.w	#16,d5
	move.w	(CameraA_X).w,d6
	moveq	#3,d7			; bit 0: go right, bit 1: go left (3: they don't fit)
	cmp.w	d5,d4
	bgt.s	.notAhead
	moveq	#0,d7
	cmp.w	d4,d6
	bge.s	.notBehind
	bset	#0,d7
.notBehind:
	cmp.w	d5,d6
	ble.s	.notAhead
	bset	#1,d7
.notAhead:
	; A player held at an edge (a wall, a vine: CoopDrag): the camera goes no further away
	; from him, and back toward him only as far as keeps the other in (cmin, cmax limited by
	; each other). (Coming back to keep him 16 pixels in, beside one walking away, it lost
	; him again and jumped back every 18 frames; never coming back, it stayed put when the
	; other walked back to him.) Both held, or neither walking away: the camera stays.
	move.b	(CoopPinned).w,d2
	beq.w	.free
	cmpi.b	#3,d2
	beq.s	.hold
	moveq	#0,d7
	btst	#0,d2
	beq.s	.pinRight
	cmp.w	d4,d5			; held at the left: back left down to max(cmax, cmin)
	bge.s	.leftBound
	move.w	d4,d5
.leftBound:
	cmp.w	d5,d6
	ble.s	.window
	moveq	#2,d7
	bra.s	.window
.pinRight:
	cmp.w	d5,d4			; held at the right: back right up to min(cmin, cmax)
	ble.s	.rightBound
	move.w	d5,d4
.rightBound:
	cmp.w	d4,d6
	bge.s	.window
	moveq	#1,d7
	bra.s	.window
.hold:
	moveq	#0,d7
.window:
	move.w	(FollowWindow+2).w,d2	; point >= camera + left threshold
	add.w	d6,d2
	cmp.w	d2,d0
	bge.s	.notLow
	move.w	d2,d0
.notLow:
	move.w	(FollowWindow).w,d2	; point <= camera + right threshold
	add.w	d6,d2
	cmp.w	d2,d0
	ble.s	.oneWay
	move.w	d2,d0
	bra.s	.oneWay
.free:
	cmpi.b	#3,d7			; they don't fit (whatever the camera's X: tested on it,
					; it went back and forth between the two): the one walking away leads
	bne.s	.oneWay
	moveq	#1,d7
	tst.w	$18(a2)
	bgt.s	.oneWay
	moveq	#2,d7
	tst.w	$18(a1)
	bmi.s	.oneWay
	bra.s	.hold			; neither: the camera stays (back to the middle and on
					; again, it jumped while one walked into a wall)
.oneWay:
	btst	#0,d7
	beq.s	.notRightward
	move.w	d4,d2			; point >= cmin + right threshold
	add.w	(FollowWindow).w,d2
	cmp.w	d2,d0
	bge.s	.notRightward
	move.w	d2,d0
.notRightward:
	btst	#1,d7
	beq.s	.placed
	move.w	d5,d2			; point <= cmax + left threshold
	add.w	(FollowWindow+2).w,d2
	cmp.w	d2,d0
	ble.s	.placed
	move.w	d2,d0
.placed:
	movem.l	(a7)+,d4-d7/a1-a2
.limit2:
	bsr.s	.limitXY
.own:
	move.w	d0,(CoopCamX).w
	move.w	d1,(CoopCamY).w
.done:
	rts
; The point moves at most 8 pixels a frame: when the one it follows changes, it jumps,
; and a camera moving more than 16 pixels in a frame skips columns of the planes.
.limitXY:
	move.w	d0,d2
	sub.w	(CoopCamX).w,d2
	bsr.s	.limit
	add.w	(CoopCamX).w,d2
	move.w	d2,d0
	move.w	d1,d2
	sub.w	(CoopCamY).w,d2
	bsr.s	.limit
	add.w	(CoopCamY).w,d2
	move.w	d2,d1
	rts
.limit:
	cmpi.w	#8,d2
	ble.s	.notAbove
	moveq	#8,d2
.notAbove:
	cmpi.w	#-8,d2
	bge.s	.limited
	moveq	#-8,d2
.limited:
	rts

; UpdateScrolling ($00F6E4, in place of its jsr CameraFollow and jsr UpdateCameras), called
; from Mickey's code: on Player 2's turn only CameraFollow's part that keeps the player inside
; the level runs (the camera it steers is put back afterwards). The cameras' movement, the
; planes' streaming and the stage's frame effect run once a frame, on Mickey's turn: run twice,
; the second pass streamed tiles for a camera that was then put back, and moved CameraB twice.
CoopScrolling:
	jsr	($00F71A).l		; CameraFollow
	tst.b	(P2Running).w
	bne.s	.p2
	jsr	($00F7A4).l		; UpdateCameras
	; The stage's frame effect reads Mickey's position (stage 2's, StageEffect01, blacks out
	; the background's palette with Mickey's Y below $180, a pit's depth): with Mickey out of
	; play and Player 2 playing, it is given Player 2's, the one the camera follows (falling
	; into a pit, Mickey blacked out the background above Player 2).
	tst.b	(P2Active).w
	beq.s	.effect
	tst.b	(CoopStatus).w
	beq.s	.effect
	tst.b	(CoopStatus+1).w
	bne.s	.effect
	move.l	(MickeySlot+$10).w,-(a7)
	move.l	(MickeySlot+$14).w,-(a7)
	move.l	(P2State+$10).w,(MickeySlot+$10).w
	move.l	(P2State+$14).w,(MickeySlot+$14).w
	jsr	($00F6EC).l
	move.l	(a7)+,(MickeySlot+$14).w
	move.l	(a7)+,(MickeySlot+$10).w
	bra.s	.river
.effect:
	jsr	($00F6EC).l		; the frame effect
.river:
	; Stage 12's (StageEffect12, effect 11) raging river catches Mickey ($9864, his own:
	; MickeyCheckCurrent carries him back and hurts him) once his Y reaches its surface
	; ($F8 less its rise at $FFFF9880); for Player 2 too (his $9864 at P2State+135).
	cmpi.w	#11,($FFFF9808).w
	bne.s	.p2
	tst.b	(P2Active).w
	beq.s	.p2
	tst.b	(CoopStatus+1).w
	bne.s	.p2
	tst.b	(P2State+135).w
	bne.s	.p2
	move.w	#$F8,d1
	sub.w	($FFFF9880).w,d1
	cmp.w	(P2State+$14).w,d1
	bhi.s	.p2
	move.b	#1,(P2State+135).w
.p2:
	rts

; ---------------------------------------------------------------------------------------------
; Each player's own power, tries, deaths and bubbles (docs/notes/coop.md, "Deaths and bubbles").
; CoopStatus holds each player's state (St*). A death (the power's or a pit's) runs as Mickey's
; does, without freezing the other objects while the other player plays on; at its end the
; player waits hidden (StWait) and 5.5 seconds later floats in from the edge of the view in a
; bubble (StBubble), which the other player pops by touching it (StPop, then back in play on his
; next turn). Out of tries (StOut) he waits until he holds Start (the port: P1Continue). With
; both down at once the level restarts, or with no tries left the game's game over follows.
; ---------------------------------------------------------------------------------------------

; Mickey's code ($00AEA2, in place of jsr sub_00CE3C(pc) and subq.w #1,($FFFFF316).w, then a
; nop): for a player out of play (hidden, or in his bubble) it is not run (CoopParked), and a
; player whose bubble popped comes back first (CoopRevive). The flags are subq's for the bpl.s
; after it.
CoopPlayerEntry:
	moveq	#0,d0
	move.b	(P2Running).w,d0
	lea	(CoopStatus).w,a1
	move.b	(a1,d0.w),d1
	cmpi.b	#StWait,d1
	blo.s	.play
	cmpi.b	#StDown,d1		; down with the other: his death runs on until the restart
	beq.s	.play
	tst.b	3(a0)			; a level's first frame: MickeyStart places him, all the same
	bne.s	.parked
.play:
	jsr	($00CE3C).l
	subq.w	#1,($FFFFF316).w
	rts
.parked:
	addq.l	#4,a7			; not back into Mickey's code
	cmpi.b	#StPop,d1
	beq.w	CoopRevive
	bra.w	CoopParked

; Back in play where his bubble popped (a0: his slot, d0: 0 Mickey, 1 Player 2), standing (he
; falls if there is no ground), flashing for 2 seconds; after a death with the power a try
; starts with. Then Mickey's code runs as usual this frame.
CoopRevive:
	lea	(CoopFlags).w,a1
	move.b	(a1,d0.w),d2
	clr.b	(a1,d0.w)
	lea	(CoopStatus).w,a1
	clr.b	(a1,d0.w)
	bsr.w	CoopBubbleOf
	move.w	$10(a2),$10(a0)
	clr.w	$12(a0)
	move.w	$14(a2),d1
	subq.w	#8,d1
	move.w	d1,$14(a0)
	clr.w	$16(a0)
	clr.l	$18(a0)
	clr.l	$1C(a0)
	bclr	#2,2(a0)
	bclr	#2,$31(a0)
	bclr	#7,4(a0)		; his sprites' priority as before the bubble
	btst	#2,d2
	beq.s	.priority
	bset	#7,4(a0)
.priority:
	clr.b	($FFFFF450).w
	clr.b	($FFFFF447).w
	clr.b	($FFFFF448).w
	clr.w	($FFFFF444).w
	clr.b	($FFFF9864).w		; not caught by stage 12's river
	clr.b	($FFFFF44D).w		; no tunnel's current from before
	clr.b	($FFFFF44C).w		; out in water, swimming (MickeyCheckWater, as he first went
	clr.b	($FFFFF44B).w		; in; with his flags as they were, he stood and walked under
	movem.l	d0-d2/d7/a6,-(a7)	; water), else on foot (as at a current's end, which clears
	moveq	#-16,d1			; them, though the bubble rode it with the other)
	moveq	#0,d2
	jsr	(BlockAt).l
	subi.b	#9,d7			; water blocks: 9-12
	cmpi.b	#3,d7
	bhi.s	.dry
	move.b	#1,($FFFFF44B).w
.dry:
	movem.l	(a7)+,d0-d2/d7/a6
	move.b	#$78,(MickeyFlashing).w
	btst	#1,d2
	bne.s	.power
	bsr.w	CoopStartPower
	move.w	d1,(Power).w
.power:
	jsr	(MickeyToStand).l
	jsr	($00CE3C).l
	subq.w	#1,($FFFFF316).w
	jmp	($00AEAA).l

; A player out of play, on his turn (a0: his slot, d0: 0 Mickey, 1 Player 2, d1: his state).
; Hidden (waiting, out of tries): where the other player is, so what aims at Mickey aims at the
; one playing. In his bubble: posed as when he falls into a pit, inside it, the bubble drawn in
; front of him. Objects leave him alone either way (CoopP1Out, CoopCanTest; he flashes too).
; On Mickey's turn the camera and the planes are updated here, as at the end of his code.
CoopParked:
	move.b	#2,(MickeyFlashing).w
	move.b	#1,($FFFFF450).w
	cmpi.b	#StBubble,d1
	beq.s	.bubble
	bset	#2,2(a0)		; not drawn
	tst.b	(P2Active).w		; (Player 2 not in the level yet: where he is)
	beq.s	.still
	move.l	(P2State+$10).w,$10(a0)	; the other player (in the swap area on either turn)
	move.l	(P2State+$14).w,$14(a0)
	bra.s	.still
.bubble:
	lea	(CoopFlags).w,a1
	bset	#0,(a1,d0.w)
	bne.s	.posed
	bclr	#2,(a1,d0.w)		; his sprites' priority, put back when he comes out
	btst	#7,4(a0)
	beq.s	.low
	bset	#2,(a1,d0.w)
.low:
	clr.b	($FFFFF447).w		; off any vine
	clr.b	($FFFFF448).w
	move.w	d0,-(a7)
	moveq	#$3C,d0			; MickeyToPitFall's animation
	jsr	(MickeySetAnimation).l
	move.w	(a7)+,d0
.posed:
	bclr	#2,2(a0)
	bset	#7,4(a0)		; in front of the planes' high tiles, as the bubble is
	bsr.w	CoopBubbleOf
	move.w	$10(a2),$10(a0)		; his feet 8 pixels above the bubble's bottom
	clr.w	$12(a0)
	move.w	$14(a2),d1
	subq.w	#8,d1
	move.w	d1,$14(a0)
	clr.w	$16(a0)
	movea.w	(DrawFront).w,a1	; the bubble, drawn in front of him
	move.w	a2,(a1)+
	move.w	a1,(DrawFront).w
	jsr	(AnimateMickey).l
.still:
	clr.l	$18(a0)
	clr.l	$1C(a0)
	tst.b	(P2Running).w
	bne.s	.done
	lea	(MickeySlot).w,a0
	jmp	(UpdateScrolling).l
.done:
	rts

; Mickey's death ($00AEFE, in place of move.w #$50,($FFFFF346).w): the dying player's own
; timer (CoopDieTimer, in CoopSwapList). $F346 also stops the stage's exits and some objects
; while Mickey dies; set only when the other player isn't playing on (or without him).
CoopDieStart:
	move.w	#$50,(CoopDieTimer).w
	tst.b	(P2Active).w
	beq.s	.freeze
	moveq	#0,d0
	move.b	(P2Running).w,d0
	eori.w	#1,d0
	lea	(CoopStatus).w,a1
	tst.b	(a1,d0.w)
	bne.s	.freeze
	move.b	#120,(CoopMusicWait).w	; the music again once the jingle ($8A, 1.9 s) is over
	rts
.freeze:
	move.w	#$50,($FFFFF346).w
.done:
	rts

; MickeyDying ($00B192, in place of subq.w #1,($FFFFF346).w and beq.s to $00B1AA): counts his
; own timer down ($F346 too while it is set), then the end of the death, as before.
CoopDyingTick:
	tst.w	($FFFFF346).w
	beq.s	.own
	subq.w	#1,($FFFFF346).w
.own:
	subq.w	#1,(CoopDieTimer).w
	beq.s	.over
	rts
.over:
	addq.l	#4,a7
	jmp	($00B1AA).l

; The end of a death ($00B1AA, the power's or a pit's, in place of tst.w (Tries).w and bne.s):
; with the other player playing on, he waits for his bubble (or, out of tries, for a continue).
; With the other down too: the level restarts if either has tries left, else game over (both
; set after both players' turns, CoopAfterBoth). Without Player 2, as before.
CoopDeathEnd:
	tst.b	(P2Active).w
	beq.s	.solo
	moveq	#0,d0
	move.b	(P2Running).w,d0
	lea	(CoopStatus).w,a1
	move.w	d0,d1
	eori.w	#1,d1
	tst.b	(a1,d1.w)
	bne.s	.both
	moveq	#StWait,d2
	tst.w	(Tries).w
	bne.s	.set
	moveq	#StOut,d2
.set:
	move.b	d2,(a1,d0.w)
	lea	(CoopFlags).w,a2
	clr.b	(a2,d0.w)
	lea	(CoopTimer).w,a2
	add.w	d0,d0
	move.w	#330,(a2,d0.w)		; 5.5 seconds
	rts
.both:
	move.b	#StDown,(a1,d0.w)
	move.b	#1,(CoopRestart).w
	move.w	(Tries).w,d2
	or.w	(P2Tries).w,d2		; the other's (in the swap area on either turn)
	bne.s	.done
	move.b	#2,(CoopRestart).w
.done:
	rts
.solo:
	tst.w	(Tries).w
	bne.s	.again
	jmp	($00B1B0).l
.again:
	jmp	($00B1B6).l

; d1: the power a try starts with (the level start's rule, $009726: Normal 3, Hard 2,
; Practice 5).
CoopStartPower:
	moveq	#3,d1
	move.b	(Difficulty).w,d3
	beq.s	.done
	moveq	#2,d1
	subq.b	#1,d3
	beq.s	.done
	moveq	#5,d1
.done:
	rts

; The level's start ($009652, in place of move.w #1,($FFFFDC00).w, where every stage, part
; and restart makes Mickey's object): Player 2 out of the level, so he drops in again with
; the players' states set afresh (CoopLevelStart), and the art loaded again. (Objects don't
; run between levels, so CoopObjects can't see one end.)
CoopLevelInit:
	move.w	#1,(MickeySlot).w
	clr.b	(P2Active).w
	clr.b	(CoopArtDone).w
	clr.b	(CoopMusicWait).w
	clr.w	(CoopStatus).w		; (CoopLevelStart sets who stays out)
	clr.b	(CoopFinale).w
	tst.w	(Tries).w
	bne.s	.done
	move.b	#StOut,(CoopStatus).w
.done:
	rts

; A level's (or part's) start, Player 2 not in it yet: he drops in where Mickey is, with his
; own power and tries (on first joining: 3 tries and a try's power; after a game over the
; continue's 3). A new stage refills his power as the tally (PowerRefill) does Mickey's. Who
; plays: whoever has tries left; the others stay out until they continue.
CoopLevelStart:
	move.w	(P2Power).w,-(a7)
	move.w	(P2Tries).w,-(a7)
	bsr.w	CoopSpawn
	move.w	(a7)+,(P2Tries).w
	move.w	(a7)+,d2
	bclr	#2,(P2State+2).w	; Mickey's copy may be hidden or flashing (out of play)
	clr.b	(P2State+120).w		; his $F446
	clr.b	(P2State+129).w		; his $F450
	clr.b	(P2Squash).w		; his $F444
	bsr.s	CoopStartPower
	tst.b	(CoopKnown).w
	bne.s	.known
	move.b	#1,(CoopKnown).w
	move.w	#3,(P2Tries).w
	move.w	d1,d2
.known:
	tst.b	(CoopGameOver).w
	beq.s	.notOver
	clr.b	(CoopGameOver).w
	move.w	#3,(P2Tries).w
.notOver:
	move.w	(StageIndex).w,d3	; a new stage refills his power as the tally does Mickey's
	addq.w	#1,d3
	cmp.w	(CoopLastStage).w,d3
	beq.s	.noRefill
	move.w	d3,(CoopLastStage).w
	cmp.w	d1,d2
	bhs.s	.noRefill
	move.w	d1,d2
.noRefill:
	tst.w	d2			; back from a death: a try's power
	bne.s	.p2Power
	move.w	d1,d2
.p2Power:
	move.w	d2,(P2Power).w
	tst.w	(Power).w		; Mickey back from a death on a new part (no refill there)
	bne.s	.p1Power
	move.w	d1,(Power).w
.p1Power:
	clr.w	(CoopStatus).w
	tst.w	(Tries).w
	bne.s	.p1
	move.b	#StOut,(CoopStatus).w
.p1:
	tst.w	(P2Tries).w
	bne.s	.p2
	move.b	#StOut,(CoopStatus+1).w
.p2:
	clr.w	(CoopFlags).w
	clr.b	(CoopRestart).w
	clr.b	(CoopPinned).w
	clr.w	(CoopOffView).w
	rts

; After both players' turns (Player 2 swapped out again): a restart a death asked for, the
; port's requests, each player's state, then the edges of the view.
CoopAfterBoth:
	move.b	(CoopRestart).w,d0
	beq.s	.noRestart
	clr.b	(CoopRestart).w
	move.w	#$FFFF,($FFFFF340).w	; as at the end of Mickey's death ($00B1B6)
	move.b	#1,($FFFFF40F).w
	subq.b	#1,d0
	beq.s	.noRestart
	move.b	#1,($FFFFF449).w	; game over
	move.b	#1,(CoopGameOver).w
.noRestart:
	tst.b	(ReviveP1).w		; Player 2 leaving: Player 1 back now, where he is
	beq.s	.noRevive
	clr.b	(ReviveP1).w
	move.b	(CoopStatus).w,d0
	cmpi.b	#StWait,d0
	blo.s	.noRevive
	cmpi.b	#StDown,d0
	beq.s	.noRevive
	tst.w	(Tries).w
	bne.s	.tries
	move.w	#3,(Tries).w
.tries:
	lea	(CoopBubbles).w,a2
	move.w	(P2State+$10).w,$10(a2)
	move.w	(P2State+$14).w,d0
	addq.w	#8,d0
	move.w	d0,$14(a2)
	clr.b	(CoopFlags).w
	move.b	#StPop,(CoopStatus).w
.noRevive:
	tst.b	(CoopMusicWait).w	; a death's jingle over: the music it stopped, again
	beq.s	.music
	subq.b	#1,(CoopMusicWait).w
	bne.s	.music
	tst.b	(CoopStatus).w		; (not with both down: the level restarts)
	beq.s	.again
	tst.b	(CoopStatus+1).w
	bne.s	.music
.again:
	move.b	(CoopMusic).w,(SoundRequest).w
	beq.s	.stage
	movea.l	(CoopSoundRoutine).l,a1	; as the game asks (with MD+, the pack's routine)
	jsr	(a1)
	bra.s	.music
.stage:
	jsr	($009686).l		; PlayStageMusic (not seen yet: joined mid-level)
.music:
	moveq	#1,d0			; Player 2, then Mickey
.player:
	bsr.s	CoopPlayerStep
	dbra	d0,.player
	bra.w	CoopDrag

; One player's state, after both turns (d0: 0 Mickey, 1 Player 2; kept).
CoopPlayerStep:
	lea	(CoopStatus).w,a4
	move.b	(a4,d0.w),d1
	lea	(MickeySlot).w,a3
	tst.w	d0
	beq.s	.slot
	lea	(P2State).w,a3
.slot:
	bsr.w	CoopBubbleOf
	cmpi.b	#StPlay,d1
	bne.w	.notPlay
	move.b	3(a3),d2		; dying ($1F) or falling into a pit ($21, $22)
	cmpi.b	#$1F,d2
	beq.s	.dying
	cmpi.b	#$21,d2
	beq.s	.dying
	cmpi.b	#$22,d2
	bne.s	.button
.dying:
	move.b	#StDying,(a4,d0.w)
	rts
.button:
	tst.b	(CoopFinale).w		; the final boss's script: no bubbles and no rescues
	bpl.s	.buttons		; (both walk to their places, CoopFinaleCheck)
	rts
.buttons:
	lea	(P1BubbleBtn).w,a5	; the bubble button, newly pressed
	move.b	(a5,d0.w),d2
	lea	(CoopPrevBtn).w,a5
	move.b	(a5,d0.w),d3
	move.b	d2,(a5,d0.w)
	tst.b	d2
	beq.s	.offView
	tst.b	d3
	bne.s	.offView
	bsr.w	.otherPlays
	bne.s	.offView
	lea	(CoopFlags).w,a5	; into a bubble where he is, keeping his power
	move.b	#2,(a5,d0.w)
	move.w	$10(a3),$10(a2)
	move.w	$14(a3),d2
	addq.w	#8,d2
	move.w	d2,$14(a2)
	bra.w	CoopBubbleStart
.offView:
	; The other carried through a tunnel by its current, he not, and out of the view to a
	; side (left behind; held by a wall, the camera went on with the other): his bubble, from
	; where he is, follows the other through the rock (stage 10's rapids).
	bsr.w	.otherPlays
	bne.s	.notLeft
	bsr.w	CoopOtherSlot
	move.b	3(a5),d2
	subi.b	#$29,d2
	cmpi.b	#1,d2
	bhi.s	.notLeft
	move.b	3(a3),d2
	subi.b	#$29,d2
	cmpi.b	#1,d2
	bls.s	.notLeft
	moveq	#0,d3
	move.b	(CoopWide).w,d3
	move.w	$10(a3),d2		; X from the view's left edge
	sub.w	(CameraA_X).w,d2
	add.w	d3,d2
	cmpi.w	#-16,d2
	blt.s	.leftBehind
	add.w	d3,d3
	addi.w	#320+16,d3
	cmp.w	d3,d2
	ble.s	.notLeft
.leftBehind:
	lea	(CoopFlags).w,a5	; keeping his power
	move.b	#2,(a5,d0.w)
	move.w	$10(a3),$10(a2)
	move.w	$14(a3),d2
	addq.w	#8,d2
	move.w	d2,$14(a2)
	bra.w	CoopBubbleStart
.notLeft:
	; Below the view, the other player playing above him (fallen, or left behind where a
	; current carried the other up): with ground somewhere below him, he comes back to the
	; other at once in a bubble, keeping his power (from below the view under the other);
	; over a pit (or nothing down to the level's bottom) he falls on into it, as alone.
	lea	(CoopOffView).w,a5
	move.w	$14(a3),d2
	sub.w	(CameraA).w,d2
	cmpi.w	#224+16,d2
	ble.s	.notBelow
	bsr.w	.otherPlays
	bne.s	.notBelow
	bsr.w	CoopOtherSlot
	move.w	$14(a5),d3
	cmp.w	$14(a3),d3
	bge.s	.notBelow
	bsr.w	CoopGroundBelow
	beq.s	.rescue
	lea	(CoopOffView).w,a5	; a pit: no bubble
	clr.b	(a5,d0.w)
	rts
.rescue:
	lea	(CoopOffView).w,a5
	clr.b	(a5,d0.w)
	bsr.w	CoopOtherSlot
	move.w	$10(a5),$10(a2)
	move.w	(CameraA).w,d2
	addi.w	#224+48,d2
	move.w	d2,$14(a2)
	lea	(CoopFlags).w,a5
	move.b	#2,(a5,d0.w)
	bra.w	CoopBubbleStart
.notBelow:
	; Above the view he stays: the higher player is the one the other comes back to (stage
	; 10's rapids carry a player up out of the view; brought back down after 2 seconds, as
	; before, neither could get on).
	rts
.notPlay:
	cmpi.b	#StWait,d1
	bne.s	.notWait
	lea	(CoopTimer).w,a5
	move.w	d0,d2
	add.w	d2,d2
	subq.w	#1,(a5,d2.w)
	bhi.s	.done
	bra.w	CoopBubbleEdge
.notWait:
	cmpi.b	#StOut,d1
	bne.s	.notOut
	lea	(P1Continue).w,a5	; Start held 2 seconds (the port): 3 tries, in a bubble
	tst.b	(a5,d0.w)
	beq.s	.done
	clr.b	(a5,d0.w)
	lea	(Tries).w,a5
	tst.w	d0
	beq.s	.p1Tries
	lea	(P2Tries).w,a5
.p1Tries:
	move.w	#3,(a5)
	lea	(CoopFlags).w,a5
	clr.b	(a5,d0.w)
	bra.w	CoopBubbleEdge
.notOut:
	cmpi.b	#StBubble,d1
	beq.w	CoopBubbleMove
.done:
	rts
; Z set when the other player is playing.
.otherPlays:
	move.w	d0,d2
	eori.w	#1,d2
	tst.b	(a4,d2.w)
	rts

; Z set: ground under the player at a3: going down from his feet, a block he would stand on
; (1-5, 8: MickeyFloorBlock's table) before a pit's (6) or the level's bottom.
CoopGroundBelow:
	movem.l	d0-d2/d6-d7/a0/a6,-(a7)
	movea.l	a3,a0
	move.w	($FFFFFA8C).w,d6	; the level's height in blocks
	lsl.w	#4,d6
	sub.w	$14(a0),d6		; pixels down to its bottom
	moveq	#0,d1
.down:
	cmp.w	d6,d1
	bge.s	.none
	moveq	#0,d2
	movem.l	d1/d6,-(a7)
	jsr	(BlockAt).l
	movem.l	(a7)+,d1/d6
	cmpi.b	#6,d7
	beq.s	.none
	cmpi.b	#1,d7
	blo.s	.next
	cmpi.b	#5,d7
	bls.s	.ground
	cmpi.b	#8,d7
	beq.s	.ground
.next:
	addi.w	#16,d1
	bra.s	.down
.ground:
	movem.l	(a7)+,d0-d2/d6-d7/a0/a6
	cmp.b	d0,d0			; Z set
	rts
.none:
	movem.l	(a7)+,d0-d2/d6-d7/a0/a6
	tst.l	(a7)			; Z clear
	rts

; a2: his bubble (d0: 0 Mickey, 1 Player 2).
CoopBubbleOf:
	lea	(CoopBubbles).w,a2
	tst.w	d0
	beq.s	.done
	lea	(CoopBubble2).w,a2
.done:
	rts

; a5: the other player's slot (d0: this player).
CoopOtherSlot:
	lea	(P2State).w,a5
	tst.w	d0
	bne.s	.done
	rts
.done:
	lea	(MickeySlot).w,a5
	rts

; His bubble comes in from the edge of the view (d0: the player, a2: his bubble, a4:
; CoopStatus): from beside the view on the side away from the other player, at his height.
CoopBubbleEdge:
	bsr.w	CoopOtherSlot
	moveq	#0,d3
	move.b	(CoopWide).w,d3
	move.w	(CameraA_X).w,d4
	move.w	d4,d5
	addi.w	#160,d5
	cmp.w	$10(a5),d5
	bhi.s	.right			; the other player left of the middle: from the right
	sub.w	d3,d4
	subi.w	#24,d4
	bra.s	.x
.right:
	add.w	d3,d4
	addi.w	#320+24,d4
.x:
	move.w	d4,$10(a2)
	move.w	$14(a5),d4
	addq.w	#8,d4
	move.w	d4,$14(a2)
	; falls through
; His bubble at its position (a2), set up to be drawn with the orb's animation (object $EA's,
; stage 28) from the tiles at BubbleTiles.
CoopBubbleStart:
	move.b	#StBubble,(a4,d0.w)
	lea	(CoopFlags).w,a5
	bclr	#0,(a5,d0.w)		; his pose on his next turn
	move.w	#$00EA,(a2)
	move.b	#$10,2(a2)		; never deleted off the view (DrawObject: 384 pixels away, it
	clr.w	$24(a2)			; was, and released a spawn entry at whatever $24 held)
	move.b	#1,3(a2)
	move.w	#BubbleTiles,4(a2)
	move.l	#$000550CE,6(a2)
	move.w	$14(a2),$30(a2)		; the height it bobs about
	clr.b	$32(a2)			; where in the bob
	move.b	#60,$33(a2)		; a second before it can be popped (gone in beside the other)
	movem.l	d0/a0,-(a7)
	movea.l	a2,a0
	moveq	#$E,d0
	jsr	(SetAnimation).l
	movem.l	(a7)+,d0/a0
	rts

; His bubble drifts toward the other player (1 pixel a frame, 2 when far), bobbing 8 pixels
; up and down, and pops when the other player, playing, touches it, after its first second
; (d0, a2, a4 as above).
CoopBubbleMove:
	tst.w	(a2)			; (lost: from the edge again)
	beq.w	CoopBubbleEdge
	bsr.w	CoopOtherSlot
	move.w	$10(a5),d2
	sub.w	$10(a2),d2
	bsr.w	.speed
	bsr.w	.step
	add.w	d2,$10(a2)
	move.w	$14(a5),d2		; its middle at his middle: its bottom 8 below his feet
	addq.w	#8,d2
	sub.w	$30(a2),d2
	bsr.w	.speed
	bsr.w	.step
	add.w	$30(a2),d2
	move.w	d2,$30(a2)
	addq.b	#1,$32(a2)
	moveq	#0,d3
	move.b	$32(a2),d3
	lsr.b	#1,d3
	andi.w	#63,d3
	lea	CoopSine(pc),a6
	move.b	(a6,d3.w),d3
	ext.w	d3
	add.w	d3,d2
	move.w	d2,$14(a2)
	tst.b	$33(a2)
	beq.s	.poppable
	subq.b	#1,$33(a2)
	rts
.poppable:
	move.w	d0,d2			; popped by the other player, playing, touching it
	eori.w	#1,d2
	tst.b	(a4,d2.w)
	bne.s	.done
	move.b	3(a5),d2		; (not while a current carries him: it follows him out)
	subi.b	#$29,d2
	cmpi.b	#1,d2
	bls.s	.done
	move.w	$10(a5),d2
	sub.w	$10(a2),d2
	bpl.s	.dx
	neg.w	d2
.dx:
	cmpi.w	#24,d2
	bhs.s	.done
	move.w	$14(a5),d2		; his middle (16 above his feet) to its middle (24 above its bottom)
	sub.w	$14(a2),d2
	addq.w	#8,d2
	bpl.s	.dy
	neg.w	d2
.dy:
	cmpi.w	#32,d2
	bhs.s	.done
	bsr.s	CoopBubbleInWall	; not inside a wall: he would come out stuck in it
	bne.s	.done
	move.b	#StPop,(a4,d0.w)
	move.w	$10(a2),d4
	move.w	$14(a2),d5
	subi.w	#24,d5
	bsr.w	CoopSparkles
	move.b	#$A8,(SoundRequest).w	; an enemy's beaten sound (EnemyBeaten)
	jsr	(PlaySound).l
.done:
	rts
; d3: its speed for the distance d2 (1 pixel a frame, 2 beyond 96 pixels, 4 beyond 224: from
; below the view, or after one carried through a tunnel, 1 took 12 seconds).
.speed:
	moveq	#1,d3
	move.w	d2,d4
	bpl.s	.abs
	neg.w	d4
.abs:
	cmpi.w	#96,d4
	bls.s	.slow
	moveq	#2,d3
	cmpi.w	#224,d4
	bls.s	.slow
	moveq	#4,d3
.slow:
	rts
; d2 limited to -d3..d3.
.step:
	cmp.w	d3,d2
	ble.s	.notAbove
	move.w	d3,d2
.notAbove:
	neg.w	d3
	cmp.w	d3,d2
	bge.s	.limited
	move.w	d3,d2
.limited:
	rts

; Z clear: his body at the bubble (a2), with room round it, is in a wall: blocks 1, 3 and 8
; (BlockAt) at 9 points, 20 pixels across and from 2 below his feet (8 above the bubble's
; bottom) to 40 above them. (14 by 32 let him out with his head in a ceiling, knocking it.)
CoopBubbleInWall:
	movem.l	d0-d2/d7/a0/a6,-(a7)
	movea.l	a2,a0
	lea	.points(pc),a1
	moveq	#8,d0
.point:
	move.b	(a1)+,d2
	ext.w	d2
	move.b	(a1)+,d1
	ext.w	d1
	jsr	(BlockAt).l
	cmpi.b	#1,d7
	beq.s	.wall
	cmpi.b	#3,d7
	beq.s	.wall
	cmpi.b	#8,d7
	beq.s	.wall
	dbra	d0,.point
	movem.l	(a7)+,d0-d2/d7/a0/a6
	cmp.b	d0,d0			; Z set
	rts
.wall:
	movem.l	(a7)+,d0-d2/d7/a0/a6
	tst.l	(a7)			; Z clear
	rts
.points:				; X, Y from the bubble's bottom middle
	dc.b	-10,-6, 0,-6, 10,-6, -10,-26, 0,-26, 10,-26, -10,-48, 0,-48, 10,-48

; Five small sparkles (object 3, as a beaten enemy's, its short animation) about d4, d5, in
; free slots of the pool at $FFDD80 (as SpawnSplash finds one).
CoopSparkles:
	movem.l	d0/a0,-(a7)
	lea	CoopSparkleSpots(pc),a5
	lea	($FFFFDD80).w,a1
	moveq	#8,d7
	moveq	#4,d6
.find:
	tst.w	(a1)
	beq.s	.free
.next:
	lea	$40(a1),a1
	dbra	d7,.find
	bra.s	.done
.free:
	move.w	#3,(a1)
	clr.b	2(a1)
	bset	#6,2(a1)
	move.w	#$00C4,4(a1)
	move.l	#$000152CA,6(a1)
	move.b	#$2F,$3A(a1)
	move.b	(a5)+,d2
	ext.w	d2
	add.w	d4,d2
	move.w	d2,$10(a1)
	move.b	(a5)+,d2
	ext.w	d2
	add.w	d5,d2
	move.w	d2,$14(a1)
	movea.l	a1,a0
	moveq	#2,d0
	jsr	(SetAnimation).l
	dbra	d6,.next
.done:
	movem.l	(a7)+,d0/a0
	rts
CoopSparkleSpots:
	dc.b	-14,-12, 13,-15, 1,2, -12,14, 15,11

; 8 * sin(2 pi n / 64)
CoopSine:
	dc.b	0,1,2,2,3,4,4,5,6,6,7,7,7,8,8,8
	dc.b	8,8,8,8,7,7,7,6,6,5,4,4,3,2,2,1
	dc.b	0,-1,-2,-2,-3,-4,-4,-5,-6,-6,-7,-7,-7,-8,-8,-8
	dc.b	-8,-8,-8,-8,-7,-7,-7,-6,-6,-5,-4,-4,-3,-2,-2,-1

; The edges of the view (8 pixels in from them; the original's 320, plus CoopWide each side
; in widescreen) drag a player along, both playing: one left behind is moved to the edge. A
; wall there holds him: he stays, and the view stops for him (CoopPinned, CoopCameraTarget),
; so the other is held at the far edge in turn. (Stuck, he can go into his bubble.) The
; camera keeps the players 16 pixels in (CoopCameraTarget), so one walking ahead gets past
; that before the edge holds him, and the camera, which moves before his turn, follows.
CoopDrag:
	clr.b	(CoopPinned).w
	tst.w	(CoopStatus).w
	bne.w	.done
	moveq	#0,d3
	move.b	(CoopWide).w,d3
	move.w	(CameraA_X).w,d4
	sub.w	d3,d4
	addq.w	#8,d4
	move.w	(CameraA_X).w,d5
	add.w	d3,d5
	addi.w	#320-8,d5
	lea	(MickeySlot).w,a0
	bsr.s	.one
	lea	(P2State).w,a0
.one:
	move.b	3(a0),d2		; carried by a tunnel's current: his path moves him
	subi.b	#$29,d2
	cmpi.b	#1,d2
	bls.s	.done
	bsr.w	.holding		; on a vine: the view goes no further from him while he
	beq.s	.loose			; holds it (held only past the edge, the view moved on each
	tst.b	($FFFFF447).w		; time his swing brought him in, and he swung out of it)
	beq.s	.notSame		; Both on the same vine: the view follows them as usual
	tst.b	(P2Holding).w		; (each held it for the other, and it never scrolled on);
	beq.s	.notSame		; the edges don't drag them off it either
	move.l	($FFFFF230).w,d2
	cmp.l	(P2Vine).w,d2
	beq.s	.done
.notSame:
	lea	(P2State).w,a1
	cmpa.w	#MickeySlot&$FFFF,a0
	beq.s	.other
	lea	(MickeySlot).w,a1
.other:
	move.w	$10(a1),d2
	cmp.w	$10(a0),d2
	blt.s	.otherLeft
	bset	#0,(CoopPinned).w
	rts
.otherLeft:
	bset	#1,(CoopPinned).w
	rts
.loose:
	move.w	$10(a0),d6
	cmp.w	d4,d6
	blt.s	.left
	cmp.w	d5,d6
	bgt.s	.right
.done:
	rts
.left:
	bsr.s	.holding
	bne.s	.heldLeft
	move.w	d4,$10(a0)
	moveq	#8,d2
	bsr.s	.wall
	beq.s	.done
	move.w	d6,$10(a0)
.heldLeft:
	bset	#0,(CoopPinned).w
	rts
.right:
	bsr.s	.holding
	bne.s	.heldRight
	move.w	d5,$10(a0)
	moveq	#-8,d2
	bsr.s	.wall
	beq.s	.done
	move.w	d6,$10(a0)
.heldRight:
	bset	#1,(CoopPinned).w
	rts
; Z clear: he holds a vine or rope, which holds him as a wall does (the vine sets his X:
; dragged, he and the view swung against the edge; let go, he could fall into a pit, as
; when widescreen is turned off with the two on vines apart).
.holding:
	cmpa.w	#MickeySlot&$FFFF,a0
	bne.s	.p2Holding
	tst.b	($FFFFF447).w
	rts
.p2Holding:
	tst.b	(P2Holding).w
	rts
; Z clear: a wall d2 pixels to his side, at his feet, middle or head (MickeyLeftBlock and
; MickeyRightBlock's points; blocks 1 and 8 solid, 3 solid in its lower half).
.wall:
	movem.l	d2-d6,-(a7)
	move.w	d2,d6
	moveq	#-2,d1
	bsr.s	.point
	bne.s	.found
	moveq	#-16,d1
	bsr.s	.point
	bne.s	.found
	moveq	#-32,d1
	bsr.s	.point
.found:
	movem.l	(a7)+,d2-d6
	rts
.point:
	move.w	d6,d2
	jsr	(BlockAt).l
	cmpi.b	#1,d7
	beq.s	.solid
	cmpi.b	#8,d7
	beq.s	.solid
	cmpi.b	#3,d7
	bne.s	.open
	btst	#3,$15(a0)
	beq.s	.solid
.open:
	cmp.b	d7,d7			; Z set
	rts
.solid:
	tst.l	(a7)			; Z clear
	rts

; UploadMickeyFrame ($00D486, called from VInt, in place of bclr #0,($FFFFF404).w): Player
; 2's frame (queued by CoopObjects at P2UploadFrame) to VRAM $B000, then Mickey's as before.
; In VInt, like Mickey's, so the tiles change together with the sprite table: uploaded while
; the screen was drawn, a new frame's tiles showed under the old frame's sprites.
CoopVIntUpload:
	moveq	#0,d0
	move.b	(P2UploadFrame).w,d0
	beq.s	.mickey
	clr.b	(P2UploadFrame).w
	add.w	d0,d0
	add.w	d0,d0
	lea	(MickeyFrames).l,a1
	movea.l	-4(a1,d0.w),a1
	move.l	#P2VramWrite,(VDP_Ctrl).l
	lea	(VDP_Data).l,a2
	moveq	#7,d7
.copy:
	jsr	(CopyLongs32).l
	dbra	d7,.copy
.mickey:
	bclr	#0,(UploadFlags).w
	bne.s	.upload
	rts
.upload:
	jmp	($00D490).l

; Player 2 drops in where Mickey is: his saved state starts as a copy of Mickey's. (Offset
; to the left, he could land on a stage's left-edge exit, which he can trigger.)
CoopSpawn:
	lea	CoopSwapList(pc),a1
	lea	(P2State).w,a2
.area:
	move.w	(a1)+,d0
	beq.s	.copied
	movea.w	d0,a3
	move.w	(a1)+,d1
	subq.w	#1,d1
.byte:
	move.b	(a3)+,(a2)+
	dbra	d1,.byte
	bra.s	.area
.copied:
	clr.b	(P2Previous).w
	move.b	(MickeySlot+$30).w,(P2UploadFrame).w	; his first frame (Mickey's)
	move.b	#1,(P2Active).w
	rts

; Stage 13's boss pushing back (objects $DB and $DC, 13 places, in place of: move.l
; ($FFFFDC10).w,d0, cmp.l $10(a0),d0, bcs.s past, move.w #$FFF0,($FFFFDC18).w): each player
; on its right (X at least its X) gets X speed -16, not just Mickey. d0: Mickey's X, as after
; the original.
CoopBossPush:
	tst.b	(P2Active).w
	beq.s	.mickey
	move.l	(P2State+$10).w,d0
	cmp.l	$10(a0),d0
	bcs.s	.mickey
	move.w	#$FFF0,(P2State+$18).w
.mickey:
	move.l	(MickeySlot+$10).w,d0
	cmp.l	$10(a0),d0
	bcs.s	.done
	move.w	#$FFF0,(MickeySlot+$18).w
.done:
	rts

; Stage 27's boss's raised arm (object $CA, routine 4, $014464, in place of jsr TouchHurtsMickey
; and all that follows it). The original reacts to TouchHurtsMickey's flags, not to d7: they are
; "Z clear" not only on some hits but whenever Mickey is flashing (or holds a vine, or $F450),
; touching or not. So the usual push comes the frame after the hit (often the head's or the
; legs' hit, which run after the arm): the arm's timer at $FFD4A2 goes to 1, and Mickey is
; pushed if he is on the side the boss faces (bit 0 of $FFD4A7 clear: at or left of the legs'
; X at $FFD490, X speed -4; set: right of it, +4). The co-op wrapper's result is d7 (0 while
; flashing), so the arm never reacted to Mickey after a hit. Here the original test runs for
; each player (Player 2 with his test state swapped in, as CoopAlsoPlayer2 does), each keeps
; the original's flags, and each player they mark is pushed by the original's rule.
CoopBossArm27:
	bsr.w	.test
	sne	(CoopResult).w
	clr.b	(CoopResult2).w
	bsr.w	CoopCanTest
	bne.s	.tested
	move.l	a0,-(a7)
	bsr.w	CoopSwapTests
	movea.l	(a7),a0
	move.w	(Power).w,(CoopPower).w
	bsr.w	.test
	sne	(CoopResult2).w
	move.w	(Power).w,d0
	cmp.w	(CoopPower).w,d0
	sne	(CoopHurt).w
	bsr.w	CoopSwapTests
	movea.l	(a7)+,a0
	tst.b	(CoopHurt).w
	beq.s	.tested
	jsr	(ShowPower).l		; the HUD's power Mickey's again
.tested:
	move.b	(CoopResult).w,d0
	or.b	(CoopResult2).w,d0
	beq.s	.done
	move.w	#1,($FFFFD4A2).w
	tst.b	(CoopResult2).w
	beq.s	.mickey
	lea	(P2State).w,a1
	bsr.s	.push
.mickey:
	tst.b	(CoopResult).w
	beq.s	.done
	lea	(MickeySlot).w,a1
.push:
	move.l	$10(a1),d0
	btst	#0,($FFFFD4A7).w
	bne.s	.right
	cmp.l	($FFFFD490).w,d0
	bgt.s	.done
	move.l	#$FFFC0000,$18(a1)
	rts
.right:
	cmp.l	($FFFFD490).w,d0
	ble.s	.done
	move.l	#$00040000,$18(a1)
.done:
	rts
.test:
	moveq	#0,d7			; TouchHurtsMickey as the original, its flags kept
	tst.b	(MickeyFlashing).w
	jmp	($0092D8).l

; Stage 27's boss raising its arm (object $C8, $0139C8 facing left and $013B06 facing right, in
; place of its test of Mickey: in the air (Y above $1D0) or in a jump state ($13-$1C)). They are
; reached with its target (CoopTargetX) in its reach in front of it: X distance from the legs
; more than -$78 and below 0 (left), or above 0 and below $78 (right). It blocks for a player
; who is jumping inside that same reach, whichever player is targeted (without the reach, one
; player's jump anywhere raised the arm while the other was the one beside it). Z clear: it
; blocks. d0's low byte is Mickey's state when he is on the ground, as the original leaves it
; (the code after compares d0 as a long, its X distance with that byte in it).
CoopBossBlockL:
	movem.l	d1-d3/a1,-(a7)
	move.l	#$FF880000,d1		; reach: d1 < distance < d2
	moveq	#0,d2
	bra.s	CoopBossBlock
CoopBossBlockR:
	movem.l	d1-d3/a1,-(a7)
	moveq	#0,d1
	move.l	#$00780000,d2
CoopBossBlock:
	cmpi.w	#$1D0,(MickeySlot+$14).w
	blt.s	.tests
	move.b	(MickeySlot+3).w,d0
.tests:
	lea	(MickeySlot).w,a1
	bsr.s	.player
	bne.s	.done
	tst.b	(P2Active).w
	beq.s	.done			; (Z set: no block)
	lea	(P2State).w,a1
	bsr.s	.player
.done:
	movem.l	(a7)+,d1-d3/a1		; (the flags kept)
	rts
.player:
	move.l	$10(a1),d3
	sub.l	$10(a0),d3
	cmp.l	d1,d3
	ble.s	.no
	cmp.l	d2,d3
	bge.s	.no
	cmpi.w	#$1D0,$14(a1)
	blt.s	.yes
	cmpi.b	#$13,3(a1)
	blt.s	.no
	cmpi.b	#$1C,3(a1)
	ble.s	.yes
.no:
	cmp.b	d3,d3			; Z set
	rts
.yes:
	tst.l	(a7)			; Z clear (the return address)
	rts

; The bosses' target (each frame, after both players have run): the bosses that turn to face
; the player or go toward him read his X from CoopTargetX in the co-op image (their reads of
; $FFDC10, boss_rush.cpp's coop patches), which is Mickey's or Player 2's, picked at random
; every 2 to 4 seconds. Player 2 out of the level: Mickey's.
CoopBossTarget:
	subq.w	#1,(CoopTargetTime).w
	bpl.s	.picked
	move.w	(CoopRandom).w,d0	; xorshift
	bne.s	.seeded
	move.w	#$ACE1,d0
.seeded:
	move.w	d0,d1
	lsl.w	#7,d1
	eor.w	d1,d0
	move.w	d0,d1
	lsr.w	#8,d1
	lsr.w	#1,d1
	eor.w	d1,d0
	move.w	d0,d1
	lsl.w	#8,d1
	eor.w	d1,d0
	move.w	d0,(CoopRandom).w
	move.w	d0,d1
	andi.w	#127,d1
	addi.w	#120,d1
	move.w	d1,(CoopTargetTime).w
	tst.w	d0			; the top bit picks (bit 8 came out the same for picks on end)
	smi	(CoopTargetP2).w
.picked:
	tst.b	(P2Active).w
	beq.s	.mickey
	tst.b	(CoopStatus+1).w	; only a player in play is aimed at
	bne.s	.mickey
	tst.b	(CoopStatus).w
	bne.s	.p2
	tst.b	(CoopTargetP2).w
	beq.s	.mickey
.p2:
	move.l	(P2State+$10).w,(CoopTargetX).w
	rts
.mickey:
	move.l	(MickeySlot+$10).w,(CoopTargetX).w
	rts

; Exchanges Player 2's saved state with what is in place: by longs while both addresses are
; even, the rest by bytes (it runs twice every frame of play: byte by byte it cost about 1,300
; instructions a frame, and with the ? box clown out stage 7 ran past its frame).
CoopSwap:
	lea	CoopSwapList(pc),a1
	lea	(P2State).w,a2
.area:
	move.w	(a1)+,d0
	beq.s	.swapped
	movea.w	d0,a3
	move.w	(a1)+,d1
	move.w	a2,d2
	or.w	d2,d0
	btst	#0,d0
	bne.s	.bytes
.long:
	subq.w	#4,d1
	bcs.s	.rest
	move.l	(a3),d2
	move.l	(a2),(a3)+
	move.l	d2,(a2)+
	bra.s	.long
.rest:
	addq.w	#4,d1
.bytes:
	subq.w	#1,d1
	bmi.s	.area
	move.b	(a3),d2
	move.b	(a2),(a3)+
	move.b	d2,(a2)+
	bra.s	.bytes
.swapped:
	move.b	($FFFFF444).w,d2	; and $F444, the squash HurtMickey2 asks for (a boulder's):
	move.b	(P2Squash).w,($FFFFF444).w ; each player his own (the state above is full
	move.b	d2,(P2Squash).w		; up to his bubble at $FFFCA0)
	rts

; Exchanges the part of Player 2's state that objects' tests use (CoopTestList: address,
; offset in his saved state, length), a fraction of CoopSwap's, since it runs for every
; object that tests him.
CoopSwapTests:
	lea	CoopTestList(pc),a1
.area:
	move.w	(a1)+,d0
	beq.s	.swapped
	movea.w	d0,a3
	lea	(P2State).w,a2
	adda.w	(a1)+,a2
	move.w	(a1)+,d1
	move.w	a2,d2			; both even: by longs (as CoopSwap)
	or.w	d2,d0
	btst	#0,d0
	bne.s	.bytes
.long:
	subq.w	#4,d1
	bcs.s	.rest
	move.l	(a3),d2
	move.l	(a2),(a3)+
	move.l	d2,(a2)+
	bra.s	.long
.rest:
	addq.w	#4,d1
.bytes:
	subq.w	#1,d1
	bmi.s	.area
	move.b	(a3),d2
	move.b	(a2),(a3)+
	move.b	d2,(a2)+
	bra.s	.bytes
.swapped:
	move.b	($FFFFF444).w,d2	; and $F444, the squash HurtMickey2 asks for (a boulder's):
	move.b	(P2Squash).w,($FFFFF444).w ; each player his own (the state above is full
	move.b	d2,(P2Squash).w		; up to his bubble at $FFFCA0)
	rts

; Offsets follow CoopSwapList: the slot at 0, $F322 at 72, $F328 at 74, $F40B at 113, $F445
; at 119, $F450 at 129.
CoopTestList:
	dc.w	$DC1C, $1C, 4		; Y velocity (BouncedOn)
	dc.w	$DC31, $31, 1		; bounced (bit 1), dying (bit 2)
	dc.w	$F322, 72, 2		; power
	dc.w	$F330, 82, 16		; his rectangles
	dc.w	$F40B, 113, 1		; can bounce
	dc.w	$F445, 119, 4		; flashing ($F446) and the hurt flags
	dc.w	$F450, 129, 1		; invincible for now
	dc.w	$F324, 140, 2		; tries (HurtMickey takes one with the last power)
	dc.w	0

; What is Mickey's own, as address (low word) and length, from what his code writes
; (docs/notes/coop.md). Shared by the pair, so left out: items, the stage's exit,
; sound requests, the finale's flags, the HUD's.
CoopSwapList:
	dc.w	$DC00, $40		; his slot
	dc.w	$F310, 8		; the pad as his code reads it
	dc.w	$F322, 2		; power
	dc.w	$F328, 24		; his rectangles for touching and bouncing ($F330, $F338)
	dc.w	$F342, 4
	dc.w	$F35E, 4
	dc.w	$F36E, 2
	dc.w	$F404, 5		; frame upload and movement flags
	dc.w	$F40B, 1		; bouncing (BouncedOn)
	dc.w	$F40F, 3		; MickeyGround and its neighbours
	dc.w	$F413, 1
	dc.w	$F416, 1
	dc.w	$F445, 10		; MickeyFlashing and the hurt flags
	dc.w	$F450, 1
	dc.w	$F452, 1
	dc.w	$F456, 4
	dc.w	$9864, 1
	dc.w	$F230, 4		; the vine or rope he holds (each player his own)
	dc.w	$F324, 2		; tries (at 140: P2Tries)
	dc.w	CoopDieTimer&$FFFF, 2	; his death's timer (at 142)
	dc.w	0

; DrawHud ($009B4A, in place of lea (VDP_Ctrl).l,a2): with Player 2 in, the power and the
; tries are each player's, on two rows: "P1" in reds (row 25), "P2" in blues (row 26), then
; his power's five dots (columns 3-7), and at columns 14-16 x and his tries beside the tries
; icon (the POWER and TRIES labels go). The rest of the HUD (items, score) as before.
CoopDrawHud:
	lea	(VDP_Ctrl).l,a2
	lea	-4(a2),a3
	move.w	(Power).w,d4		; Player 1's (in the swap area while Player 2's turn runs)
	move.w	(Tries).w,d5
	move.w	(P2Power).w,d6
	move.w	(P2Tries).w,d7
	tst.b	(P2Running).w
	beq.s	.order
	exg	d4,d6
	exg	d5,d7
.order:
	move.l	#$7C820002,(a2)		; row 25, column 1
	move.w	#LabelTiles,(a3)
	move.w	#LabelTiles+1,(a3)
	move.w	d4,d0
	bsr.s	.power
	move.l	#$7C9C0002,(a2)		; row 25, column 14
	move.w	#$803E,(a3)		; x
	move.w	d5,d0
	bsr.s	.tries
	move.l	#$800F800F,(a3)		; (where "ES" of TRIES was)
	move.l	#$7D020002,(a2)		; row 26, column 1
	move.w	#LabelTiles+2,(a3)
	move.w	#LabelTiles+3,(a3)
	move.w	d6,d0
	bsr.s	.power
	move.l	#$7D1C0002,(a2)		; row 26, column 14
	move.w	#$803E,(a3)
	move.w	d7,d0
	bsr.s	.tries
	bclr	#0,(HudDirty).w
	bclr	#1,(HudDirty).w
	jmp	($009B80).l		; the items and the score
.power:					; five dots: red for each point of power, then blue
	moveq	#4,d1
.dot:
	move.w	#$807C,d2
	subq.w	#1,d0
	bpl.s	.full
	move.w	#$807D,d2
.full:
	move.w	d2,(a3)
	dbra	d1,.dot
	rts
.tries:					; two digits (ShowTries' tiles, $8010 + the digit)
	andi.l	#$FFFF,d0
	cmpi.w	#99,d0
	bls.s	.digits
	moveq	#99,d0
.digits:
	divu.w	#10,d0
	addi.w	#$8010,d0
	move.w	d0,(a3)
	swap	d0
	addi.w	#$8010,d0
	move.w	d0,(a3)
	rts

; The bubble's tiles (the port decodes object $EA's from the ROM, recoloured for palette 0,
; to CoopArt) to VRAM $B400 (rows 8-21 of the window plane's map, never shown in play, after
; Player 2's frame at $B000), then the HUD's labels after them at $B780: the font's P, 1 and
; 2 (tiles $29, $11, $12, read back from VRAM) in Mickey's reds and in blues. Once a level,
; from the main loop, with interrupts off (VInt also sets the VDP's address).
CoopLoadArt:
	move.w	sr,-(a7)
	move.w	#$2700,sr
	lea	(VDP_Ctrl).l,a2
	lea	(VDP_Data).l,a3
	move.l	#$74000002,(a2)		; write VRAM $B400
	lea	(CoopArt).l,a1
	move.w	#28*8-1,d1
.orb:
	move.l	(a1)+,(a3)
	dbra	d1,.orb
	lea	CoopLabels(pc),a1
	move.l	#$77800002,d5		; write VRAM $B780
	moveq	#3,d6
.label:
	move.l	(a1)+,(a2)		; read the font's tile
	lea	-32(a7),a7
	movea.l	a7,a4
	moveq	#7,d1
.read:
	move.l	(a3),(a4)+
	dbra	d1,.read
	move.l	d5,(a2)
	addi.l	#$00200000,d5		; the next tile
	movea.l	(a1)+,a5		; its colours: a table of 16
	movea.l	a7,a4
	moveq	#7,d1
.row:
	move.l	(a4)+,d2
	moveq	#7,d3
.pixel:
	rol.l	#4,d2
	moveq	#$F,d4
	and.b	d2,d4
	andi.b	#$F0,d2
	or.b	(a5,d4.w),d2
	dbra	d3,.pixel
	move.l	d2,(a3)
	dbra	d1,.row
	lea	32(a7),a7
	dbra	d6,.label
	move.w	(a7)+,sr
	rts
; Rows 0-23 of the window plane's map ($B000-$BBFF: Player 2's frame, the bubble, the
; labels) cleared, with interrupts off.
CoopClearArt:
	move.w	sr,-(a7)
	move.w	#$2700,sr
	move.l	#$70000002,(VDP_Ctrl).l	; write VRAM $B000
	lea	(VDP_Data).l,a3
	move.w	#$C00/4-1,d1
.clear:
	clr.l	(a3)
	dbra	d1,.clear
	move.w	(a7)+,sr
	rts

; VDP read commands of the font's tiles, and their colours.
CoopLabels:
	dc.l	$05200000, CoopReds	; P ($29)
	dc.l	$02200000, CoopReds	; 1 ($11)
	dc.l	$05200000, CoopBlues	; P
	dc.l	$02400000, CoopBlues	; 2 ($12)
; The font's light face (2) and its shading (4, $B): Mickey's light and dark reds (7, 9), or
; the power dots' blues (12, 10).
CoopReds:
	dc.b	0,1,7,3,9,5,6,7,8,9,10,9,12,13,14,15
CoopBlues:
	dc.b	0,1,12,3,10,5,6,7,8,9,10,10,12,13,14,15

; The routine the game calls for a sound (CoopSoundRoutine; the port patches its start to
; jump here): the music asked for (below $A0) is kept in CoopMusic, but for the jingles (stage
; clear $89, life lost $8A, boss beaten $99, game over $9D), so it can be asked for again after
; a death the other player plays on through (the life lost jingle replaces it).
; PlaySound ($079F86, in place of move.w #$2700,sr and movem.l d0-d1,-(a7)), then as before.
CoopPlaySound:
	move.w	#$2700,sr
	movem.l	d0-d1,-(a7)
	bsr.s	CoopKeepMusic
	jmp	($079F8E).l
; An MD+ pack's ($080750 in "Grant Kirkhope Arranged": bsr.s +2, rts, move.b (SoundRequest).w,d0,
; ..., in place of its first 6 bytes; the game's calls of PlaySound go there): then its own
; code after the move (its rts returns to the game's caller, as its own rts did).
CoopPlaySoundMd:
	bsr.s	CoopKeepMusic
	move.l	(CoopSoundRoutine).l,-(a7)
	addq.l	#8,(a7)
	move.b	(SoundRequest).w,d0
	rts
CoopKeepMusic:
	move.w	d0,-(a7)
	move.b	(SoundRequest).w,d0
	cmpi.b	#$A0,d0
	bhs.s	.done
	cmpi.b	#$89,d0
	beq.s	.done
	cmpi.b	#$8A,d0
	beq.s	.done
	cmpi.b	#$99,d0
	beq.s	.done
	cmpi.b	#$9D,d0
	beq.s	.done
	move.b	d0,(CoopMusic).w
.done:
	move.w	(a7)+,d0
	rts

; Stage 24's rolling boulders (object $4C): the original rolls one back once it is $B8 pixels
; ($E0 in the widened view) right of Mickey's X ($005B28: move.w ($FFFFDC10).w,d0, addi.w
; #range,d0) or left of it ($005B82, subi.w). In co-op, beyond both players in play: the
; rightmost one's X plus the range, or the leftmost one's less it, in d0. The call stands in
; for the move and the addi's opcode; the range word after it is read and stepped over.
CoopBoulderR:
	movem.l	d1/a1,-(a7)
	movea.l	8(a7),a1
	addq.l	#2,8(a7)
	bsr.s	CoopBoulderXs
	cmp.w	d0,d1
	ble.s	.right
	move.w	d1,d0
.right:
	add.w	(a1),d0
	movem.l	(a7)+,d1/a1
	rts
CoopBoulderL:
	movem.l	d1/a1,-(a7)
	movea.l	8(a7),a1
	addq.l	#2,8(a7)
	bsr.s	CoopBoulderXs
	cmp.w	d0,d1
	bge.s	.left
	move.w	d1,d0
.left:
	sub.w	(a1),d0
	movem.l	(a7)+,d1/a1
	rts
; d0: Mickey's X, d1: Player 2's; either one out of play takes the other's (neither: Mickey's).
CoopBoulderXs:
	move.w	(MickeySlot+$10).w,d0
	move.w	d0,d1
	tst.b	(P2Active).w
	beq.s	.done
	tst.b	(CoopStatus+1).w
	bne.s	.done
	move.w	(P2State+$10).w,d1
	tst.b	(CoopStatus).w
	beq.s	.done
	move.w	d1,d0
.done:
	rts

; The final boss's last hit ($F45A, set by Mizrabel's code) starts a script: Mickey drops to the
; floor (MickeyFinaleFall), walks to X $F0 (MickeyFinaleWalk) and waits ($F420 set, Mickey-
; FinaleWait) until the boss clears $F420; then he holds up the orb (MickeyFinaleEnd, at $40
; frames left loc_00CDCA puts it at $FFDC40), which at its top puts the burst at $FFDC80 and
; sets $F421 for the boss. In co-op both players play it: Player 2 to the mirror of Mickey's
; place about the arena's middle (X $160: X $1D0), facing left, with his own orb and burst
; ($FFDCC0, $FFDD00, among the thrown items' slots as Mickey's are); the boss is released once
; both are in place, so both hold the orb up together. A player in his bubble, or dead and
; waiting for it, comes back in to play it too (as a pop, where he is or where the one who
; started it is). The port ends co-op once the fade to white is over (main.cpp).
FinaleMiddle	equ	$160
FinaleX2	equ	2*FinaleMiddle-$F0

; MickeyCheckFinale ($00B494, in place of bclr #0,($FFFFF45A).w): Z clear when this player's
; script starts now.
CoopFinaleCheck:
	movem.l	d0-d2/a1-a2,-(a7)
	bclr	#0,($FFFFF45A).w
	beq.s	.mine
	move.b	#$83,(CoopFinale).w	; begun, both players' starts to come
	clr.b	(CoopFinaleAt).w
	moveq	#0,d2			; who plays it: whoever is playing, or back from a bubble
	moveq	#0,d0
.player:
	lea	(CoopStatus).w,a1
	move.b	(a1,d0.w),d1
	beq.s	.in			; playing
	cmpi.b	#StBubble,d1
	beq.s	.pop
	cmpi.b	#StWait,d1
	bne.s	.next			; dying, out of tries: left out
	bsr.w	CoopBubbleOf		; not come yet: comes back where this one stands
	move.w	$10(a0),$10(a2)
	move.w	$14(a0),d1
	addq.w	#8,d1
	move.w	d1,$14(a2)
.pop:
	move.b	#StPop,(a1,d0.w)
.in:
	bset	d0,d2
.next:
	addq.w	#1,d0
	cmpi.w	#2,d0
	bne.s	.player
	tst.b	(P2Active).w
	bne.s	.both
	bclr	#1,d2
.both:
	move.b	d2,(CoopFinaleIn).w
.mine:
	moveq	#0,d0
	move.b	(P2Running).w,d0
	bclr	d0,(CoopFinale).w	; Z clear: his start was to come
	movem.l	(a7)+,d0-d2/a1-a2	; (movem leaves the flags)
	rts

; MickeyCheckFinale and MickeyFinaleWalk ($00B412, $00B43A, $00B4C8, in place of cmpi.w
; #$F0,$10(a0)): his place, Mickey's or the mirror of it for Player 2.
CoopFinaleX:
	tst.b	(P2Running).w
	bne.s	.p2
	cmpi.w	#$F0,$10(a0)
	rts
.p2:
	cmpi.w	#FinaleX2,$10(a0)
	rts

; MickeyFinaleWalk at his place ($00B422, in place of move.b #1,($FFFFF420).w): the boss is
; released once every player in the script is at his place.
CoopFinaleArrive:
	movem.l	d0-d1,-(a7)
	moveq	#0,d0
	move.b	(P2Running).w,d0
	bset	d0,(CoopFinaleAt).w
	move.b	(CoopFinaleIn).w,d1
	move.b	(CoopFinaleAt).w,d0
	and.b	d1,d0
	cmp.b	d1,d0
	bne.s	.wait
	bset	#6,(CoopFinale).w
	bne.s	.wait			; (once)
	move.b	#1,($FFFFF420).w
.wait:
	movem.l	(a7)+,d0-d1
	rts

; MickeyFinaleWait ($00B460, in place of tst.b ($FFFFF420).w and bne.s to its jmp
; AnimateObject at $00B478): he waits while the boss isn't released yet, too (the first in
; place waits for the other).
CoopFinaleWaiting:
	tst.b	($FFFFF420).w
	bne.s	.wait
	btst	#6,(CoopFinale).w
	beq.s	.wait
	rts
.wait:
	addq.l	#4,a7
	jmp	($00B478).l

; MickeyFinaleEnd's orb (loc_00CDCA, in place of lea ($FFFFDC40).w,a1 and moveq #6,d0; the
; move.w #$FC,$10(a1) after it is stepped over): Mickey's at $FFDC40, X $FC; Player 2's at
; $FFDCC0, the mirror of it, marked ($2E) for its burst.
CoopOrb:
	addq.l	#6,(a7)
	tst.b	(P2Running).w
	bne.s	.p2
	lea	($FFFFDC40).w,a1
	move.w	#$FC,$10(a1)
	clr.b	$2E(a1)
	moveq	#6,d0
	rts
.p2:
	lea	($FFFFDCC0).w,a1
	move.w	#2*FinaleMiddle-$FC,$10(a1)
	move.b	#1,$2E(a1)
	moveq	#6,d0
	rts

; The orb's burst (loc_00CDB0, in place of lea ($FFFFDC80).w,a1 and moveq #8,d0): Player 2's
; orb's at $FFDD00.
CoopOrbBurst:
	lea	($FFFFDC80).w,a1
	tst.b	$2E(a0)
	beq.s	.mine
	lea	($FFFFDD00).w,a1
.mine:
	moveq	#8,d0
	rts

; DrawObject ($001B42, in place of bclr #$F,d0 and adda.w d0,a1; the bne.s after it takes the
; frame's flip): Player 2 in his place (MickeyFinaleWait, MickeyFinaleEnd: states $31, $32)
; faces left, the mirror of Mickey's pose (the orb's animation, $4C, has no left-facing twin).
CoopDrawFlip:
	cmpa.w	#P2State&$FFFF,a0
	bne.s	.frame
	cmpi.b	#$31,3(a0)
	blo.s	.frame
	cmpi.b	#$32,3(a0)
	bhi.s	.frame
	bchg	#$F,d0
.frame:
	bclr	#$F,d0
	adda.w	d0,a1
	rts

; The boss gone ($0109A8, in place of ori.b #1,($FFFFDC82).w; Mickey's orb is marked just before):
; his burst, and Player 2's orb and burst, deleted too.
CoopOrbsGone:
	ori.b	#1,($FFFFDC82).w
	cmpi.w	#$6C,($FFFFDCC0).w
	bne.s	.burst
	ori.b	#1,($FFFFDCC2).w
.burst:
	cmpi.w	#$6C,($FFFFDD00).w
	bne.s	.done
	ori.b	#1,($FFFFDD02).w
.done:
	rts

; The finale's orbs and bursts (loc_00CDE8, in place of move.w #$A580,$4(a1)): their tiles are
; graphics set $2B's first 49, which every level start puts at VRAM $B000 (tile $580), where
; co-op keeps Player 2's frame (tiles $580-$59F) and the bubble's tiles ($5A0-$5BB): drawn from
; there they were a jumble. The port puts those 49 tiles in the co-op image (FinaleArt); the
; first orb copies them to tiles $231-$261 (VRAM $4620), blank and unused in the final boss's
; arena from its start to the ending, and they are drawn from there.
CoopOrbTiles:
	move.w	#$A231,4(a1)
	bset	#5,(CoopFinale).w
	bne.s	.done
	movem.l	d1/a2-a3,-(a7)
	move.w	sr,-(a7)
	move.w	#$2700,sr
	lea	(VDP_Ctrl).l,a2
	lea	(VDP_Data).l,a3
	move.l	#$46200001,(a2)		; write VRAM $4620
	lea	(FinaleArt).l,a2
	move.w	#49*8-1,d1
.tile:
	move.l	(a2)+,(a3)
	dbra	d1,.tile
	move.w	(a7)+,sr
	movem.l	(a7)+,d1/a2-a3
.done:
	rts
