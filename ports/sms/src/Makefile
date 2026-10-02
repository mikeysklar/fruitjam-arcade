# Native module build for sms. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = sms
SRC = sms.c
BUILD = build/$(ARCH)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2 -funroll-loops
CFLAGS += $(OPT)
$(BUILD)/sms.o: smscore.h z80_ops.inc

# Regenerate the opcode switch (checked in).
z80_ops.inc: gen_z80.py
	python3 gen_z80.py > $@
