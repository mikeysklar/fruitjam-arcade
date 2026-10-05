# ARM build of the core as a bare-metal ELF for tools/armemu/run.py.
# Reuses the objects of the natmod build (run `make` at the top first).
TOP = ../..
ARMBUILD = $(TOP)/build/armv7emsp
OUT = $(TOP)/build/armemu
CC = arm-none-eabi-gcc
ARCH = -mthumb -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard
OBJ = $(filter-out %/modscumm.o %/prelink.o,$(shell find $(ARMBUILD)/src -name '*.o'))
LIBGCC = $(shell $(CC) $(ARCH) --print-libgcc-file-name)

$(OUT)/armtest.elf: armtest.c link.ld $(OBJ)
	mkdir -p $(OUT)
	$(CC) $(ARCH) -O2 -fpic -fno-builtin -fno-tree-loop-distribute-patterns -I$(TOP)/src -c -o $(OUT)/armtest.o armtest.c
	arm-none-eabi-ld -T link.ld --gc-sections -o $@ $(OUT)/armtest.o $(OBJ) $(LIBGCC)
