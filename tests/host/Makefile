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

test: ums/test_ums usbs/test_usbs usbs/test_usbs_async fat/test_fat fat/test_diskio sdio/test_sdio dbg/test_dbg \
		ums/test_ums_dbg usbs/test_usbs_dbg sdio/test_sdio_dbg  fat/test_fat_dbg
	$(RUN) ./ums/test_ums
	$(RUN) ./usbs/test_usbs
	$(RUN) ./usbs/test_usbs_async
	$(RUN) ./fat/test_fat
	$(RUN) ./fat/test_diskio
	$(RUN) ./sdio/test_sdio
	$(RUN) ./dbg/test_dbg
	$(RUN) ./ums/test_ums_dbg
	$(RUN) ./usbs/test_usbs_dbg
	$(RUN) ./sdio/test_sdio_dbg
	$(RUN) ./fat/test_fat_dbg

# The debug area's logic (mload-module/dbg_core.c, -DD2X_DEBUG builds).
MLOADMOD := ../../source/mload-module
dbg/test_dbg: dbg/test_dbg.c hostcheck.h $(MLOADMOD)/dbg_core.c $(CIOSLIB)/d2xdbg.h
	$(CC) $(ALL_CFLAGS) -I. -I$(CIOSLIB) -I$(MLOADMOD) -o $@ dbg/test_dbg.c

# Modules built as the +dbg variant (-DD2X_DEBUG): the debug area runs on the
# PC (dbg/swi_host.c), with each test's own types (D2XDBG_HAVE_TYPES).
DBG_FLAGS := -DD2X_DEBUG -DD2XDBG_HAVE_TYPES
DBG_DEPS  := dbg/swi_host.c dbg/dbg_host.h $(MLOADMOD)/dbg_core.c $(CIOSLIB)/d2xdbg.h

ums/test_ums_dbg: ums/test_ums_dbg.c ums/ums_unit.c $(SIM_DEPS) $(DBG_DEPS) $(TINYEHCI)/usb.c \
		$(TINYEHCI)/usb.h $(TINYEHCI)/usbstorage.c $(TINYEHCI)/usbstorage.h
	$(CC) $(ALL_CFLAGS) $(DBG_FLAGS) -I. -Iums -Idbg -I$(TINYEHCI) -I$(MLOADMOD) -I$(CIOSLIB) -o $@ \
		ums/test_ums_dbg.c ums/sim.c ums/ums_unit.c dbg/swi_host.c

