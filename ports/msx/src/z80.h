// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Z80 interpreter, written for this project. All documented and undocumented
// opcodes, undocumented X/Y flags (bits 3 and 5) and MEMPTR (WZ), NMI, IM 0/1/2
// maskable interrupts. No bus contention or wait states.
//
// The includer defines, with a `z80_t *z` in scope:
//   Z80_RD(addr) Z80_WR(addr, val) Z80_IN(port16) Z80_OUT(port16, val)
// then includes this file and calls z80_reset() and z80_run().
#ifndef Z80_H
#define Z80_H

#include <stdint.h>

#define Z_CF 0x01
#define Z_NF 0x02
#define Z_PF 0x04
#define Z_XF 0x08
#define Z_HF 0x10
#define Z_YF 0x20
#define Z_ZF 0x40
#define Z_SF 0x80

typedef union {
    uint16_t w;
    struct {
        uint8_t l, h;  // little endian hosts only (ARM, x86)
    } b;
} z80_pair_t;

typedef struct {
    z80_pair_t bc, de, hl, ix, iy;
    z80_pair_t af_, bc_, de_, hl_;  // alternates; af_.b.h is A', af_.b.l is F'
    uint16_t sp, pc, wz;
    uint8_t a, f, i, r, r7;  // r counts M1 cycles; r7 holds bit 7 from LD R,A
    uint8_t im, iff1, iff2, halted;
    uint8_t nmi;  // edge latched, cleared when taken
    uint8_t irq;  // level; acked by the CPU only when iff1
    uint8_t ei_delay;
    uint8_t irq_vec;  // data bus value for IM 0/2 (IM 0 treats it as RST)
    int32_t cyc;  // T-states, the caller rebases it
    uint32_t instrs;
    uint8_t sz53[256];  // S, Z, Y, X of a byte
    uint8_t sz53p[256];  // plus P (even parity)
} z80_t;

#define RD(a) Z80_RD((uint16_t)(a))
#define WR(a, v) Z80_WR((uint16_t)(a), (uint8_t)(v))

static void z80_reset(z80_t *z) {
    for (int i = 0; i < 256; i++) {
        uint8_t f = (i & (Z_SF | Z_YF | Z_XF)) | (i ? 0 : Z_ZF);
        int p = i;
        p ^= p >> 4;
        p ^= p >> 2;
        p ^= p >> 1;
        z->sz53[i] = f;
        z->sz53p[i] = f | ((p & 1) ? 0 : Z_PF);
    }
    z->pc = 0;
    z->sp = 0xFFFF;
    z->a = 0xFF;
    z->f = 0xFF;
    z->i = z->r = z->r7 = 0;
    z->im = z->iff1 = z->iff2 = z->halted = 0;
    z->nmi = z->irq = z->ei_delay = 0;
    z->irq_vec = 0xFF;
    z->wz = 0;
}

static inline uint16_t rd16(z80_t *z, uint16_t a) {
    uint8_t lo = RD(a);
    return lo | (RD(a + 1) << 8);
}

static inline void wr16(z80_t *z, uint16_t a, uint16_t v) {
    WR(a, v);
    WR(a + 1, v >> 8);
}

static inline void push16(z80_t *z, uint16_t v) {
    z->sp -= 2;
    WR(z->sp + 1, v >> 8);
    WR(z->sp, v);
}

static inline uint16_t pop16(z80_t *z) {
    uint16_t v = rd16(z, z->sp);
    z->sp += 2;
    return v;
}

static inline void alu_add(z80_t *z, uint8_t v) {
    unsigned a = z->a, r = a + v;
    z->f = z->sz53[r & 0xFF] | ((r >> 8) & Z_CF) | ((a ^ v ^ r) & Z_HF) |
        (((a ^ ~v) & (a ^ r) & 0x80) >> 5);
    z->a = r;
}

static inline void alu_adc(z80_t *z, uint8_t v) {
    unsigned a = z->a, r = a + v + (z->f & Z_CF);
    z->f = z->sz53[r & 0xFF] | ((r >> 8) & Z_CF) | ((a ^ v ^ r) & Z_HF) |
        (((a ^ ~v) & (a ^ r) & 0x80) >> 5);
    z->a = r;
}

