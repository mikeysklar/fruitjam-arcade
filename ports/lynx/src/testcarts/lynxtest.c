/* SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
 * SPDX-License-Identifier: MIT
 *
 * Lynx hardware test cart for the lynx emulator, built with cc65:
 *   cl65 -t lynx -C lynx-coll.cfg -O -o lynxtest.lnx lynxtest.c
 * (lynx-coll.cfg reserves the collision buffer; without it the collision
 * buffer overlaps the C stack.)
 *
 * Draws a fixed scene each frame so frames can be pinned by CRC:
 *   - TGI primitives (bars, lines, circle, text), all drawn by Suzy
 *   - sprites encoded here at run time: literal and packed data, 1x/2x
 *     scaling, h/v flip, stretch + tilt, a 1 bpp sprite with a pen palette
 *   - collision: two normal sprites overlap, the depository is printed
 *   - Suzy math: unsigned multiply, accumulate, divide, signed multiply,
 *     checked against C arithmetic, pass counts printed and stored at $00F0
 *   - joystick and switch bits printed
 *   - a square wave on audio channel 0 (volume from the joystick A button)
 */
#include <lynx.h>
#include <tgi.h>
#include <joystick.h>
#include <string.h>
#include <stdio.h>
#include <6502.h>
#include <_suzy.h>
#include <_mikey.h>

extern unsigned char _sprsys;  /* cc65's shadow of SPRSYS ($FC92) */

#define RESULT ((unsigned char *)0x00F0)

static unsigned char lit_data[256];
static unsigned char pak_data[256];
static unsigned char one_data[64];
static unsigned char box_data[64];
static unsigned char *wp;
static unsigned char bitbuf, nbits;

static void put_bits(unsigned char v, unsigned char n) {
    while (n--) {
        bitbuf = (bitbuf << 1) | ((v >> n) & 1);
        if (++nbits == 8) {
            *wp++ = bitbuf;
            nbits = 0;
            bitbuf = 0;
        }
    }
}

static unsigned char *flush_bits(void) {
    if (nbits) {
        *wp++ = bitbuf << (8 - nbits);
    }
    nbits = 0;
    bitbuf = 0;
    return wp;
}

/* Pixel p(x, y) of a 16x8 test pattern: diagonal color bands, 0 = clear. */
static unsigned char pat(unsigned char x, unsigned char y) {
    unsigned char c = (x + y) & 15;
    if (x == 0 || y == 0 || x == 15 || y == 7) return 15;
    return c ? c : 1;
}

/* Literal 4 bpp: offset byte, 16 pixels (8 bytes), one pad byte. */
static void make_literal(void) {
    unsigned char y, x;
    wp = lit_data;
    for (y = 0; y < 8; ++y) {
        unsigned char *start = wp++;
        for (x = 0; x < 16; ++x) put_bits(pat(x, y), 4);
        flush_bits();
        *wp++ = 0;
        *start = (unsigned char)(wp - start);
    }
    *wp = 0;
}

/* Packed 4 bpp: runs of equal pixels as packed packets, the rest literal. */
static void make_packed(void) {
    unsigned char y, x, n, c;
    wp = pak_data;
    for (y = 0; y < 8; ++y) {
        unsigned char *start = wp++;
        x = 0;
        while (x < 16) {
            c = pat(x, y);
            n = 1;
            while (x + n < 16 && pat(x + n, y) == c && n < 16) ++n;
            if (n >= 2) {
                put_bits(0, 1);
                put_bits(n - 1, 4);
                put_bits(c, 4);
            } else {
                put_bits(1, 1);
                put_bits(0, 4);
                put_bits(c, 4);
            }
            x += n;
        }
        put_bits(0, 5);  /* end of line */
        flush_bits();
        *start = (unsigned char)(wp - start);
    }
    *wp = 0;
}

/* 1 bpp literal 8x8 "L" shape, drawn through a pen palette. */
static void make_one(void) {
    static const unsigned char rows[8] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFE, 0xFE};
    unsigned char y;
    wp = one_data;
    for (y = 0; y < 8; ++y) {
        *wp++ = 3;
        *wp++ = rows[y];
        *wp++ = 0;
    }
    *wp = 0;
}

