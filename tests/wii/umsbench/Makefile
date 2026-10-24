# umsbench: time USB sector reads and writes through a cIOS on a Wii.
#
#     make -C tests/wii/umsbench        (devkitPPC and libogc; on Windows from
#                                       the devkitPro MSYS2 shell)
#     hbc.py run tests/wii/umsbench/umsbench.dol --ios=249 [--write-back]
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

umsbench.dol: umsbench.elf
	$(ELF2DOL) $< $@

umsbench.elf: main.c hbc_netlog.h
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -logc -o $@

clean:
	rm -f umsbench.elf umsbench.dol

.PHONY: clean
