// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// 65C02 / 65SC02 instruction set as a code fragment, extended from the NMOS
// core in ../../atari2600/src/cpu6502.h. The includer defines, before
// including this inside its run loop:
//
//   RD(a)    bus read, one cycle        WR(a, v)  bus write, one cycle
//   TICK()   one internal cycle with no bus access modelled
//   WAIT()   WAI: stop until an interrupt   STOP()  STP: stop for good
//
// and has these locals in scope: uint8_t A, X, Y, S; uint16_t PC; unsigned
// C_, V_, D_, I_ (0 or 1), N_ (bit 7 is N), Z_ (Z flag is Z_ == 0).
//
// Define CPU_ROCKWELL to 1 for the Rockwell/WDC bit opcodes (RMB, SMB, BBR,
// BBS). With it 0 those 32 opcodes are one byte, one cycle NOPs, which is
// what the Lynx's VTI 65SC02 does. WAI and STP are always present.
//
// Decimal mode follows the 65C02: N and Z come from the BCD result, and it
// takes one extra cycle. Undefined opcodes are NOPs of the documented sizes.

#ifndef CPU_ROCKWELL
#define CPU_ROCKWELL 0
#endif

#define FETCH() RD(PC++)
#define SETNZ(v) (N_ = Z_ = (uint8_t)(v))
#define GETP(b) ((N_ & 0x80) | (V_ << 6) | 0x20 | (b) | (D_ << 3) | (I_ << 2) | (Z_ ? 0 : 2) | C_)
#define SETP(p) do { unsigned p_ = (p); N_ = p_ & 0x80; V_ = (p_ >> 6) & 1; D_ = (p_ >> 3) & 1; \
                     I_ = (p_ >> 2) & 1; Z_ = (p_ & 2) ? 0 : 1; C_ = p_ & 1; } while (0)
#define PUSH(v) do { WR(0x100 | S, (v)); S--; } while (0)
#define PULL() (S++, RD(0x100 | S))

// Addressing modes: leave the effective address in ea.
#define AM_ZP() ea = FETCH()
#define AM_ZPX() do { ea = (uint8_t)(FETCH() + X); TICK(); } while (0)
#define AM_ZPY() do { ea = (uint8_t)(FETCH() + Y); TICK(); } while (0)
#define AM_ABS() do { unsigned lo_ = FETCH(); ea = lo_ | (FETCH() << 8); } while (0)
#define AM_ABI_R(r) do { AM_ABS(); if ((ea & 0xFF) + (r) > 0xFF) { TICK(); } ea = (ea + (r)) & 0xFFFF; } while (0)
#define AM_ABI_W(r) do { AM_ABS(); TICK(); ea = (ea + (r)) & 0xFFFF; } while (0)
#define AM_IZX() do { unsigned z_ = (uint8_t)(FETCH() + X); TICK(); unsigned lo_ = RD(z_); \
                      ea = lo_ | (RD((z_ + 1) & 0xFF) << 8); } while (0)
#define AM_IZY_(w) do { unsigned z_ = FETCH(); unsigned lo_ = RD(z_); ea = lo_ | (RD((z_ + 1) & 0xFF) << 8); \
                        if ((w) || (ea & 0xFF) + Y > 0xFF) { TICK(); } ea = (ea + Y) & 0xFFFF; } while (0)
#define AM_IZY_R() AM_IZY_(0)
#define AM_IZY_W() AM_IZY_(1)
#define AM_IZP() do { unsigned z_ = FETCH(); unsigned lo_ = RD(z_); ea = lo_ | (RD((z_ + 1) & 0xFF) << 8); } while (0)

