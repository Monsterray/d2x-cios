# Shared rules for the Wii bench programs (sdbench, umsbench, netbench).
# A program's Makefile sets NAME and includes this file:
#
#     NAME := umsbench
#     include ../common/bench.mk
#
# Needs devkitPPC and libogc (on Windows, the devkitPro MSYS2 shell).
# hbc_netlog.h is hbc-reborn's public-domain header; one copy, in common/.

ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I../common -I$(DEVKITPRO)/libogc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii
LIBS ?= -lfat -logc

$(NAME).dol: $(NAME).elf
	$(ELF2DOL) $< $@

$(NAME).elf: main.c ../common/hbc_netlog.h
	$(CC) $(CFLAGS) main.c $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -f $(NAME).elf $(NAME).dol

.PHONY: clean
