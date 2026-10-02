// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Host test for cpu65c02.h on a flat 64 KB bus. Klaus Dormann's tests are
// loaded at 0 and entered at $0400; a test passes when the CPU loops on the
// success trap address, and fails if it loops anywhere else.
//
//   host_cpu_test rockwell 65C02_extended_opcodes_test.bin 24f1
//   host_cpu_test rockwell 6502_functional_test.bin 3469
//   host_cpu_test sc02                       own checks of the 65SC02 NOPs
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[65536];
static uint64_t cyc;
static int stopped;

typedef struct {
    uint8_t A, X, Y, S;
    uint16_t PC;
    unsigned C_, V_, D_, I_, N_, Z_;
} regs_t;

#define RD(a) (cyc++, mem[(uint16_t)(a)])
#define WR(a, v) (cyc++, mem[(uint16_t)(a)] = (uint8_t)(v))
#define TICK() (cyc++)
#define WAIT() (stopped = 1)
#define STOP() (stopped = 2)

#define STEP_BODY \
    uint8_t A = r->A, X = r->X, Y = r->Y, S = r->S; \
    uint16_t PC = r->PC; \
    unsigned C_ = r->C_, V_ = r->V_, D_ = r->D_, I_ = r->I_, N_ = r->N_, Z_ = r->Z_;
#define STEP_END \
    r->A = A; r->X = X; r->Y = Y; r->S = S; r->PC = PC; \
    r->C_ = C_; r->V_ = V_; r->D_ = D_; r->I_ = I_; r->N_ = N_; r->Z_ = Z_;

static void step_rockwell(regs_t *r) {
    STEP_BODY
#define CPU_ROCKWELL 1
#include "cpu65c02.h"
#undef CPU_ROCKWELL
    STEP_END
}

static void step_sc02(regs_t *r) {
    STEP_BODY
#define CPU_ROCKWELL 0
#include "cpu65c02.h"
#undef CPU_ROCKWELL
    STEP_END
}

static int run_klaus(const char *path, unsigned success) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    size_t n = fread(mem, 1, sizeof(mem), f);
    fclose(f);
    regs_t r = {0};
    r.S = 0xFF;
    r.PC = 0x400;
    r.I_ = 1;
    uint64_t steps = 0;
    for (;;) {
        uint16_t pc = r.PC;
        step_rockwell(&r);
        steps++;
        if (r.PC == pc) {
            if (pc == success) {
                printf("PASS %s (%zu bytes): success trap $%04X after %llu instructions, %llu cycles\n",
                    path, n, pc, (unsigned long long)steps, (unsigned long long)cyc);
                return 0;
            }
            printf("FAIL %s: trapped at $%04X after %llu instructions\n", path, pc,
                (unsigned long long)steps);
            return 1;
        }
        if (steps > 200000000ULL) {
            printf("FAIL %s: no trap after %llu instructions\n", path, (unsigned long long)steps);
            return 1;
        }
    }
}

// The Lynx 65SC02 has no Rockwell bit opcodes: $x7 and $xF are one byte NOPs.
static int run_sc02(void) {
    int fails = 0;
    for (int hi = 0; hi < 16; hi++) {
        for (int lo = 0x07; lo <= 0x0F; lo += 8) {
            memset(mem, 0xEA, sizeof(mem));
            mem[0x200] = (uint8_t)(hi << 4 | lo);
            regs_t r = {0};
            r.S = 0xFF;
            r.PC = 0x200;
            r.A = 0x12;
            r.Z_ = 1;
            cyc = 0;
            step_sc02(&r);
            if (r.PC != 0x201 || cyc != 1 || r.A != 0x12) {
                printf("FAIL sc02 opcode $%02X: PC=$%04X cycles=%llu\n", mem[0x200], r.PC,
                    (unsigned long long)cyc);
                fails++;
            }
        }
    }
    if (!fails) {
        printf("PASS sc02: all 32 Rockwell opcodes are 1 byte, 1 cycle NOPs\n");
    }
    return fails != 0;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "rockwell")) {
        return run_klaus(argv[2], (unsigned)strtoul(argv[3], NULL, 16));
    }
    if (argc >= 2 && !strcmp(argv[1], "sc02")) {
        return run_sc02();
    }
    fprintf(stderr, "usage: host_cpu_test rockwell test.bin success_hex | sc02\n");
    return 2;
}