// ALU
#define OP_ORA(v) SETNZ(A |= (v))
#define OP_AND(v) SETNZ(A &= (v))
#define OP_EOR(v) SETNZ(A ^= (v))
// Decimal ADC/SBC as in Bruce Clark's 65C02 sequences (6502.org tutorial,
// "Decimal Mode", appendix A); V is from the signed intermediate for ADC and
// binary for SBC.
#define OP_ADC(v) do { unsigned m_ = (v); \
        if (D_) { \
            int al_ = (A & 0x0F) + (m_ & 0x0F) + C_; \
            if (al_ >= 0x0A) al_ = ((al_ + 0x06) & 0x0F) + 0x10; \
            int s_ = (int)(int8_t)(A & 0xF0) + (int)(int8_t)(m_ & 0xF0) + al_; \
            V_ = (s_ < -128 || s_ > 127); \
            int t_ = (A & 0xF0) + (m_ & 0xF0) + al_; \
            if (t_ >= 0xA0) t_ += 0x60; \
            C_ = t_ >= 0x100; \
            SETNZ(A = (uint8_t)t_); \
            TICK(); \
        } else { \
            unsigned t_ = A + m_ + C_; \
            V_ = ((~(A ^ m_) & (A ^ t_)) >> 7) & 1; \
            C_ = t_ >> 8; \
            SETNZ(A = (uint8_t)t_); \
        } } while (0)
#define OP_SBC(v) do { unsigned m_ = (v); unsigned t_ = A - m_ - (C_ ^ 1); \
        V_ = (((A ^ m_) & (A ^ t_)) >> 7) & 1; \
        if (D_) { \
            int al_ = (A & 0x0F) - (m_ & 0x0F) - (int)(C_ ^ 1); \
            int r_ = (int)A - (int)m_ - (int)(C_ ^ 1); \
            if (r_ < 0) r_ -= 0x60; \
            if (al_ < 0) r_ -= 0x06; \
            C_ = !(t_ & 0x100); \
            SETNZ(A = (uint8_t)r_); \
            TICK(); \
        } else { \
            C_ = !(t_ & 0x100); \
            SETNZ(A = (uint8_t)t_); \
        } } while (0)
#define OP_CMP(r, v) do { unsigned t_ = (r) - (v); C_ = !(t_ & 0x100); SETNZ(t_); } while (0)
#define OP_ASL(v) (C_ = (v) >> 7, v = (uint8_t)((v) << 1), SETNZ(v))
#define OP_LSR(v) (C_ = (v) & 1, v = (v) >> 1, SETNZ(v))
#define OP_ROL(v) do { unsigned c_ = C_; C_ = (v) >> 7; v = (uint8_t)(((v) << 1) | c_); SETNZ(v); } while (0)
#define OP_ROR(v) do { unsigned c_ = C_; C_ = (v) & 1; v = (uint8_t)(((v) >> 1) | (c_ << 7)); SETNZ(v); } while (0)
#define OP_BIT(v) (N_ = (v), V_ = ((v) >> 6) & 1, Z_ = A & (v))

// Read-modify-write through memory: read, internal cycle, write.
#define RMW(am, body) do { am; uint8_t v = RD(ea); TICK(); body; WR(ea, v); } while (0)
#define RDOP(am, body) do { am; uint8_t v = RD(ea); body; } while (0)
#define BRANCH(cond) do { int8_t o_ = (int8_t)FETCH(); if (cond) { TICK(); \
        uint16_t n_ = (uint16_t)(PC + o_); if ((n_ ^ PC) & 0xFF00) { TICK(); } PC = n_; } } while (0)

// One opcode group across the eight classic addressing modes plus (zp).
#define GROUP_R(b, body) \
    case b + 0x01: RDOP(AM_IZX(), body); break; \
    case b + 0x05: RDOP(AM_ZP(), body); break; \
    case b + 0x09: { uint8_t v = FETCH(); body; } break; \
    case b + 0x0D: RDOP(AM_ABS(), body); break; \
    case b + 0x11: RDOP(AM_IZY_R(), body); break; \
    case b + 0x12: RDOP(AM_IZP(), body); break; \
    case b + 0x15: RDOP(AM_ZPX(), body); break; \
    case b + 0x19: RDOP(AM_ABI_R(Y), body); break; \
    case b + 0x1D: RDOP(AM_ABI_R(X), body); break;
// Shift group: abs,X only takes the extra cycle on a page cross (65C02).
#define GROUP_S(b, body) \
    case b + 0x06: RMW(AM_ZP(), body); break; \
    case b + 0x0E: RMW(AM_ABS(), body); break; \
    case b + 0x16: RMW(AM_ZPX(), body); break; \
    case b + 0x1E: RMW(AM_ABI_R(X), body); break;
