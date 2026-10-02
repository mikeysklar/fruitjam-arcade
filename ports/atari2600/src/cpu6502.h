// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// NMOS 6502 / 6507 instruction set as a code fragment. The includer defines,
// before including this inside its run loop:
//
//   RD(a)    bus read, one cycle        WR(a, v)  bus write, one cycle
//   TICK()   one internal cycle with no bus access modelled
//   JAM()    called on a KIL/JAM opcode
//
// and has these locals in scope: uint8_t A, X, Y, S; uint16_t PC; unsigned
// C_, V_, D_, I_ (0 or 1), N_ (bit 7 is N), Z_ (Z flag is Z_ == 0).
//
// Every bus access is one cycle, and internal cycles are TICK()s, so writes
// land on the same cycle as on hardware (what TIA timing depends on). Dummy
// reads and dummy writes are counted but not put on the bus.
//
// Decimal mode is NMOS: N, V and Z come from the binary intermediate.
// The common undocumented opcodes are here (LAX, SAX, DCP, ISB, SLO, RLA,
// SRE, RRA, ANC, ALR, ARR, SBX, the NOPs, SBC $EB); the unstable ones (XAA,
// LAS, SHA, SHX, SHY, TAS) use their usual approximations.

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

// ALU
#define OP_ORA(v) SETNZ(A |= (v))
#define OP_AND(v) SETNZ(A &= (v))
#define OP_EOR(v) SETNZ(A ^= (v))
#define OP_ADC(v) do { unsigned m_ = (v); \
        if (D_) { \
            unsigned lo_ = (A & 0x0F) + (m_ & 0x0F) + C_; \
            if (lo_ > 9) lo_ += 6; \
            unsigned hi_ = (A >> 4) + (m_ >> 4) + (lo_ > 0x0F); \
            Z_ = (uint8_t)(A + m_ + C_); \
            N_ = (hi_ << 4) & 0x80; \
            V_ = ((~(A ^ m_) & (A ^ (hi_ << 4))) >> 7) & 1; \
            if (hi_ > 9) hi_ += 6; \
            C_ = hi_ > 0x0F; \
            A = (uint8_t)((hi_ << 4) | (lo_ & 0x0F)); \
        } else { \
            unsigned t_ = A + m_ + C_; \
            V_ = ((~(A ^ m_) & (A ^ t_)) >> 7) & 1; \
            C_ = t_ >> 8; \
            SETNZ(A = (uint8_t)t_); \
        } } while (0)
#define OP_SBC(v) do { unsigned m_ = (v); unsigned t_ = A - m_ - (C_ ^ 1); \
        V_ = (((A ^ m_) & (A ^ t_)) >> 7) & 1; \
        if (D_) { \
            unsigned lo_ = (A & 0x0F) - (m_ & 0x0F) - (C_ ^ 1); \
            unsigned hi_ = (A >> 4) - (m_ >> 4); \
            if (lo_ & 0x10) { lo_ -= 6; hi_--; } \
            if (hi_ & 0x10) hi_ -= 6; \
            C_ = !(t_ & 0x100); \
            SETNZ(t_); \
            A = (uint8_t)((hi_ << 4) | (lo_ & 0x0F)); \
        } else { \
            C_ = !(t_ & 0x100); \
            SETNZ(A = (uint8_t)t_); \
        } } while (0)
#define OP_CMP(r, v) do { unsigned t_ = (r) - (v); C_ = !(t_ & 0x100); SETNZ(t_); } while (0)
#define OP_ASL(v) (C_ = (v) >> 7, v = (uint8_t)((v) << 1), SETNZ(v))
#define OP_LSR(v) (C_ = (v) & 1, v = (v) >> 1, SETNZ(v))
#define OP_ROL(v) do { unsigned c_ = C_; C_ = (v) >> 7; v = (uint8_t)(((v) << 1) | c_); SETNZ(v); } while (0)
#define OP_ROR(v) do { unsigned c_ = C_; C_ = (v) & 1; v = (uint8_t)(((v) >> 1) | (c_ << 7)); SETNZ(v); } while (0)

// Read-modify-write through memory: read, dummy write, write.
#define RMW(am, body) do { am; uint8_t v = RD(ea); TICK(); body; WR(ea, v); } while (0)
#define RDOP(am, body) do { am; uint8_t v = RD(ea); body; } while (0)
#define BRANCH(cond) do { int8_t o_ = (int8_t)FETCH(); if (cond) { TICK(); \
        uint16_t n_ = (uint16_t)(PC + o_); if ((n_ ^ PC) & 0xFF00) { TICK(); } PC = n_; } } while (0)

