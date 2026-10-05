# fruitjam-scumm: the SCUMM engine as a CircuitPython native module.
#   make MPY_DIR=/path/to/circuitpython      -> scumm.mpy
# The engine is C++ and never sees MicroPython headers; src/modscumm.c is
# the only file built through dynruntime.mk. Everything is partially linked
# into one section first (tools/natmod.ld) and then handed to mpy_ld.
MPY_DIR ?= ../circuitpython
ARCH ?= armv7emsp
MOD = scumm
SRC = src/modscumm.c
BUILD = build/$(ARCH)
# These come before dynruntime.mk so its generic %.c rule (MicroPython
# flags, -Werror) only builds modscumm.c.
$(BUILD)/src/backend/%.o: src/backend/%.c
	$(ECHO) "CC $<"
	$(Q)$(MKDIR) -p $(dir $@)
	$(Q)$(CC) $(FJ_CFLAGS) -MMD -c -o $@ $<

$(BUILD)/src/libc/%.o: src/libc/%.c
	$(ECHO) "CC $<"
	$(Q)$(MKDIR) -p $(dir $@)
	$(Q)$(CC) $(FJ_CFLAGS) -MMD -c -o $@ $<

include $(MPY_DIR)/py/dynruntime.mk
OPT ?= -O2
CFLAGS += $(OPT) -ffunction-sections -fdata-sections

CXX = $(CROSS)g++
LD = $(CROSS)ld
CXXFLAGS = $(CFLAGS_ARCH) $(OPT) -std=gnu++11 -Wall -Wno-unused -Wno-sign-compare -Wno-reorder -Wno-unknown-pragmas \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit \
	-fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
	-fno-builtin-malloc -Isrc -DNDEBUG -DFJ_NATMOD -DFJ_ADLIB
FJ_CFLAGS = $(CFLAGS_ARCH) $(OPT) -std=gnu99 -Wall -ffunction-sections -fdata-sections \
	-fno-unwind-tables -fno-builtin -Isrc -DFJ_NATMOD

ENGINE_SRC := $(wildcard src/scumm/*.cpp src/scumm/players/*.cpp src/common/*.cpp \
	src/graphics/*.cpp src/audio/*.cpp src/audio/decoders/*.cpp src/audio/softsynth/opl/*.cpp \
	src/backend/*.cpp src/libc/*.cpp)
FJ_C_SRC := $(wildcard src/backend/*.c src/libc/*.c)
ENGINE_O := $(patsubst %.cpp,$(BUILD)/%.o,$(ENGINE_SRC)) $(patsubst %.c,$(BUILD)/%.o,$(FJ_C_SRC))
LIBGCC := $(realpath $(shell $(CC) $(CFLAGS_ARCH) --print-libgcc-file-name))

$(BUILD)/%.o: %.cpp
	$(ECHO) "CXX $<"
	$(Q)$(MKDIR) -p $(dir $@)
	$(Q)$(CXX) $(CXXFLAGS) -MMD -c -o $@ $<

$(BUILD)/prelink.o: $(SRC_O) $(ENGINE_O) tools/natmod.ld
	$(ECHO) "LD -r $@"
	$(Q)$(LD) -r --force-group-allocation -T tools/natmod.ld --gc-sections -e mpy_init -u mpy_init -Map $(BUILD)/prelink.map \
		-o $@ $(SRC_O) $(ENGINE_O) $(LIBGCC)

# Replaces dynruntime.mk's link rule (make warns about the override).
$(BUILD)/$(MOD).mpy: $(BUILD)/prelink.o
	$(ECHO) "LINK $<"
	$(Q)$(MPY_LD) --arch $(ARCH) --qstrs $(CONFIG_H) $(MPY_LD_FLAGS) -o $@ $<

-include $(ENGINE_O:.o=.d)

# ARM build under the Unicorn emulator (pip install unicorn pyelftools).
armemu: $(BUILD)/prelink.o
	$(MAKE) -C tools/armemu
	MPY_DIR=$(MPY_DIR) python3 tools/armemu/mpyrun.py testdata/loom-dos-long-demo-en 600