static inline void alu_sub(z80_t *z, uint8_t v) {
    unsigned a = z->a, r = a - v;
    z->f = z->sz53[r & 0xFF] | Z_NF | ((r >> 8) & Z_CF) | ((a ^ v ^ r) & Z_HF) |
        (((a ^ v) & (a ^ r) & 0x80) >> 5);
    z->a = r;
}

static inline void alu_sbc(z80_t *z, uint8_t v) {
    unsigned a = z->a, r = a - v - (z->f & Z_CF);
    z->f = z->sz53[r & 0xFF] | Z_NF | ((r >> 8) & Z_CF) | ((a ^ v ^ r) & Z_HF) |
        (((a ^ v) & (a ^ r) & 0x80) >> 5);
    z->a = r;
}

static inline void alu_cp(z80_t *z, uint8_t v) {
    unsigned a = z->a, r = a - v;
    z->f = (z->sz53[r & 0xFF] & (Z_SF | Z_ZF)) | (v & (Z_YF | Z_XF)) | Z_NF | ((r >> 8) & Z_CF) |
        ((a ^ v ^ r) & Z_HF) | (((a ^ v) & (a ^ r) & 0x80) >> 5);
}

static inline void alu_and(z80_t *z, uint8_t v) {
    z->a &= v;
    z->f = z->sz53p[z->a] | Z_HF;
}

static inline void alu_xor(z80_t *z, uint8_t v) {
    z->a ^= v;
    z->f = z->sz53p[z->a];
}

static inline void alu_or(z80_t *z, uint8_t v) {
    z->a |= v;
    z->f = z->sz53p[z->a];
}

static inline uint8_t inc8(z80_t *z, uint8_t v) {
    uint8_t r = v + 1;
    z->f = (z->f & Z_CF) | z->sz53[r] | ((r & 0x0F) ? 0 : Z_HF) | (r == 0x80 ? Z_PF : 0);
    return r;
}

static inline uint8_t dec8(z80_t *z, uint8_t v) {
    uint8_t r = v - 1;
    z->f = (z->f & Z_CF) | Z_NF | z->sz53[r] | ((r & 0x0F) == 0x0F ? Z_HF : 0) |
        (r == 0x7F ? Z_PF : 0);
    return r;
}

static inline uint16_t add16(z80_t *z, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + b;
    z->wz = a + 1;
    z->f = (z->f & (Z_SF | Z_ZF | Z_PF)) | ((r >> 16) & Z_CF) | (((a ^ b ^ r) >> 8) & Z_HF) |
        ((r >> 8) & (Z_YF | Z_XF));
    return r;
}

static inline void adc16(z80_t *z, uint16_t b) {
    uint32_t a = z->hl.w, r = a + b + (z->f & Z_CF);
    z->wz = a + 1;
    z->f = ((r >> 16) & Z_CF) | ((r >> 8) & (Z_SF | Z_YF | Z_XF)) | (((a ^ b ^ r) >> 8) & Z_HF) |
        (((a ^ ~(uint32_t)b) & (a ^ r) & 0x8000) >> 13) | ((r & 0xFFFF) ? 0 : Z_ZF);
    z->hl.w = r;
}

static inline void sbc16(z80_t *z, uint16_t b) {
    uint32_t a = z->hl.w, r = a - b - (z->f & Z_CF);
    z->wz = a + 1;
    z->f = Z_NF | ((r >> 16) & Z_CF) | ((r >> 8) & (Z_SF | Z_YF | Z_XF)) |
        (((a ^ b ^ r) >> 8) & Z_HF) | (((a ^ b) & (a ^ r) & 0x8000) >> 13) |
        ((r & 0xFFFF) ? 0 : Z_ZF);
    z->hl.w = r;
}

