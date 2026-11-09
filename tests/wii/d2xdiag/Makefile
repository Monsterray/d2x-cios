# d2xdiag: what a d2x debug build (+dbg) knows, and fault scenarios through it.
#
#     make -C tests/wii/d2xdiag         (devkitPPC and libogc; on Windows from
#                                       the devkitPro MSYS2 shell)
#     python tests/wii/d2xdiag.py -- --ios=249 [--wd=S] [--scenario=NAME,...]
#
# hbc_netlog.h is hbc-reborn's public-domain header (sdk/hbc_netlog.h).

ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii

d2xdiag.dol: d2xdiag.elf
	$(ELF2DOL) $< $@

d2xdiag.elf: main.c hbc_netlog.h ../../../source/cios-lib/d2xdbg.h
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -lfat -logc -o $@

clean:
	rm -f d2xdiag.elf d2xdiag.dol

.PHONY: clean
