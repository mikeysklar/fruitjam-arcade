# SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
# SPDX-License-Identifier: MIT
# Native module build for msx. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = msx
SRC = msx.c
BUILD = build/$(ARCH)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2 -funroll-loops
CFLAGS += $(OPT)

# Regenerate the opcode switches and the C-BIOS arrays (both checked in).
z80_ops.inc: gen_ops.py
	python3 gen_ops.py > $@
cbios.h: gen_cbios.py cbios/cbios_main_msx1.rom cbios/cbios_logo_msx1.rom
	python3 gen_cbios.py > $@