// CB rotate/shift group, y = 0..7: RLC RRC RL RR SLA SRA SLL SRL.
static inline uint8_t rot8(z80_t *z, int y, uint8_t v) {
    uint8_t r, c;
    switch (y) {
        case 0:
            r = (v << 1) | (v >> 7);
            c = v >> 7;
            break;
        case 1:
            r = (v >> 1) | (v << 7);
            c = v & 1;
            break;
        case 2:
            r = (v << 1) | (z->f & Z_CF);
            c = v >> 7;
            break;
        case 3:
            r = (v >> 1) | ((z->f & Z_CF) << 7);
            c = v & 1;
            break;
        case 4:
            r = v << 1;
            c = v >> 7;
            break;
        case 5:
            r = (v >> 1) | (v & 0x80);
            c = v & 1;
            break;
        case 6:
            r = (v << 1) | 1;
            c = v >> 7;
            break;
        default:
            r = v >> 1;
            c = v & 1;
            break;
    }
    z->f = z->sz53p[r] | c;
    return r;
}

// BIT n: xy supplies flag bits 3 and 5 (the operand, or WZ high for memory).
static inline void bit8(z80_t *z, int n, uint8_t v, uint8_t xy) {
    z->f = (z->f & Z_CF) | Z_HF | (z->sz53p[v & (1 << n)] & ~(Z_YF | Z_XF)) | (xy & (Z_YF | Z_XF));
}

static inline int z80_cond(z80_t *z, int cc) {
    switch (cc) {
        case 0:
            return !(z->f & Z_ZF);
        case 1:
            return z->f & Z_ZF;
        case 2:
            return !(z->f & Z_CF);
        case 3:
            return z->f & Z_CF;
        case 4:
            return !(z->f & Z_PF);
        case 5:
            return z->f & Z_PF;
        case 6:
            return !(z->f & Z_SF);
        default:
            return z->f & Z_SF;
    }
}

static inline void z80_daa(z80_t *z) {
    uint8_t a = z->a, t = 0, c = z->f & Z_CF, h;
    if ((z->f & Z_HF) || (a & 0x0F) > 9) {
        t = 0x06;
    }
    if (c || a > 0x99) {
        t |= 0x60;
        c = Z_CF;
    }
    if (z->f & Z_NF) {
        h = ((z->f & Z_HF) && (a & 0x0F) < 6) ? Z_HF : 0;
        a -= t;
    } else {
        h = (a & 0x0F) > 9 ? Z_HF : 0;
        a += t;
    }
    z->f = z->sz53p[a] | h | (z->f & Z_NF) | c;
    z->a = a;
}

static inline uint8_t z80_getr(z80_t *z) {
    return (z->r & 0x7F) | z->r7;
}

// DDCB/FDCB: operand at ad; results other than BIT also go to register op&7.
static void z80_xycb(z80_t *z, uint16_t ad) {
    uint8_t op = RD(z->pc++);
    uint8_t v = RD(ad), r;
    int y = (op >> 3) & 7;
    switch (op >> 6) {
        case 0:
            r = rot8(z, y, v);
            break;
        case 1:
            bit8(z, y, v, ad >> 8);
            z->cyc += 16;
            return;
        case 2:
            r = v & ~(1 << y);
            break;
        default:
            r = v | (1 << y);
            break;
    }
    WR(ad, r);
    switch (op & 7) {
        case 0:
            z->bc.b.h = r;
            break;
        case 1:
            z->bc.b.l = r;
            break;
        case 2:
            z->de.b.h = r;
            break;
        case 3:
            z->de.b.l = r;
            break;
        case 4:
            z->hl.b.h = r;
            break;
        case 5:
            z->hl.b.l = r;
            break;
        case 7:
            z->a = r;
            break;
    }
    z->cyc += 19;
}

static inline uint8_t *z80_ed_reg(z80_t *z, int y) {
    switch (y) {
        case 0:
            return &z->bc.b.h;
        case 1:
            return &z->bc.b.l;
        case 2:
            return &z->de.b.h;
        case 3:
            return &z->de.b.l;
        case 4:
            return &z->hl.b.h;
        case 5:
            return &z->hl.b.l;
        default:
            return &z->a;
    }
}

