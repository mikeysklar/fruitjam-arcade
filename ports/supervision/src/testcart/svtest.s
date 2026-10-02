; SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
; SPDX-License-Identifier: MIT
;
; svtest: a 64 KB Supervision test cart for the emulator's host tests and the
; board. Build: ca65 svtest.s && ld65 -C svtest.cfg -o svtest.sv svtest.o
;
; Screen, top to bottom:
;   lines 0-7     four shade bars, lightest (0) to darkest (3)
;   lines 8-15    bank check: bank b's first byte ($B0+b) read through the
;                 $8000 window, one 4-pixel block per correct bank
;   lines 20-49   VRAM DMA: 1440 bytes copied from bank 1, a diagonal
;                 checker pattern
;   lines 56-...  hex counters, one row each (see RES below)
;
; Sound: square 1 (right) 440 Hz always on, square 2 (left) a 330 Hz blip
; on a length counter, a noise burst, and the audio DMA playing a 256 byte
; sawtooth from bank 2 over and over (restarted from its own IRQ).
;
; Results in WRAM for the host test (dump $0000):
RES_NMI   = $00   ; NMI count
RES_TIMER = $01   ; timer IRQ count
RES_ADMA  = $02   ; audio DMA IRQ count
RES_BANK  = $03   ; 4 bytes: what $8000 read with banks 0-3 selected
RES_PAD   = $07   ; $2020 (1 = pressed after EOR)
RES_DMAL  = $08   ; $200C read after the VRAM DMA (expected 0)
RES_DMAS  = $09   ; $2008/$2009 after the DMA (expected $8010 + 1440 = $85B0)
RES_TVAL  = $0B   ; $2023 read right after writing 200 (counting down, <= 200)
RES_OK    = $0C   ; $5A when the start-up checks all passed

ptr  = $10
ptr2 = $12
tmp  = $14

.segment "BANK0"
    .byte $B0
.segment "BANK1"
    .byte $B1
    .res 15, 0
    ; 30 lines x 48 bytes: alternating $33/$CC bytes, phase flipping every 4 lines
    .repeat 30, line
        .repeat 48, col
            .if ((line / 4) + col) & 1
                .byte $CC
            .else
                .byte $33
            .endif
        .endrepeat
    .endrepeat
.segment "BANK2"
    .byte $B2
    .res 15, 0
saw:
    .repeat 256, i
        .byte ((i * 2) & $0F) << 4 | ((i * 2 + 1) & $0F)
    .endrepeat
.segment "BANK3"
    .byte $B3

.segment "CODE"

; 3x5 hex font, one byte per row: a set pixel is shade 3, pixel 0 in bits 0-1.
.define PX(v) ((((v) >> 2) & 1) * $03 | (((v) >> 1) & 1) * $0C | ((v) & 1) * $30)
font:
    .byte PX(7), PX(5), PX(5), PX(5), PX(7)  ; 0
    .byte PX(2), PX(6), PX(2), PX(2), PX(7)  ; 1
    .byte PX(7), PX(1), PX(7), PX(4), PX(7)  ; 2
    .byte PX(7), PX(1), PX(3), PX(1), PX(7)  ; 3
    .byte PX(5), PX(5), PX(7), PX(1), PX(1)  ; 4
    .byte PX(7), PX(4), PX(7), PX(1), PX(7)  ; 5
    .byte PX(7), PX(4), PX(7), PX(5), PX(7)  ; 6
    .byte PX(7), PX(1), PX(1), PX(1), PX(1)  ; 7
    .byte PX(7), PX(5), PX(7), PX(5), PX(7)  ; 8
    .byte PX(7), PX(5), PX(7), PX(1), PX(7)  ; 9
    .byte PX(2), PX(5), PX(7), PX(5), PX(5)  ; A
    .byte PX(6), PX(5), PX(6), PX(5), PX(6)  ; B
    .byte PX(7), PX(4), PX(4), PX(4), PX(7)  ; C
    .byte PX(6), PX(5), PX(5), PX(5), PX(6)  ; D
    .byte PX(7), PX(4), PX(6), PX(4), PX(7)  ; E
    .byte PX(7), PX(4), PX(6), PX(4), PX(4)  ; F

reset:
    sei
    cld
    ldx #$FF
    txs
    lda #$A0
    sta $2000
    sta $2001
    stz $2002
    stz $2003
    lda #$08            ; display on, no interrupts yet
    sta $2026

    ldx #0              ; clear WRAM $00-$FF
:   stz $00,x
    inx
    bne :-

    stz ptr             ; clear VRAM
    lda #$40
    sta ptr+1
    ldx #$20
    lda #0
    tay
:   sta (ptr),y
    iny
    bne :-
    inc ptr+1
    dex
    bne :-

    ; shade bars: 40 bytes a line, 10 bytes per shade
    ldx #0
@bar_line:
    lda line_lo,x
    sta ptr
    lda line_hi,x
    sta ptr+1
    ldy #0
:   tya
    lsr                 ; y / 8 approximates y / 10 well enough for 4 bars
    lsr
    lsr
    cmp #4
    bcc :+
    lda #3