/* Solid 8x8 packed box of pen 1 (for the collision test). */
static void make_box(void) {
    unsigned char y;
    wp = box_data;
    for (y = 0; y < 8; ++y) {
        unsigned char *start = wp++;
        put_bits(0, 1);
        put_bits(7, 4);
        put_bits(1, 4);
        put_bits(0, 5);
        flush_bits();
        *start = (unsigned char)(wp - start);
    }
    *wp = 0;
}

static SCB_REHV_PAL s_lit = {BPP_4 | TYPE_NORMAL, REHV | LITERAL, NO_COLLIDE, 0, lit_data, 4, 30, 0x100, 0x100,
                             {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};
static SCB_REHV_PAL s_pak = {BPP_4 | TYPE_NORMAL, REHV, NO_COLLIDE, 0, pak_data, 24, 30, 0x100, 0x100,
                             {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};
static SCB_REHV_PAL s_big = {BPP_4 | TYPE_NORMAL, REHV, NO_COLLIDE, 0, pak_data, 44, 30, 0x200, 0x200,
                             {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};
static SCB_REHV_PAL s_hfl = {BPP_4 | TYPE_NORMAL | HFLIP, REHV, NO_COLLIDE, 0, pak_data, 100, 30, 0x100, 0x100,
                             {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};
static SCB_REHV_PAL s_vfl = {BPP_4 | TYPE_NORMAL | VFLIP, REHV, NO_COLLIDE, 0, pak_data, 104, 55, 0x100, 0x100,
                             {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};
static SCB_REHVST_PAL s_st = {BPP_4 | TYPE_NORMAL, REHVST, NO_COLLIDE, 0, pak_data, 120, 30, 0x100, 0x100,
                              0x0010, 0x0080, {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};
static SCB_REHV_PAL s_one = {BPP_1 | TYPE_NORMAL, REHV | LITERAL, NO_COLLIDE, 0, one_data, 146, 30, 0x100, 0x100,
                             {0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
/* Collision pair: A (number 2) drawn first, B (number 5) overlaps it. */
/* cc65's driver sets COLLOFF to -1: the depository is the byte before the SCB */
typedef struct {
    unsigned char dep;
    SCB_REHV_PAL scb;
} coll_scb;
static coll_scb c_a = {0xFF, {BPP_4 | TYPE_NORMAL, REHV, 0x02, 0, box_data, 4, 48, 0x100, 0x100,
                              {0x09, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}}};
static coll_scb c_b = {0xFF, {BPP_4 | TYPE_NORMAL, REHV, 0x05, 0, box_data, 8, 52, 0x100, 0x100,
                              {0x0E, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}}};

static unsigned char mul_ok, div_ok, smul_ok, acc_ok;
unsigned int frame;

static void math_test(void) {
    static const unsigned int a[6] = {0, 1, 255, 1234, 40000, 65535};
    static const unsigned int b[6] = {7, 65535, 255, 5678, 3, 65535};
    unsigned char i;
    unsigned long p, q;
    mul_ok = div_ok = smul_ok = acc_ok = 0;
    for (i = 0; i < 6; ++i) {
        /* Unsigned multiply: write B (D,C) then A (B,A); writing A starts it. */
        FACTOR_B = b[i];
        FACTOR_A = a[i];
        while (SUZY.sprsys & 0x80) ;
        p = ((unsigned long)PRODUCT1 << 16) | PRODUCT0;
        if (p == (unsigned long)a[i] * b[i]) ++mul_ok;
        /* Divide: EFGH / NP, writing E starts it. */
        if (b[i]) {
            q = (unsigned long)a[i] * 3 + 11;
            DIVISOR = b[i];
            DIVIDEND0 = (unsigned int)q;
            DIVIDEND1 = (unsigned int)(q >> 16);
            while (SUZY.sprsys & 0x80) ;
            if ((((unsigned long)QUOTIENT1 << 16) | QUOTIENT0) == q / b[i] &&
                (((unsigned long)REMAINDER1 << 16) | REMAINDER0) == q % b[i]) ++div_ok;
        }
    }
    /* Signed multiply: -300 * 45 = -13500, 123 * -2 = -246, -7 * -9 = 63. */
    SUZY.sprsys = _sprsys | 0x80;
    FACTOR_B = 0xFED4; FACTOR_A = 45;
    p = ((unsigned long)PRODUCT1 << 16) | PRODUCT0;
    if ((long)p == -13500L) ++smul_ok;
    FACTOR_B = 123; FACTOR_A = 0xFFFE;
    p = ((unsigned long)PRODUCT1 << 16) | PRODUCT0;
    if ((long)p == -246L) ++smul_ok;
    FACTOR_B = 0xFFF9; FACTOR_A = 0xFFF7;
    p = ((unsigned long)PRODUCT1 << 16) | PRODUCT0;
    if ((long)p == 63L) ++smul_ok;
    /* Accumulate: JKLM += EFGH for three products. */
    SUZY.sprsys = (_sprsys & 0x3F) | 0x40;
    REMAINDER0 = 0;
    REMAINDER1 = 0;
    FACTOR_B = 1000; FACTOR_A = 1000;
    FACTOR_B = 2000; FACTOR_A = 3;
    FACTOR_B = 7; FACTOR_A = 7;
    p = ((unsigned long)REMAINDER1 << 16) | REMAINDER0;
    if (p == 1000000UL + 6000UL + 49UL) ++acc_ok;
    SUZY.sprsys = _sprsys;
    RESULT[0] = mul_ok;
    RESULT[1] = div_ok;
    RESULT[2] = smul_ok;
    RESULT[3] = acc_ok;
    RESULT[4] = 0xA5;
}

static void sound_on(unsigned char vol) {
    MIKEY.channel_a.volume = vol;
    MIKEY.channel_a.feedback = 0x01;   /* tap 0: the LFSR toggles, a square wave */
    MIKEY.channel_a.dac = 0;
    MIKEY.channel_a.shiftlo = 0x01;
    MIKEY.channel_a.reload = 142;     /* 143 x 4 us = 572 us per toggle: an 874 Hz square */
    MIKEY.channel_a.control = 0x18 | 2;
    MIKEY.mstereo = 0;
}

void main(void) {
    char buf[24];

    unsigned char joy, coll;

    make_literal();
    make_packed();
    make_one();
    make_box();
    s_lit.next = (char *)&s_pak;
    s_pak.next = (char *)&s_big;
    s_big.next = (char *)&s_hfl;
    s_hfl.next = (char *)&s_vfl;
    s_vfl.next = (char *)&s_st;
    s_st.next = (char *)&s_one;

    tgi_install(tgi_static_stddrv);
    joy_install(joy_static_stddrv);
    tgi_init();
    CLI();
    while (tgi_busy()) ;
    tgi_setcollisiondetection(1);
    math_test();
    sound_on(0x30);

    for (;;) {
        joy = joy_read(JOY_1);
        sound_on((joy & JOY_BTN_1_MASK) ? 0x60 : 0x30);
        tgi_setbgcolor(COLOR_BLUE);
        tgi_clear();
        tgi_setcolor(COLOR_WHITE);
        tgi_outtextxy(4, 2, "LYNX TEST");
        tgi_setcolor(COLOR_YELLOW);
        tgi_bar(100, 2, 156, 9);
        tgi_setcolor(COLOR_RED);
        tgi_line(0, 12, 159, 12);
        tgi_line(0, 101, 159, 70);
        tgi_setcolor(COLOR_GREEN);
        tgi_circle(140, 85, 12);

        /* the sprite row */
        s_st.stretch = 0x0010;
        s_st.hsize = 0x100;
        tgi_sprite(&s_lit);

        /* collision: A then B over it; B's depository gets A's number */
        c_b.scb.next = 0;
        c_a.scb.next = (char *)&c_b.scb;
        tgi_sprite(&c_a.scb);
        coll = (c_a.dep << 4) | c_b.dep;

        tgi_setcolor(COLOR_WHITE);
        sprintf(buf, "MUL%u DIV%u S%u A%u", mul_ok, div_ok, smul_ok, acc_ok);
        tgi_outtextxy(4, 64, buf);
        sprintf(buf, "JOY %02X SW %02X", joy, SUZY.switches);
        tgi_outtextxy(4, 74, buf);
        sprintf(buf, "F %u", frame);
        tgi_outtextxy(4, 84, buf);
        sprintf(buf, "C %02X", coll);
        tgi_outtextxy(60, 48, buf);
        tgi_updatedisplay();
        while (tgi_busy()) ;
        ++frame;
    }
}
