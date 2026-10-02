// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Run a CP/M .com Z80 test (zexdoc, zexall) against z80.h on the host.
// Flat 64 KB RAM, program at $0100, BDOS calls 2 and 9 at $0005, exit at $0000.
//   host_zex testroms/zex/zexdoc.com
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t mem[65536];

#define Z80_RD(a) mem[a]
#define Z80_WR(a, v) (mem[a] = (v))
#define Z80_IN(p) 0xFF
#define Z80_OUT(p, v) ((void)(p), (void)(v))
#define Z80_COUNT
#include "z80.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: host_zex file.com\n");
        return 2;
    }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        perror(argv[1]);
        return 2;
    }
    size_t n = fread(mem + 0x100, 1, 0xFE00, fp);
    fclose(fp);
    printf("loaded %zu bytes\n", n);
    static z80_t cpu;
    z80_t *z = &cpu;
    z80_reset(z);
    mem[0x0005] = 0xC9;  // RET; BDOS is handled below before it runs
    mem[0x0006] = 0x00;  // top of TPA at $FF00 for programs that read it
    mem[0x0007] = 0xFF;
    z->pc = 0x100;
    z->sp = 0xFF00;
    int errors = 0;
    char line[256];
    int ll = 0;
    uint64_t cycles = 0;
    for (;;) {
        if (z->pc == 0x0005) {
            if (z->bc.b.l == 2) {
                putchar(z->de.b.l);
                line[ll < 255 ? ll++ : ll] = z->de.b.l;
            } else if (z->bc.b.l == 9) {
                for (uint16_t a = z->de.w; mem[a] != '$'; a++) {
                    putchar(mem[a]);
                    line[ll < 255 ? ll++ : ll] = mem[a];
                    if (mem[a] == '\n') {
                        line[ll] = 0;
                        if (strstr(line, "ERROR")) {
                            errors++;
                        }
                        ll = 0;
                    }
                }
            }
            fflush(stdout);
        } else if (z->pc == 0x0000) {
            break;
        }
        int32_t c0 = z->cyc;
        z80_step(z);
        cycles += (uint32_t)(z->cyc - c0);
        if (z->cyc > 1000000000) {
            z->cyc = 0;
        }
    }
    printf("\nZEX %s errors=%d cycles=%llu\n", argv[1], errors,
        (unsigned long long)cycles);
    return errors ? 1 : 0;
}