:   phy
    tay
    lda shade_byte,y
    ply
    sta (ptr),y
    iny
    cpy #40
    bne :--
    inx
    cpx #8
    bne @bar_line

    ; bank check
    ldx #0
:   txa
    asl
    asl
    asl
    asl
    asl
    ora #$08
    sta $2026
    lda $8000
    sta RES_BANK,x
    inx
    cpx #4
    bne :-
    ldx #0
@bank_draw:
    lda RES_BANK,x
    sec
    sbc #$B0
    sta tmp
    cpx tmp
    bne @bank_skip
    ldy #8
:   lda line_lo,y
    sta ptr
    lda line_hi,y
    sta ptr+1
    phy
    txa
    asl                 ; block column 2x
    tay
    lda #$FF
    sta (ptr),y
    ply
    iny
    cpy #16
    bne :-
@bank_skip:
    inx
    cpx #4
    bne @bank_draw

    ; VRAM DMA from bank 1: $8010 -> line 20, 90 * 16 = 1440 bytes
    lda #$28            ; bank 1, display on
    sta $2026
    lda #$10
    sta $2008
    lda #$80
    sta $2009
    lda line_lo+20
    sta $200A
    lda line_hi+20
    sta $200B
    lda #90
    sta $200C
    lda #$80
    sta $200D
    lda $200C
    sta RES_DMAL
    lda $2008
    sta RES_DMAS
    lda $2009
    sta RES_DMAS+1

    ; timer read-back
    lda #200
    sta $2023
    lda $2023
    sta RES_TVAL

    ; start-up verdict
    lda RES_DMAL
    bne @bad
    lda RES_DMAS
    cmp #$B0
    bne @bad
    lda RES_DMAS+1
    cmp #$85
    bne @bad
    lda RES_TVAL
    beq @bad
    cmp #201
    bcs @bad
    lda #$5A
    sta RES_OK
@bad:

    ; sound
    lda #<283           ; 125000 / 284 = 440 Hz
    sta $2010
    lda #>283
    sta $2011
    lda #$68            ; always on, 50 %, volume 8
    sta $2012
    lda #<378           ; 330 Hz
    sta $2014
    lda #>378
    sta $2015
    lda #$16            ; length counted, 25 %, volume 6
    sta $2016
    lda #30             ; ~0.5 s
    sta $2017
    lda #$56            ; LFSR every 512 cycles, volume 6
    sta $2028
    lda #20
    sta $2029
    lda #$1D            ; on, left, right, length counted, 15-bit
    sta $202A
    jsr start_adma

    lda #$2F            ; bank 1, display, DMA IRQ, timer IRQ, NMI; timer /256
    sta $2026
    lda #255
    sta $2023
    cli

main:
    lda $2020
    eor #$FF
    sta RES_PAD
    ldx #0              ; draw RES_NMI .. RES_OK as hex, rows from line 56
@row:
    lda $00,x
    jsr draw_hex
    inx
    cpx #13
    bne @row
    jmp main

; Draw A as two hex digits at row X (line 56 + 6X, columns 2-3).
draw_hex:
    phx
    pha
    lda row_lo,x
    sta ptr
    lda row_hi,x
    sta ptr+1
    pla
    pha
    lsr
    lsr
    lsr
    lsr
    jsr glyph
    inc ptr
    pla
    and #$0F
    jsr glyph
    plx
    rts

; 5 rows of glyph A at ptr, 48 bytes apart.
glyph:
    sta tmp
    asl
    asl
    adc tmp             ; A * 5 (carry is clear: A < 16)
    tax
    lda ptr
    sta ptr2
    lda ptr+1
    sta ptr2+1
    ldy #5
:   lda font,x
    sta (ptr2)
    lda ptr2
    clc
    adc #48
    sta ptr2
    bcc :+
    inc ptr2+1
:   inx
    dey
    bne :--
    rts

start_adma:
    lda #<saw
    sta $2018
    lda #>saw
    sta $2019
    lda #16             ; 256 bytes
    sta $201A
    lda #$2F            ; bank 2, left, right, a nibble every 2048 cycles
    sta $201B
    lda #$80
    sta $201C
    rts

nmi:
    inc RES_NMI
    rti

irq:
    pha
    lda $2027
    and #1
    beq :+
    inc RES_TIMER
    lda $2024
    lda #255
    sta $2023
:   lda $2027
    and #2
    beq :+
    inc RES_ADMA
    lda $2025
    jsr start_adma
:   pla
    rti

shade_byte:
    .byte $00, $55, $AA, $FF

line_lo:
    .repeat 64, i
        .byte <($4000 + i * 48)
    .endrepeat
line_hi:
    .repeat 64, i
        .byte >($4000 + i * 48)
    .endrepeat
row_lo:
    .repeat 13, i
        .byte <($4000 + (56 + i * 6) * 48 + 2)
    .endrepeat
row_hi:
    .repeat 13, i
        .byte >($4000 + (56 + i * 6) * 48 + 2)
    .endrepeat

.segment "VECTORS"
    .word nmi, reset, irq