static inline uint16_t *z80_ed_rp(z80_t *z, int p) {
    switch (p) {
        case 0:
            return &z->bc.w;
        case 1:
            return &z->de.w;
        case 2:
            return &z->hl.w;
        default:
            return &z->sp;
    }
}

// Flags for INI/IND/OUTI/OUTD and repeats; k is the 9-bit sum from the manual.
static inline void z80_blockio_flags(z80_t *z, uint8_t v, unsigned k) {
    z->f = z->sz53[z->bc.b.h] | ((v & 0x80) ? Z_NF : 0) | (k > 255 ? (Z_HF | Z_CF) : 0) |
        (z->sz53p[(k & 7) ^ z->bc.b.h] & Z_PF);
}

static void z80_ed(z80_t *z) {
    uint8_t op = RD(z->pc++);
    z->r++;
    int y = (op >> 3) & 7, p = y >> 1;
    if (op >= 0x40 && op < 0x80) {
        switch (op & 7) {
            case 0: {  // IN r,(C); y=6 is IN F,(C)
                uint8_t v = Z80_IN(z->bc.w);
                z->wz = z->bc.w + 1;
                z->f = (z->f & Z_CF) | z->sz53p[v];
                if (y != 6) {
                    *z80_ed_reg(z, y) = v;
                }
                z->cyc += 12;
                return;
            }
            case 1:  // OUT (C),r; y=6 outputs 0
                Z80_OUT(z->bc.w, y == 6 ? 0 : *z80_ed_reg(z, y));
                z->wz = z->bc.w + 1;
                z->cyc += 12;
                return;
            case 2:
                if (y & 1) {
                    adc16(z, *z80_ed_rp(z, p));
                } else {
                    sbc16(z, *z80_ed_rp(z, p));
                }
                z->cyc += 15;
                return;
            case 3: {
                uint16_t nn = rd16(z, z->pc);
                z->pc += 2;
                if (y & 1) {
                    *z80_ed_rp(z, p) = rd16(z, nn);
                } else {
                    wr16(z, nn, *z80_ed_rp(z, p));
                }
                z->wz = nn + 1;
                z->cyc += 20;
                return;
            }
            case 4: {  // NEG
                uint8_t v = z->a;
                z->a = 0;
                alu_sub(z, v);
                z->cyc += 8;
                return;
            }
            case 5:  // RETN, RETI
                z->iff1 = z->iff2;
                z->pc = z->wz = pop16(z);
                z->cyc += 14;
                return;
            case 6:
                z->im = (y & 3) == 2 ? 1 : (y & 3) == 3 ? 2 : 0;
                z->cyc += 8;
                return;
            default:
                switch (y) {
                    case 0:
                        z->i = z->a;
                        z->cyc += 9;
                        return;
                    case 1:
                        z->r = z->a;
                        z->r7 = z->a & 0x80;
                        z->cyc += 9;
                        return;
                    case 2:
                    case 3:
                        z->a = y == 2 ? z->i : z80_getr(z);
                        z->f = (z->f & Z_CF) | z->sz53[z->a] | (z->iff2 ? Z_PF : 0);
                        z->cyc += 9;
                        return;
                    case 4: {  // RRD
                        uint8_t m = RD(z->hl.w);
                        WR(z->hl.w, (z->a << 4) | (m >> 4));
                        z->a = (z->a & 0xF0) | (m & 0x0F);
                        z->f = (z->f & Z_CF) | z->sz53p[z->a];
                        z->wz = z->hl.w + 1;
                        z->cyc += 18;
                        return;
                    }
                    case 5: {  // RLD
                        uint8_t m = RD(z->hl.w);
                        WR(z->hl.w, (m << 4) | (z->a & 0x0F));
                        z->a = (z->a & 0xF0) | (m >> 4);
                        z->f = (z->f & Z_CF) | z->sz53p[z->a];
                        z->wz = z->hl.w + 1;
                        z->cyc += 18;
                        return;
                    }
                    default:
                        z->cyc += 8;
                        return;
                }
        }
    }
    if (op < 0xA0 || op > 0xBF || (op & 7) > 3) {
        z->cyc += 8;  // undefined: 8 T-state NOP
        return;
    }
    int dec = op & 8, rep = op & 0x10;
    int d = dec ? -1 : 1;
    switch (op & 3) {
        case 0: {  // LDI LDD LDIR LDDR
            uint8_t v = RD(z->hl.w);
            WR(z->de.w, v);
            z->hl.w += d;
            z->de.w += d;
            z->bc.w--;
            unsigned n = v + z->a;
            z->f = (z->f & (Z_SF | Z_ZF | Z_CF)) | (z->bc.w ? Z_PF : 0) | (n & Z_XF) |
                ((n << 4) & Z_YF);
            if (rep && z->bc.w) {
                z->pc -= 2;
                z->wz = z->pc + 1;
                z->cyc += 21;
            } else {
                z->cyc += 16;
            }
            return;
        }
        case 1: {  // CPI CPD CPIR CPDR
            uint8_t v = RD(z->hl.w);
            unsigned r = (unsigned)z->a - v;
            z->hl.w += d;
            z->bc.w--;
            z->wz += d;
            uint8_t f = (z->f & Z_CF) | Z_NF | (z->sz53[r & 0xFF] & (Z_SF | Z_ZF)) |
                ((z->a ^ v ^ r) & Z_HF) | (z->bc.w ? Z_PF : 0);
            unsigned n = r - ((f & Z_HF) ? 1 : 0);
            z->f = f | (n & Z_XF) | ((n << 4) & Z_YF);
            if (rep && z->bc.w && !(z->f & Z_ZF)) {
                z->pc -= 2;
                z->wz = z->pc + 1;
                z->cyc += 21;
            } else {
                z->cyc += 16;
            }
            return;
        }
        case 2: {  // INI IND INIR INDR
            uint8_t v = Z80_IN(z->bc.w);
            z->wz = z->bc.w + d;
            WR(z->hl.w, v);
            z->hl.w += d;
            z->bc.b.h--;
            z80_blockio_flags(z, v, v + (uint8_t)(z->bc.b.l + d));
            break;
        }
        default: {  // OUTI OUTD OTIR OTDR
            uint8_t v = RD(z->hl.w);
            z->bc.b.h--;
            z->wz = z->bc.w + d;
            Z80_OUT(z->bc.w, v);
            z->hl.w += d;
            z80_blockio_flags(z, v, v + z->hl.b.l);
            break;
        }
    }
    if (rep && z->bc.b.h) {
        z->pc -= 2;
        z->cyc += 21;
    } else {
        z->cyc += 16;
    }
}

