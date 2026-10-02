# Native module build for chip8. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = chip8
SRC = chip8.c
BUILD = build/$(ARCH)
ifneq ($(MAKECMDGOALS),host_test)
include $(MPY_DIR)/py/dynruntime.mk
endif
OPT ?= -O2
CFLAGS += $(OPT)

# Host test: `make host_test && ./host_test --suite testroms` (see NOTES.md).
host_test: host_main.c chip8.c
	cc -O2 -Wall -DHOST_TEST -o $@ host_main.c
