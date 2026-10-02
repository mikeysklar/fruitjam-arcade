// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host test for cpu6502.h on a flat 64 KB bus.
//
//   host_cpu_test                     own checks: undocumented opcodes, cycles
//   host_cpu_test 6502_functional_test.bin
//                                     Klaus Dormann's functional test, loaded at
//                                     0, entered at 0x0400, passes on reaching
//                                     the trap at 0x3469.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[65536];
static uint64_t cyc;
static int jammed;

typedef struct {
    uint8_t A, X, Y, S;
    uint16_t PC;
    unsigned C_, V_, D_, I_, N_, Z_;
} regs_t;

// Run one instruction; returns cycles used.
static unsigned step(regs_t *r) {
    uint8_t A = r->A, X = r->X, Y = r->Y, S = r->S;
    uint16_t PC = r->PC;
    unsigned C_ = r->C_, V_ = r->V_, D_ = r->D_, I_ = r->I_, N_ = r->N_, Z_ = r->Z_;
    uint64_t c0 = cyc;
#define RD(a) (cyc++, mem[(uint16_t)(a)])
#define WR(a, v) (cyc++, mem[(uint16_t)(a)] = (uint8_t)(v))
#define TICK() (cyc++)
#define JAM() (jammed = 1)
#include "cpu6502.h"
#undef RD
#undef WR
#undef TICK
#undef JAM
    r->A = A; r->X = X; r->Y = Y; r->S = S; r->PC = PC;
    r->C_ = C_; r->V_ = V_; r->D_ = D_; r->I_ = I_; r->N_ = N_; r->Z_ = Z_;
    return (unsigned)(cyc - c0);
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// Load bytes at 0x0200, run n instructions from there.
static regs_t run_at(const uint8_t *code, size_t len, int n, unsigned *cycles, regs_t start) {
    memcpy(&mem[0x200], code, len);
    start.PC = 0x200;
    unsigned total = 0;
    for (int i = 0; i < n; i++) {
        total += step(&start);
    }
    if (cycles) *cycles = total;
    return start;
}

static int own_tests(void) {
    regs_t z = {0};
    z.S = 0xFF;
    unsigned cy;
    regs_t r;

    // LAX $10 ; SAX $11 (A=X=0x5A, then A&X stored)
    memset(mem, 0, sizeof mem);
    mem[0x10] = 0x5A;
    { static const uint8_t c[] = {0xA7, 0x10, 0x87, 0x11}; r = run_at(c, sizeof c, 2, &cy, z); }
    CHECK(r.A == 0x5A && r.X == 0x5A && mem[0x11] == 0x5A && cy == 6, "LAX/SAX A=%02x X=%02x m=%02x cy=%u", r.A, r.X, mem[0x11], cy);

    // DCP $20 with m=0x43, A=0x42 -> m=0x42, Z=1 C=1
    memset(mem, 0, sizeof mem);
    mem[0x20] = 0x43;
    { static const uint8_t c[] = {0xA9, 0x42, 0xC7, 0x20}; r = run_at(c, sizeof c, 2, &cy, z); }
    CHECK(mem[0x20] == 0x42 && r.Z_ == 0 && r.C_ == 1 && cy == 7, "DCP m=%02x Z_=%u C=%u cy=%u", mem[0x20], r.Z_, r.C_, cy);

    // ISB $20: m=0x0F -> 0x10, A=0x30 - 0x10 (C=1) = 0x20
    memset(mem, 0, sizeof mem);
    mem[0x20] = 0x0F;
    { static const uint8_t c[] = {0x38, 0xA9, 0x30, 0xE7, 0x20}; r = run_at(c, sizeof c, 3, &cy, z); }
    CHECK(mem[0x20] == 0x10 && r.A == 0x20 && r.C_ == 1, "ISB A=%02x m=%02x", r.A, mem[0x20]);

    // SLO $20: m=0x81 -> 0x02, C=1, A = 0x10 | 0x02
    memset(mem, 0, sizeof mem);
    mem[0x20] = 0x81;
    { static const uint8_t c[] = {0xA9, 0x10, 0x07, 0x20}; r = run_at(c, sizeof c, 2, &cy, z); }
    CHECK(mem[0x20] == 0x02 && r.A == 0x12 && r.C_ == 1, "SLO A=%02x m=%02x", r.A, mem[0x20]);

    // RRA $20: C=1, m=0x02 -> 0x81 (C=0), A = 0x01 + 0x81 = 0x82
    memset(mem, 0, sizeof mem);
    mem[0x20] = 0x02;
    { static const uint8_t c[] = {0x38, 0xA9, 0x01, 0x67, 0x20}; r = run_at(c, sizeof c, 3, &cy, z); }
    CHECK(mem[0x20] == 0x81 && r.A == 0x82, "RRA A=%02x m=%02x", r.A, mem[0x20]);

    // SBX #$03: X = (A & X) - 3 = 0x0F - 3 = 0x0C
    memset(mem, 0, sizeof mem);
    { static const uint8_t c[] = {0xA9, 0xFF, 0xA2, 0x0F, 0xCB, 0x03}; r = run_at(c, sizeof c, 3, &cy, z); }
    CHECK(r.X == 0x0C && r.C_ == 1, "SBX X=%02x", r.X);

    // ANC #$80 sets C from bit 7; ALR #$03 with A=0xFF -> 0x01, C=1
    memset(mem, 0, sizeof mem);
    { static const uint8_t c[] = {0xA9, 0xFF, 0x0B, 0x80}; r = run_at(c, sizeof c, 2, &cy, z); }
    CHECK(r.A == 0x80 && r.C_ == 1, "ANC A=%02x C=%u", r.A, r.C_);
    { static const uint8_t c[] = {0xA9, 0xFF, 0x4B, 0x03}; r = run_at(c, sizeof c, 2, &cy, z); }
    CHECK(r.A == 0x01 && r.C_ == 1, "ALR A=%02x C=%u", r.A, r.C_);

    // Cycle counts: LDA abs,X page cross = 5, STA abs,X = 5, INC abs,X = 7,
    // LDA (zp),Y cross = 6, taken branch across page = 4, NOP abs,X cross = 5.
    memset(mem, 0, sizeof mem);
    mem[0x30] = 0xFF; mem[0x31] = 0x10;
    {
        static const uint8_t c[] = {0xA2, 0x01, 0xA0, 0x01, 0xBD, 0xFF, 0x10};
        run_at(c, sizeof c, 2, NULL, z);
        regs_t s = z; s.X = 1; s.Y = 1;
        run_at(c + 4, 3, 1, &cy, s); CHECK(cy == 5, "LDA abs,X cross cy=%u", cy);
        static const uint8_t c2[] = {0x9D, 0x00, 0x10}; run_at(c2, 3, 1, &cy, s); CHECK(cy == 5, "STA abs,X cy=%u", cy);
        static const uint8_t c3[] = {0xFE, 0x00, 0x10}; run_at(c3, 3, 1, &cy, s); CHECK(cy == 7, "INC abs,X cy=%u", cy);
        static const uint8_t c4[] = {0xB1, 0x30}; run_at(c4, 2, 1, &cy, s); CHECK(cy == 6, "LDA (zp),Y cross cy=%u", cy);
        static const uint8_t c5[] = {0x1C, 0xFF, 0x10}; run_at(c5, 3, 1, &cy, s); CHECK(cy == 5, "NOP abs,X cross cy=%u", cy);
        static const uint8_t c6[] = {0x20, 0x00, 0x03}; run_at(c6, 3, 1, &cy, s); CHECK(cy == 6, "JSR cy=%u", cy);
    }
    {
        // BNE at 0x2F0 forward +0x20 crosses into 0x3xx: 4 cycles.
        memset(mem, 0, sizeof mem);
        mem[0x2F0] = 0xD0; mem[0x2F1] = 0x20;
        regs_t s = z; s.PC = 0x2F0; s.Z_ = 1;
        cy = step(&s);
        CHECK(cy == 4 && s.PC == 0x312, "branch cross cy=%u pc=%04x", cy, s.PC);
    }
    {
        // Decimal: 0x58 + 0x46 + 1 = 0x05, C=1; 0x12 - 0x21 = 0x91, C=0.
        memset(mem, 0, sizeof mem);
        static const uint8_t c[] = {0xF8, 0x38, 0xA9, 0x58, 0x69, 0x46};
        r = run_at(c, sizeof c, 4, &cy, z);
        CHECK(r.A == 0x05 && r.C_ == 1, "BCD ADC A=%02x C=%u", r.A, r.C_);
        static const uint8_t d[] = {0xF8, 0x38, 0xA9, 0x12, 0xE9, 0x21};
        r = run_at(d, sizeof d, 4, &cy, z);
        CHECK(r.A == 0x91 && r.C_ == 0, "BCD SBC A=%02x C=%u", r.A, r.C_);
    }
    printf("own 6502 checks: %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

static int klaus(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    size_t n = fread(mem, 1, sizeof mem, f);
    fclose(f);
    if (n != 65536) { printf("expected a 64 KB image\n"); return 2; }
    regs_t r = {0};
    r.S = 0xFF;
    r.PC = 0x0400;
    r.I_ = 1;
    cyc = 0;
    for (uint64_t i = 0; i < 200000000ULL; i++) {
        uint16_t pc = r.PC;
        step(&r);
        if (jammed) { printf("Klaus: JAM at %04x\n", pc); return 1; }
        if (r.PC == pc) {
            if (pc == 0x3469) {
                printf("Klaus 6502_functional_test: PASS (trap 0x3469, %llu instructions, %llu cycles)\n",
                    (unsigned long long)i, (unsigned long long)cyc);
                return 0;
            }
            printf("Klaus 6502_functional_test: FAIL, trapped at %04x (see .lst)\n", pc);
            return 1;
        }
    }
    printf("Klaus: timeout at %04x\n", r.PC);
    return 1;
}

int main(int argc, char **argv) {
    if (argc > 1) return klaus(argv[1]);
    return own_tests();
}
