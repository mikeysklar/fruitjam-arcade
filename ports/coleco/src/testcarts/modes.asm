; SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
; SPDX-License-Identifier: MIT
;
; VDP mode test cart for the host harness (assemble with gasm80).
; Frames 0-119: Text mode showing the BIOS font. Then Multicolor blocks.
; No BIOS calls; it reads the font pointer at $006C like CVBasic does.

	org $8000
	db $55,$aa
	dw 0,0,0,0
	dw start
	jp 0
	jp 0
	jp 0
	jp 0
	jp 0
	jp 0
	jp 0
	jp nmi

frames:	equ $7000

start:
	di
	ld sp,$7400
	xor a
	ld (frames),a
	ld hl,text_regs
	call set_regs
	; font: 768 bytes from ($006C)-128 to VRAM $0100
	ld de,$0100
	call vram_write_addr
	ld hl,($006c)
	ld de,-128
	add hl,de
	ld bc,768
	call copy_out
	; name table $0800: 960 chars cycling $20-$7F
	ld de,$0800
	call vram_write_addr
	ld bc,960
	ld e,$20
.1:	ld a,e
	out ($be),a
	inc e
	bit 7,e
	jr z,.2
	ld e,$20
.2:	dec bc
	ld a,b
	or c
	jr nz,.1
	in a,($bf)
	ld a,$f0		; display on, NMI on, text
	out ($bf),a
	ld a,$81
	out ($bf),a
.3:	ld a,(frames)
	cp 120
	jr c,.3
	; multicolor: patterns at $0000 = 0..255 repeating, names at $0800
	ld hl,mc_regs
	call set_regs
	ld de,$0000
	call vram_write_addr
	ld bc,2048
	ld e,0
.4:	ld a,e
	out ($be),a
	inc e
	dec bc
	ld a,b
	or c
	jr nz,.4
	ld de,$0800
	call vram_write_addr
	ld bc,768
	ld e,0
.5:	ld a,e
	out ($be),a
	inc e
	dec bc
	ld a,b
	or c
	jr nz,.5
	in a,($bf)
	ld a,$e8		; display on, NMI on, multicolor
	out ($bf),a
	ld a,$81
	out ($bf),a
.6:	jr .6

nmi:
	push af
	in a,($bf)
	ld a,(frames)
	inc a
	ld (frames),a
	pop af
	retn

set_regs:
	ld b,8
	ld c,$80
.1:	ld a,(hl)
	out ($bf),a
	ld a,c
	out ($bf),a
	inc hl
	inc c
	djnz .1
	ret

vram_write_addr:
	ld a,e
	out ($bf),a
	ld a,d
	or $40
	out ($bf),a
	ret

copy_out:
	ld a,(hl)
	out ($be),a
	inc hl
	dec bc
	ld a,b
	or c
	jr nz,copy_out
	ret

text_regs:	db $00,$90,$02,$00,$00,$36,$07,$f4
mc_regs:	db $00,$88,$02,$00,$00,$36,$07,$05

	org $9fff
	db $ff