usbs/test_usbs_dbg: usbs/test_usbs_dbg.c usbs/glue_sim.c usbs/usbs_unit.c usbs/stub/*.h $(SIM_DEPS) \
		$(DBG_DEPS) $(USBS)/usbstorage.c $(USBS)/usbstorage.h $(USBS)/usb2.h
	$(CC) $(ALL_CFLAGS) $(DBG_FLAGS) -I. -Iums -Idbg -I$(MLOADMOD) -I$(CIOSLIB) -c \
		-o usbs/dbg_test.o usbs/test_usbs_dbg.c
	$(CC) $(ALL_CFLAGS) -I. -Iums -I$(TINYEHCI) -c -o usbs/dbg_sim.o ums/sim.c
	$(CC) $(ALL_CFLAGS) -I. -Iums -I$(TINYEHCI) -c -o usbs/dbg_glue.o usbs/glue_sim.c
	$(CC) $(ALL_CFLAGS) $(DBG_FLAGS) -I. -Iums -Idbg -I$(MLOADMOD) -I$(CIOSLIB) -c -o usbs/dbg_swi.o dbg/swi_host.c
	$(CC) $(ALL_CFLAGS) $(DBG_FLAGS) -Wno-pointer-to-int-cast -Iusbs/stub -I$(USBS) -I$(CIOSLIB) \
		-c -o usbs/dbg_unit.o usbs/usbs_unit.c
	$(CC) $(ALL_CFLAGS) -o $@ usbs/dbg_test.o usbs/dbg_sim.o usbs/dbg_glue.o usbs/dbg_swi.o usbs/dbg_unit.o

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

# usb2.c's asynchronous bulk transfers over a model of IOS (queue, timer,
# cancel); real cios-lib headers, not usbs/stub.
usbs/test_usbs_async: usbs/test_usbs_async.c hostcheck.h $(USBS)/usb2.c $(USBS)/usb2.h \
		$(USBS)/usbstorage.h
	$(CC) $(ALL_CFLAGS) -Wno-pointer-to-int-cast -I. -I$(USBS) -I$(CIOSLIB) -o $@ usbs/test_usbs_async.c

# The FAT module's main.c (its IPC handlers), fat_wrapper.c and FatFs on two
# RAM disks. -no-pie: file descriptors are FIL pointers cast to u32
# (fat/fat_unit.c).
FATMOD := ../../source/fat-module
fat/test_fat: fat/test_fat.c fat/fat_unit.c fat/ff_unit.c fat/ramdisk.c fat/ramdisk.h \
		fat/fat_types.h hostcheck.h fat/stub/*.h $(FATMOD)/fat_wrapper.c $(FATMOD)/main.c $(FATMOD)/ff.c \
		$(FATMOD)/ff.h $(FATMOD)/ffconf.h $(CIOSLIB)/fat_tools.c
	$(CC) $(ALL_CFLAGS) -I. -Ifat -I$(FATMOD) -c -o fat/test_fat.o fat/test_fat.c
	$(CC) $(ALL_CFLAGS) -Ifat -I$(FATMOD) -c -o fat/ramdisk.o fat/ramdisk.c
	$(CC) $(ALL_CFLAGS) -Wno-implicit-fallthrough -Ifat -I$(FATMOD) -c -o fat/ff_unit.o fat/ff_unit.c
	$(CC) $(ALL_CFLAGS) -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast \
		-Ifat -Ifat/stub -I$(FATMOD) -I$(CIOSLIB) -c -o fat/fat_unit.o fat/fat_unit.c
	$(CC) $(ALL_CFLAGS) -no-pie -o $@ fat/test_fat.o fat/ramdisk.o fat/ff_unit.o fat/fat_unit.o

# The FAT module's diskio.c with the SD and EHCI drivers stubbed on RAM.
fat/test_diskio: fat/test_diskio.c fat/fat_types.h hostcheck.h fat/stub/mem.h $(FATMOD)/diskio.c \
		$(FATMOD)/diskio.h
	$(CC) $(ALL_CFLAGS) -Wno-pointer-to-int-cast -I. -Ifat -Ifat/stub -I$(FATMOD) -I$(CIOSLIB) \
		-o $@ fat/test_diskio.c

# cios-lib's sdio.c with -DSDIO_HIGH_SPEED, on a fake of IOS's SD driver.
sdio/test_sdio: sdio/test_sdio.c hostcheck.h $(CIOSLIB)/sdio.c $(CIOSLIB)/sdio.h \
		$(CIOSLIB)/sdio_hs.h
	$(CC) $(ALL_CFLAGS) -DSDIO_HIGH_SPEED -DSDIO_HS_DEBUG -Wno-pointer-to-int-cast -I. -I$(CIOSLIB) \
		-o $@ sdio/test_sdio.c

sdio/test_sdio_dbg: sdio/test_sdio_dbg.c hostcheck.h $(DBG_DEPS) $(CIOSLIB)/sdio.c $(CIOSLIB)/sdio.h \
		$(CIOSLIB)/sdio_hs.h
	$(CC) $(ALL_CFLAGS) $(DBG_FLAGS) -DSDIO_HIGH_SPEED -DSDIO_HS_DEBUG -Wno-pointer-to-int-cast -I. -Idbg \
		-I$(MLOADMOD) -I$(CIOSLIB) -o $@ sdio/test_sdio_dbg.c dbg/swi_host.c

fat/test_fat_dbg: fat/test_fat_dbg.c fat/ff_unit.c fat/ramdisk.c fat/ramdisk.h fat/fat_types.h hostcheck.h \
		$(DBG_DEPS) fat/stub/*.h $(FATMOD)/fat_wrapper.c $(FATMOD)/main.c $(FATMOD)/ff.c $(FATMOD)/ff.h \
		$(FATMOD)/ffconf.h $(CIOSLIB)/fat_tools.c
	$(CC) $(ALL_CFLAGS) -Ifat -I$(FATMOD) -c -o fat/dbg_ramdisk.o fat/ramdisk.c
	$(CC) $(ALL_CFLAGS) -Wno-implicit-fallthrough -Ifat -I$(FATMOD) -c -o fat/dbg_ff.o fat/ff_unit.c
	$(CC) $(ALL_CFLAGS) $(DBG_FLAGS) -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast -I. -Ifat -Ifat/stub \
		-Idbg -I$(FATMOD) -I$(MLOADMOD) -I$(CIOSLIB) -c -o fat/dbg_test.o fat/test_fat_dbg.c
	$(CC) $(ALL_CFLAGS) $(DBG_FLAGS) -I. -Idbg -I$(MLOADMOD) -I$(CIOSLIB) -c -o fat/dbg_swi.o dbg/swi_host.c
	$(CC) $(ALL_CFLAGS) -no-pie -o $@ fat/dbg_test.o fat/dbg_swi.o fat/dbg_ramdisk.o fat/dbg_ff.o

clean:
	rm -f ums/test_ums ums/test_ums.exe usbs/test_usbs usbs/test_usbs.exe usbs/*.o \
		fat/test_fat fat/test_diskio fat/*.o sdio/test_sdio dbg/test_dbg ums/test_ums_dbg \
		usbs/test_usbs_dbg usbs/test_usbs_async sdio/test_sdio_dbg fat/test_fat_dbg
