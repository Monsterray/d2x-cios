# sdbench: time SD reads through /dev/sdio/sdhc in a cIOS, and read its high-speed state (read only).
#
#     make -C tests/wii/sdbench        (devkitPPC and libogc; on Windows from
#                                       the devkitPro MSYS2 shell)
#     hbc.py run tests/wii/sdbench/sdbench.dol --ios=249 [--mb=32]
#
# hbc_netlog.h is hbc-reborn's public-domain header (sdk/hbc_netlog/hbc_netlog.h).

ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC in your environment.")
endif

CC = $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL := $(firstword $(wildcard $(DEVKITPRO)/tools/bin/elf2dol.exe) $(DEVKITPRO)/tools/bin/elf2dol)
CFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -O2 -Wall -Wextra $(EXTRA_CFLAGS) \
	-I$(DEVKITPRO)/libogc/include
LDFLAGS = -mrvl -mcpu=750 -meabi -mhard-float -Wl,-z,noexecstack \
	-L$(DEVKITPRO)/libogc/lib/wii

sdbench.dol: sdbench.elf
	$(ELF2DOL) $< $@

sdbench.elf: main.c hbc_netlog.h
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -lfat -logc -o $@

clean:
	rm -f sdbench.elf sdbench.dol

.PHONY: clean
