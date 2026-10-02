# Native module build for atari2600. OPT is appended after the dynruntime.mk
# include so the last -O wins over its -Os default.
#   make MPY_DIR=../../../../circuitpython ARCH=armv7emsp    -> atari2600.mpy
#   make test                                             -> host tests
MPY_DIR ?= ../../../../circuitpython
ARCH ?= armv7emsp
MOD = atari2600
SRC = atari2600.c
BUILD = build/$(ARCH)
ifeq ($(filter host test clean-host,$(MAKECMDGOALS)),)
include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2 -funroll-loops
CFLAGS += $(OPT)
endif

# Host tests (system cc), not part of the .mpy.
HOSTCC ?= cc
KLAUS = testroms/6502_functional_test.bin
KLAUS_URL = https://raw.githubusercontent.com/Klaus2m5/6502_65C02_functional_tests/master/bin_files/6502_functional_test.bin

host: host_main host_cpu_test testrom/testrom.bin
host_main: host_main.c atari2600.c cpu6502.h palette.h
	$(HOSTCC) -O2 -Wall -Wextra -DHOST_TEST -o $@ host_main.c
host_cpu_test: host_cpu_test.c cpu6502.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ host_cpu_test.c
testrom/testrom.bin: testrom/make_testrom.py
	python3 testrom/make_testrom.py $@
$(KLAUS):
	mkdir -p testroms
	curl -sSL -o $@ $(KLAUS_URL)
test: host $(KLAUS)
	./host_cpu_test
	./host_cpu_test $(KLAUS)
	./host_main --check testrom/testrom.bin
	cd testrom && python3 make_banktests.py ../testroms/banks
	./host_main --banks testroms/banks
clean-host:
	rm -f host_main host_cpu_test testrom.ppm
.PHONY: host test clean-host
