# Native module build for peanutgbc (Game Boy Color). OPT is appended after the
# dynruntime.mk include so the last -O wins over its -Os default.
#   make MPY_DIR=../../../../circuitpython ARCH=armv7emsp   -> peanutgbc.mpy
#   make host_test && ./run_host_tests.sh                -> host checks, no MPY_DIR needed
ifeq ($(MAKECMDGOALS),host_test)
host_test: host_main.c peanutgbc.c peanut_gb_cgb.h minigb_apu.c minigb_apu.h
	cc -O2 -Wall -DHOST_TEST -o $@ host_main.c
else
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = peanutgbc
SRC = peanutgbc.c
BUILD = build/$(ARCH)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2 -funroll-loops
CFLAGS += $(OPT)
endif
