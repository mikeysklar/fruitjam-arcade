#!/bin/bash
# Build the bare-metal bench images (same code flags as the .mpy build).
set -e
cd "$(dirname "$0")"
FL="-mthumb -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -O2 -g -funroll-loops
    -ffreestanding -nostdlib -fno-common -fno-builtin-printf -Wall -Wno-unused-function -I.."
arm-none-eabi-gcc $FL -T bench.ld -o bench_sms.elf bench_sms.c -lgcc
if [ -f ../../../src/peanutgb.c ]; then
    arm-none-eabi-gcc $FL -funroll-loops -I../../../src -T bench.ld -o bench_gb.elf bench_gb.c -lgcc
fi
