# mloadtest: a module for the SD loader's Wii check. Built in the container:
#
#     tools/build.sh testmod        -> build/mloadtest (the ELF stripios made)
#
# Link order and flags follow source/mload-module/Makefile; the module sits at
# 0x13760000 (link.ld), high in MLOAD's module area.

DEVKITARM ?=	/opt/devkitARM
PREFIX	=	$(DEVKITARM)/bin/arm-eabi-
CC	=	$(PREFIX)gcc
LD	=	$(PREFIX)gcc
STRIP	=	../../../../stripios

LIB	=	../../../../source/cios-lib
ARCH	=	-mcpu=arm926ej-s -mthumb -mthumb-interwork -mbig-endian
CFLAGS	=	$(ARCH) -I$(LIB) -ffreestanding -fno-builtin -ffunction-sections -fomit-frame-pointer -Os -Wall $(EXTRA_CFLAGS)
LDFLAGS	=	$(ARCH) -nostartfiles -nostdlib -Wl,-T,link.ld,-Map,mloadtest.map -Wl,--gc-sections -Wl,-static

mloadtest.elf: start.o main.o $(LIB)/cios-lib.a
	@printf ' LD\t%s\n' $@
	@$(LD) $(LDFLAGS) start.o main.o $(LIB)/cios-lib.a -lgcc -o $@.orig
	@$(STRIP) $@.orig $@

%.o: %.s
	@printf ' CC\t%s\n' $@
	@$(CC) $(CFLAGS) -D_LANGUAGE_ASSEMBLY -c -x assembler-with-cpp -o $@ $<

%.o: %.c
	@printf ' CC\t%s\n' $@
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@rm -f start.o main.o mloadtest.elf mloadtest.elf.orig mloadtest.map

.PHONY: clean
