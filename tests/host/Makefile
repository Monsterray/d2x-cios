# Host tests: module code built for this PC and run against simulations.
#
#     make -C tests/host          build and run every test
#     make -C tests/host clean
#
# Needs a C compiler (gcc or clang). On Windows, use the devkitPro MSYS2
# shell, WSL, or `tools/build.sh test`, which runs this in the build image.
#
#   ums/    the EHCI module's tinyehci usb.c and usbstorage.c
#   usbs/   the USBS module's usbstorage.c, over IOS's USB API
# Both run against the simulated drive in ums/sim.c.

CC      ?= cc
CFLAGS  ?= -O1 -g
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -Wno-unused-variable -Wno-sign-compare -Wno-pointer-sign \
           -Wno-address-of-packed-member -Wno-unused-but-set-variable \
           -Wno-unused-label -Wno-misleading-indentation
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
TINYEHCI := ../../source/ehci-module/tinyehci
USBS     := ../../source/usb-module
CIOSLIB  := ../../source/cios-lib

ALL_CFLAGS = $(CFLAGS) $(WARN) $(SAN) $(EXTRA_CFLAGS)
SIM_DEPS   = ums/sim.c ums/sim.h ums/types.h ums/ehci_dev.h hostcheck.h

# A wall-clock limit as well as the simulated one: a hang outside the
# simulation (a sanitizer runtime, a loop that never calls the simulation)
# must fail the run, not stall CI.
RUN := $(shell command -v timeout >/dev/null 2>&1 && echo timeout 300)

.PHONY: all test clean
all: test

test: ums/test_ums usbs/test_usbs fat/test_fat
	$(RUN) ./ums/test_ums
	$(RUN) ./usbs/test_usbs
	$(RUN) ./fat/test_fat

ums/test_ums: ums/test_ums.c ums/ums_unit.c $(SIM_DEPS) $(TINYEHCI)/usb.c $(TINYEHCI)/usb.h \
		$(TINYEHCI)/usbstorage.c $(TINYEHCI)/usbstorage.h
	$(CC) $(ALL_CFLAGS) -I. -Iums -I$(TINYEHCI) -o $@ ums/test_ums.c ums/sim.c ums/ums_unit.c

# The USBS code is built with cios-lib's types (s32 is long there), apart
# from the simulation; usbs/stub stands in for cios-lib's ipc.h, mem.h and
# timer.h. Its MEM2 checks cast pointers to u32, which is harmless here.
usbs/test_usbs: usbs/test_usbs.c usbs/glue_sim.c usbs/usbs_unit.c usbs/stub/*.h $(SIM_DEPS) \
		$(USBS)/usbstorage.c $(USBS)/usbstorage.h $(USBS)/usb2.h
	$(CC) $(ALL_CFLAGS) -I. -Iums -c -o usbs/test_usbs.o usbs/test_usbs.c
	$(CC) $(ALL_CFLAGS) -I. -Iums -I$(TINYEHCI) -c -o usbs/sim.o ums/sim.c
	$(CC) $(ALL_CFLAGS) -I. -Iums -I$(TINYEHCI) -c -o usbs/glue_sim.o usbs/glue_sim.c
	$(CC) $(ALL_CFLAGS) -Wno-pointer-to-int-cast -Iusbs/stub -I$(USBS) -I$(CIOSLIB) \
		-c -o usbs/usbs_unit.o usbs/usbs_unit.c
	$(CC) $(ALL_CFLAGS) -o $@ usbs/test_usbs.o usbs/sim.o usbs/glue_sim.o usbs/usbs_unit.o

# The FAT module's fat_wrapper.c and FatFs on two RAM disks. -no-pie: file
# descriptors are FIL pointers cast to u32 (fat/fat_unit.c).
FATMOD := ../../source/fat-module
fat/test_fat: fat/test_fat.c fat/fat_unit.c fat/ff_unit.c fat/ramdisk.c fat/ramdisk.h \
		fat/fat_types.h hostcheck.h usbs/stub/*.h $(FATMOD)/fat_wrapper.c $(FATMOD)/ff.c \
		$(FATMOD)/ff.h $(FATMOD)/ffconf.h $(CIOSLIB)/fat_tools.c
	$(CC) $(ALL_CFLAGS) -I. -Ifat -c -o fat/test_fat.o fat/test_fat.c
	$(CC) $(ALL_CFLAGS) -Ifat -I$(FATMOD) -c -o fat/ramdisk.o fat/ramdisk.c
	$(CC) $(ALL_CFLAGS) -Wno-implicit-fallthrough -Ifat -I$(FATMOD) -c -o fat/ff_unit.o fat/ff_unit.c
	$(CC) $(ALL_CFLAGS) -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
		-Ifat -Iusbs/stub -I$(FATMOD) -I$(CIOSLIB) -c -o fat/fat_unit.o fat/fat_unit.c
	$(CC) $(ALL_CFLAGS) -no-pie -o $@ fat/test_fat.o fat/ramdisk.o fat/ff_unit.o fat/fat_unit.o

clean:
	rm -f ums/test_ums ums/test_ums.exe usbs/test_usbs usbs/test_usbs.exe usbs/*.o \
		fat/test_fat fat/*.o
