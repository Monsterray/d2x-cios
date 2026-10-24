# Host tests: module code built for this PC and run against simulations.
#
#     make -C tests/host          build and run every test
#     make -C tests/host clean
#
# Needs a C compiler (gcc or clang). On Windows, use the devkitPro MSYS2
# shell, WSL, or `tools/build.sh test`, which runs this in the build image.

CC      ?= cc
CFLAGS  ?= -O1 -g
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
           -Wno-unused-variable -Wno-sign-compare -Wno-pointer-sign \
           -Wno-address-of-packed-member -Wno-unused-but-set-variable \
           -Wno-unused-label -Wno-misleading-indentation
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
TINYEHCI := ../../source/ehci-module/tinyehci

ALL_CFLAGS = $(CFLAGS) $(WARN) $(SAN) $(EXTRA_CFLAGS)

# A wall-clock limit as well as the simulated one: a hang outside the
# simulation (a sanitizer runtime, a loop that never calls the simulation)
# must fail the run, not stall CI.
RUN := $(shell command -v timeout >/dev/null 2>&1 && echo timeout 300)

.PHONY: all test clean
all: test

test: ums/test_ums
	$(RUN) ./ums/test_ums

ums/test_ums: ums/test_ums.c ums/sim.c ums/ums_unit.c ums/sim.h ums/types.h hostcheck.h \
		$(TINYEHCI)/usbstorage.c $(TINYEHCI)/usbstorage.h $(TINYEHCI)/usb.h
	$(CC) $(ALL_CFLAGS) -I. -Iums -I$(TINYEHCI) -o $@ ums/test_ums.c ums/sim.c ums/ums_unit.c

clean:
	rm -f ums/test_ums ums/test_ums.exe
