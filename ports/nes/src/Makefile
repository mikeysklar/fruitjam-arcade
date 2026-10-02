# Native module build for nes. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = nes
SRC = nes.c
BUILD = build/$(ARCH)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2 -funroll-loops
CFLAGS += $(OPT)

# Regenerate the opcode switch and tables (checked in).
nes_ops.inc: gen_ops.py
	python3 gen_ops.py > $@
nes_tables.h: gen_tables.py
	python3 gen_tables.py > $@
