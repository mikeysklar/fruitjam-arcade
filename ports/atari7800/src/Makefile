# Native module build for atari7800. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
#   make MPY_DIR=../../../../circuitpython ARCH=armv7emsp    -> atari7800.mpy
#   make test                                             -> host tests
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = atari7800
SRC = atari7800.c
BUILD = build/$(ARCH)
ifeq ($(filter host test clean-host host_main host_cpu_test,$(MAKECMDGOALS)),)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2 -funroll-loops
CFLAGS += $(OPT)
endif

# Host tests (system cc), not part of the .mpy.
HOSTCC ?= cc
KLAUS = testroms/6502_functional_test.bin

host: host_main host_cpu_test testcart/testcart.a78
host_main: host_main.c atari7800.c cpu6502.h palette.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ host_main.c
host_cpu_test: host_cpu_test.c cpu6502.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ host_cpu_test.c
testcart/testcart.a78: testcart/make_testcart.py
	python3 testcart/make_testcart.py $@
$(KLAUS):
	./get_testroms.sh
test: host $(KLAUS)
	./run_tests.sh
clean-host:
	rm -f host_main host_cpu_test host_trace
.PHONY: host test clean-host