// One opcode group across the eight classic addressing modes.
#define GROUP_R(b, body) \
    case b + 0x01: RDOP(AM_IZX(), body); break; \
    case b + 0x05: RDOP(AM_ZP(), body); break; \
    case b + 0x09: { uint8_t v = FETCH(); body; } break; \
    case b + 0x0D: RDOP(AM_ABS(), body); break; \
    case b + 0x11: RDOP(AM_IZY_R(), body); break; \
    case b + 0x15: RDOP(AM_ZPX(), body); break; \
    case b + 0x19: RDOP(AM_ABI_R(Y), body); break; \
    case b + 0x1D: RDOP(AM_ABI_R(X), body); break;
// Undocumented RMW+ALU group (xx3, xx7, xxF, x13, x17, x1B, x1F).
#define GROUP_U(b, body) \
    case b + 0x03: RMW(AM_IZX(), body); break; \
    case b + 0x07: RMW(AM_ZP(), body); break; \
    case b + 0x0F: RMW(AM_ABS(), body); break; \
    case b + 0x13: RMW(AM_IZY_W(), body); break; \
    case b + 0x17: RMW(AM_ZPX(), body); break; \
    case b + 0x1B: RMW(AM_ABI_W(Y), body); break; \
    case b + 0x1F: RMW(AM_ABI_W(X), body); break;