#include "z80_ops.inc"

static void z80_take_irq(z80_t *z) {
    z->halted = 0;
    z->iff1 = z->iff2 = 0;
    z->r++;
    switch (z->im) {
        case 2:
            push16(z, z->pc);
            z->pc = z->wz = rd16(z, (z->i << 8) | z->irq_vec);
            z->cyc += 19;
            break;
        case 1:
            push16(z, z->pc);
            z->pc = z->wz = 0x38;
            z->cyc += 13;
            break;
        default:  // IM 0 with an RST on the bus
            push16(z, z->pc);
            z->pc = z->wz = z->irq_vec & 0x38;
            z->cyc += 13;
            break;
    }
}

// Run until cyc >= until. NMI and IRQ are checked before every instruction.
static void z80_run(z80_t *z, int32_t until) {
    while (z->cyc < until) {
        if (z->nmi) {
            z->nmi = 0;
            z->halted = 0;
            z->iff1 = 0;
            z->r++;
            push16(z, z->pc);
            z->pc = z->wz = 0x66;
            z->cyc += 11;
            continue;
        }
        if (z->irq && z->iff1 && !z->ei_delay) {
            z80_take_irq(z);
            continue;
        }
        z->ei_delay = 0;
        if (z->halted) {
            // HALT runs NOPs; nothing can wake it inside this slice unless
            // an IRQ is pending with interrupts off, which never wakes it.
            int32_t n = (until - z->cyc + 3) >> 2;
            z->cyc += n << 2;
            z->r += n;
            break;
        }
        z80_step(z);
    }
}

#undef RD
#undef WR

#endif