// INC/DEC: abs,X always 7 cycles.
#define GROUP_ID(b, body) \
    case b + 0x06: RMW(AM_ZP(), body); break; \
    case b + 0x0E: RMW(AM_ABS(), body); break; \
    case b + 0x16: RMW(AM_ZPX(), body); break; \
    case b + 0x1E: RMW(AM_ABI_W(X), body); break;

#if CPU_ROCKWELL
#define RMB_SMB(n) \
    case 0x07 + (n) * 0x10: RMW(AM_ZP(), v &= ~(1 << (n))); break; \
    case 0x87 + (n) * 0x10: RMW(AM_ZP(), v |= 1 << (n)); break; \
    case 0x0F + (n) * 0x10: { AM_ZP(); uint8_t v = RD(ea); TICK(); BRANCH(!(v & (1 << (n)))); } break; \
    case 0x8F + (n) * 0x10: { AM_ZP(); uint8_t v = RD(ea); TICK(); BRANCH(v & (1 << (n))); } break;
#endif

{
    unsigned ea;
    uint8_t op = FETCH();
    switch (op) {
        GROUP_R(0x00, OP_ORA(v))
        GROUP_R(0x20, OP_AND(v))
        GROUP_R(0x40, OP_EOR(v))
        GROUP_R(0x60, OP_ADC(v))
        GROUP_R(0xC0, OP_CMP(A, v))
        GROUP_R(0xE0, OP_SBC(v))
        // LDA
        case 0xA1: RDOP(AM_IZX(), SETNZ(A = v)); break;
        case 0xA5: RDOP(AM_ZP(), SETNZ(A = v)); break;
        case 0xA9: SETNZ(A = FETCH()); break;
        case 0xAD: RDOP(AM_ABS(), SETNZ(A = v)); break;
        case 0xB1: RDOP(AM_IZY_R(), SETNZ(A = v)); break;
        case 0xB2: RDOP(AM_IZP(), SETNZ(A = v)); break;
        case 0xB5: RDOP(AM_ZPX(), SETNZ(A = v)); break;
        case 0xB9: RDOP(AM_ABI_R(Y), SETNZ(A = v)); break;
        case 0xBD: RDOP(AM_ABI_R(X), SETNZ(A = v)); break;
        // STA
        case 0x81: AM_IZX(); WR(ea, A); break;
        case 0x85: AM_ZP(); WR(ea, A); break;
        case 0x8D: AM_ABS(); WR(ea, A); break;
        case 0x91: AM_IZY_W(); WR(ea, A); break;
        case 0x92: AM_IZP(); WR(ea, A); break;
        case 0x95: AM_ZPX(); WR(ea, A); break;
        case 0x99: AM_ABI_W(Y); WR(ea, A); break;
        case 0x9D: AM_ABI_W(X); WR(ea, A); break;
        // STZ
        case 0x64: AM_ZP(); WR(ea, 0); break;
        case 0x74: AM_ZPX(); WR(ea, 0); break;
        case 0x9C: AM_ABS(); WR(ea, 0); break;
        case 0x9E: AM_ABI_W(X); WR(ea, 0); break;
        // LDX / LDY / STX / STY
        case 0xA2: SETNZ(X = FETCH()); break;
        case 0xA6: RDOP(AM_ZP(), SETNZ(X = v)); break;
        case 0xAE: RDOP(AM_ABS(), SETNZ(X = v)); break;
        case 0xB6: RDOP(AM_ZPY(), SETNZ(X = v)); break;
        case 0xBE: RDOP(AM_ABI_R(Y), SETNZ(X = v)); break;
        case 0xA0: SETNZ(Y = FETCH()); break;
        case 0xA4: RDOP(AM_ZP(), SETNZ(Y = v)); break;
        case 0xAC: RDOP(AM_ABS(), SETNZ(Y = v)); break;
        case 0xB4: RDOP(AM_ZPX(), SETNZ(Y = v)); break;
        case 0xBC: RDOP(AM_ABI_R(X), SETNZ(Y = v)); break;
        case 0x86: AM_ZP(); WR(ea, X); break;
        case 0x8E: AM_ABS(); WR(ea, X); break;
        case 0x96: AM_ZPY(); WR(ea, X); break;
        case 0x84: AM_ZP(); WR(ea, Y); break;
        case 0x8C: AM_ABS(); WR(ea, Y); break;
        case 0x94: AM_ZPX(); WR(ea, Y); break;
        // CPX / CPY / BIT
        case 0xE0: OP_CMP(X, FETCH()); break;
        case 0xE4: RDOP(AM_ZP(), OP_CMP(X, v)); break;
        case 0xEC: RDOP(AM_ABS(), OP_CMP(X, v)); break;
        case 0xC0: OP_CMP(Y, FETCH()); break;
        case 0xC4: RDOP(AM_ZP(), OP_CMP(Y, v)); break;
        case 0xCC: RDOP(AM_ABS(), OP_CMP(Y, v)); break;
        case 0x24: RDOP(AM_ZP(), OP_BIT(v)); break;
        case 0x2C: RDOP(AM_ABS(), OP_BIT(v)); break;
        case 0x34: RDOP(AM_ZPX(), OP_BIT(v)); break;
        case 0x3C: RDOP(AM_ABI_R(X), OP_BIT(v)); break;
        case 0x89: Z_ = A & FETCH(); break;  // BIT #imm only sets Z
        // TSB / TRB
        case 0x04: RMW(AM_ZP(), (Z_ = A & v, v |= A)); break;
        case 0x0C: RMW(AM_ABS(), (Z_ = A & v, v |= A)); break;
        case 0x14: RMW(AM_ZP(), (Z_ = A & v, v &= ~A)); break;
        case 0x1C: RMW(AM_ABS(), (Z_ = A & v, v &= ~A)); break;
        // shifts, INC, DEC
        GROUP_S(0x00, OP_ASL(v))
        GROUP_S(0x20, OP_ROL(v))
        GROUP_S(0x40, OP_LSR(v))
        GROUP_S(0x60, OP_ROR(v))
        GROUP_ID(0xC0, SETNZ(--v))
        GROUP_ID(0xE0, SETNZ(++v))
        case 0x0A: TICK(); OP_ASL(A); break;
        case 0x2A: TICK(); OP_ROL(A); break;
        case 0x4A: TICK(); OP_LSR(A); break;
        case 0x6A: TICK(); OP_ROR(A); break;
        case 0x1A: TICK(); SETNZ(++A); break;
        case 0x3A: TICK(); SETNZ(--A); break;
        // register ops
        case 0xAA: TICK(); SETNZ(X = A); break;
        case 0x8A: TICK(); SETNZ(A = X); break;
        case 0xA8: TICK(); SETNZ(Y = A); break;
        case 0x98: TICK(); SETNZ(A = Y); break;
        case 0xBA: TICK(); SETNZ(X = S); break;
        case 0x9A: TICK(); S = X; break;
        case 0xE8: TICK(); SETNZ(++X); break;
        case 0xCA: TICK(); SETNZ(--X); break;
        case 0xC8: TICK(); SETNZ(++Y); break;
        case 0x88: TICK(); SETNZ(--Y); break;
        // flags
        case 0x18: TICK(); C_ = 0; break;
        case 0x38: TICK(); C_ = 1; break;
        case 0x58: TICK(); I_ = 0; break;
        case 0x78: TICK(); I_ = 1; break;
        case 0xB8: TICK(); V_ = 0; break;
        case 0xD8: TICK(); D_ = 0; break;
        case 0xF8: TICK(); D_ = 1; break;
        // branches
        case 0x10: BRANCH(!(N_ & 0x80)); break;
        case 0x30: BRANCH(N_ & 0x80); break;
        case 0x50: BRANCH(!V_); break;
        case 0x70: BRANCH(V_); break;
        case 0x90: BRANCH(!C_); break;
        case 0xB0: BRANCH(C_); break;
        case 0xD0: BRANCH(Z_); break;
        case 0xF0: BRANCH(!Z_); break;
        case 0x80: BRANCH(1); break;  // BRA
        // jumps and stack
        case 0x4C: AM_ABS(); PC = ea; break;
        case 0x6C: {  // JMP (abs), no page wrap bug
            AM_ABS();
            TICK();
            unsigned lo = RD(ea);
            PC = lo | (RD((ea + 1) & 0xFFFF) << 8);
        } break;
        case 0x7C: {  // JMP (abs,X)
            AM_ABS();
            TICK();
            ea = (ea + X) & 0xFFFF;
            unsigned lo = RD(ea);
            PC = lo | (RD((ea + 1) & 0xFFFF) << 8);
        } break;
        case 0x20: {
            unsigned lo = FETCH();
            TICK();
            PUSH(PC >> 8);
            PUSH(PC & 0xFF);
            PC = lo | (RD(PC) << 8);
        } break;
        case 0x60: {
            TICK();
            TICK();
            unsigned lo = PULL();
            PC = lo | (PULL() << 8);
            PC++;
            TICK();
        } break;
        case 0x40: {
            TICK();
            TICK();
            SETP(PULL());
            unsigned lo = PULL();
            PC = lo | (PULL() << 8);
        } break;
        case 0x00: {
            FETCH();
            PUSH(PC >> 8);
            PUSH(PC & 0xFF);
            PUSH(GETP(0x10));
            I_ = 1;
            D_ = 0;
            unsigned lo = RD(0xFFFE);
            PC = lo | (RD(0xFFFF) << 8);
        } break;
        case 0x48: TICK(); PUSH(A); break;
        case 0xDA: TICK(); PUSH(X); break;
        case 0x5A: TICK(); PUSH(Y); break;
        case 0x08: TICK(); PUSH(GETP(0x10)); break;
        case 0x68: TICK(); TICK(); SETNZ(A = PULL()); break;
        case 0xFA: TICK(); TICK(); SETNZ(X = PULL()); break;
        case 0x7A: TICK(); TICK(); SETNZ(Y = PULL()); break;
        case 0x28: TICK(); TICK(); SETP(PULL()); break;
        case 0xEA: TICK(); break;
        case 0xCB: TICK(); TICK(); WAIT(); break;  // WAI
        case 0xDB: TICK(); TICK(); STOP(); break;  // STP

#if CPU_ROCKWELL
        RMB_SMB(0) RMB_SMB(1) RMB_SMB(2) RMB_SMB(3)
        RMB_SMB(4) RMB_SMB(5) RMB_SMB(6) RMB_SMB(7)
#endif

        // ---- undefined opcodes are NOPs ----
        case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xC2: case 0xE2:
            FETCH(); break;
        case 0x44: RDOP(AM_ZP(), (void)v); break;
        case 0x54: case 0xD4: case 0xF4: RDOP(AM_ZPX(), (void)v); break;
        case 0x5C: AM_ABS(); TICK(); TICK(); TICK(); TICK(); TICK(); break;
        case 0xDC: case 0xFC: RDOP(AM_ABS(), (void)v); break;
        default:  // $x3, $xB, and $x7/$xF without CPU_ROCKWELL: 1 byte, 1 cycle
            break;
    }
}

#undef FETCH
#undef SETNZ
#undef GETP
#undef SETP
#undef PUSH
#undef PULL
#undef AM_ZP
#undef AM_ZPX
#undef AM_ZPY
#undef AM_ABS
#undef AM_ABI_R
#undef AM_ABI_W
#undef AM_IZX
#undef AM_IZY_
#undef AM_IZY_R
#undef AM_IZY_W
#undef AM_IZP
#undef OP_ORA
#undef OP_AND
#undef OP_EOR
#undef OP_ADC
#undef OP_SBC
#undef OP_CMP
#undef OP_ASL
#undef OP_LSR
#undef OP_ROL
#undef OP_ROR
#undef OP_BIT
#undef RMW
#undef RDOP
#undef BRANCH
#undef GROUP_R
#undef GROUP_S
#undef GROUP_ID
#undef RMB_SMB