// Shift/inc group: accumulator form handled separately.
#define GROUP_S(b, body) \
    case b + 0x06: RMW(AM_ZP(), body); break; \
    case b + 0x0E: RMW(AM_ABS(), body); break; \
    case b + 0x16: RMW(AM_ZPX(), body); break; \
    case b + 0x1E: RMW(AM_ABI_W(X), body); break;

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
        case 0xB5: RDOP(AM_ZPX(), SETNZ(A = v)); break;
        case 0xB9: RDOP(AM_ABI_R(Y), SETNZ(A = v)); break;
        case 0xBD: RDOP(AM_ABI_R(X), SETNZ(A = v)); break;
        // STA
        case 0x81: AM_IZX(); WR(ea, A); break;
        case 0x85: AM_ZP(); WR(ea, A); break;
        case 0x8D: AM_ABS(); WR(ea, A); break;
        case 0x91: AM_IZY_W(); WR(ea, A); break;
        case 0x95: AM_ZPX(); WR(ea, A); break;
        case 0x99: AM_ABI_W(Y); WR(ea, A); break;
        case 0x9D: AM_ABI_W(X); WR(ea, A); break;
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
        case 0x24: RDOP(AM_ZP(), (N_ = v, V_ = (v >> 6) & 1, Z_ = A & v)); break;
        case 0x2C: RDOP(AM_ABS(), (N_ = v, V_ = (v >> 6) & 1, Z_ = A & v)); break;
        // shifts, INC, DEC
        GROUP_S(0x00, OP_ASL(v))
        GROUP_S(0x20, OP_ROL(v))
        GROUP_S(0x40, OP_LSR(v))
        GROUP_S(0x60, OP_ROR(v))
        GROUP_S(0xC0, SETNZ(--v))
        GROUP_S(0xE0, SETNZ(++v))
        case 0x0A: TICK(); OP_ASL(A); break;
        case 0x2A: TICK(); OP_ROL(A); break;
        case 0x4A: TICK(); OP_LSR(A); break;
        case 0x6A: TICK(); OP_ROR(A); break;
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
        // jumps and stack
        case 0x4C: AM_ABS(); PC = ea; break;
        case 0x6C: {
            AM_ABS();
            unsigned lo = RD(ea);
            PC = lo | (RD((ea & 0xFF00) | ((ea + 1) & 0xFF)) << 8);
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
            unsigned lo = RD(0xFFFE);
            PC = lo | (RD(0xFFFF) << 8);
        } break;
        case 0x48: TICK(); PUSH(A); break;
        case 0x08: TICK(); PUSH(GETP(0x10)); break;
        case 0x68: TICK(); TICK(); SETNZ(A = PULL()); break;
        case 0x28: TICK(); TICK(); SETP(PULL()); break;
        case 0xEA: TICK(); break;

        // ---- undocumented ----
        GROUP_U(0x00, (OP_ASL(v), OP_ORA(v)))           // SLO
        GROUP_U(0x20, do { OP_ROL(v); OP_AND(v); } while (0))  // RLA
        GROUP_U(0x40, (OP_LSR(v), OP_EOR(v)))           // SRE
        GROUP_U(0x60, do { OP_ROR(v); OP_ADC(v); } while (0))  // RRA
        GROUP_U(0xC0, do { v--; OP_CMP(A, v); } while (0))     // DCP
        GROUP_U(0xE0, do { v++; OP_SBC(v); } while (0))        // ISB
        // SAX
        case 0x83: AM_IZX(); WR(ea, A & X); break;
        case 0x87: AM_ZP(); WR(ea, A & X); break;
        case 0x8F: AM_ABS(); WR(ea, A & X); break;
        case 0x97: AM_ZPY(); WR(ea, A & X); break;
        // LAX
        case 0xA3: RDOP(AM_IZX(), SETNZ(A = X = v)); break;
        case 0xA7: RDOP(AM_ZP(), SETNZ(A = X = v)); break;
        case 0xAF: RDOP(AM_ABS(), SETNZ(A = X = v)); break;
        case 0xB3: RDOP(AM_IZY_R(), SETNZ(A = X = v)); break;
        case 0xB7: RDOP(AM_ZPY(), SETNZ(A = X = v)); break;
        case 0xBF: RDOP(AM_ABI_R(Y), SETNZ(A = X = v)); break;
        case 0xAB: { uint8_t v = FETCH(); SETNZ(A = X = (A | 0xEE) & v); } break;
        // immediate oddities
        case 0x0B: case 0x2B: OP_AND(FETCH()); C_ = A >> 7; break;       // ANC
        case 0x4B: { uint8_t v = A & FETCH(); OP_LSR(v); A = v; } break;  // ALR
        case 0x6B: {                                                      // ARR
            uint8_t v = A & FETCH();
            if (D_) {
                unsigned t = (v >> 1) | (C_ << 7);
                SETNZ(t);
                V_ = ((t ^ v) >> 6) & 1;
                if ((v & 0x0F) + (v & 0x01) > 5) t = (t & 0xF0) | ((t + 6) & 0x0F);
                C_ = (v & 0xF0) + (v & 0x10) > 0x50;
                if (C_) t = (t + 0x60) & 0xFF;
                A = (uint8_t)t;
            } else {
                A = (uint8_t)((v >> 1) | (C_ << 7));
                SETNZ(A);
                C_ = (A >> 6) & 1;
                V_ = ((A >> 6) ^ (A >> 5)) & 1;
            }
        } break;
        case 0xCB: {                                                      // SBX
            unsigned t = (A & X) - FETCH();
            C_ = !(t & 0x100);
            SETNZ(X = (uint8_t)t);
        } break;
        case 0xEB: OP_SBC(FETCH()); break;
        case 0x8B: { uint8_t v = FETCH(); SETNZ(A = (A | 0xEE) & X & v); } break;  // XAA
        case 0xBB: RDOP(AM_ABI_R(Y), SETNZ(A = X = S = v & S)); break;            // LAS
        case 0x93: AM_IZY_W(); WR(ea, A & X & ((ea >> 8) + 1)); break;            // SHA
        case 0x9F: AM_ABI_W(Y); WR(ea, A & X & ((ea >> 8) + 1)); break;           // SHA
        case 0x9E: AM_ABI_W(Y); WR(ea, X & ((ea >> 8) + 1)); break;               // SHX
        case 0x9C: AM_ABI_W(X); WR(ea, Y & ((ea >> 8) + 1)); break;               // SHY
        case 0x9B: AM_ABI_W(Y); S = A & X; WR(ea, S & ((ea >> 8) + 1)); break;    // TAS
        // NOPs
        case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: TICK(); break;
        case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: FETCH(); break;
        case 0x04: case 0x44: case 0x64: RDOP(AM_ZP(), (void)v); break;
        case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: RDOP(AM_ZPX(), (void)v); break;
        case 0x0C: RDOP(AM_ABS(), (void)v); break;
        case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: RDOP(AM_ABI_R(X), (void)v); break;
        default:  // 0x02, 0x12, ... KIL/JAM
            PC--;
            JAM();
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
#undef RMW
#undef RDOP
#undef BRANCH
#undef GROUP_R
#undef GROUP_U
#undef GROUP_S
