# Native module build for supervision. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = supervision
SRC = supervision.c
BUILD = build/$(ARCH)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2
CFLAGS += $(OPT) -Werror
