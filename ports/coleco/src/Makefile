# Native module build for coleco. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = coleco
SRC = coleco.c
BUILD = build/$(ARCH)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2 -funroll-loops
CFLAGS += $(OPT)

# Regenerate the opcode switches and the open BIOS (checked in).
z80_ops.inc: gen_ops.py
	python3 gen_ops.py > $@
cv_bios.h: gen_bios.py font8x8.txt
	python3 gen_bios.py > $@
