# devkitARM path
DEVKITARM ?=	/opt/devkitARM

# Prefix
PREFIX	=	$(DEVKITARM)/bin/arm-eabi-

# Executables
CC	=	$(PREFIX)gcc
LD	=	$(PREFIX)gcc
STRIP	=	../stripios

# Committer date for reproducible builds
SOURCE_DATE_EPOCH ?= $(shell git log -1 --pretty=%ct)
ifdef SOURCE_DATE_EPOCH
	BUILD_DATE ?= $(shell LC_ALL=C date -u -d "@$(SOURCE_DATE_EPOCH)" "+'%b %e %Y'" 2>/dev/null || LC_ALL=C date -u -r "$(SOURCE_DATE_EPOCH)" "+'%b %e %Y'" 2>/dev/null || LC_ALL=C date -u "+'%b %e %Y'")
	BUILD_TIME ?= $(shell LC_ALL=C date -u -d "@$(SOURCE_DATE_EPOCH)" "+'%H:%M:%S'" 2>/dev/null || LC_ALL=C date -u -r "$(SOURCE_DATE_EPOCH)" "+'%H:%M:%S'" 2>/dev/null || LC_ALL=C date -u "+'%H:%M:%S'")
else
	BUILD_DATE ?= $(shell date "+'%b %e %Y'")
	BUILD_TIME ?= $(shell date "+'%H:%M:%S'")
endif

# Flags
ARCH	=	-mcpu=arm926ej-s -mthumb -mthumb-interwork -mbig-endian
CFLAGS	=	$(ARCH) -I. -I../cios-lib -ffreestanding -fno-builtin -ffunction-sections -fomit-frame-pointer -Os -Wall -Wstrict-prototypes -ffunction-sections -Wno-builtin-macro-redefined -D__TIME__=\"$(BUILD_TIME)\" -D__DATE__=\"$(BUILD_DATE)\"
CFLAGS	+=	$(EXTRA_CFLAGS)
LDFLAGS	=	$(ARCH) -nostartfiles -nostdlib -Wl,-T,link.ld,-Map,$(TARGET).map -Wl,--gc-sections -Wl,-static

# Libraries
LIBS	=	../cios-lib/cios-lib.a

# Target
TARGET	=	usb-module

# Objects
OBJS	= main.o				\
		start.o				\
		usb2.o				\
		usbglue.o			\
		usbstorage.o			\
		wbfs.o


$(TARGET).elf: $(OBJS) $(LIBS)
	@printf ' LD\t%s\n' $@
	@$(LD) $(LDFLAGS) $(OBJS) $(LIBS) -lgcc -o $@.orig
	@$(STRIP) $@.orig $@

%.o: %.s
	@printf ' CC\t%s\n' $@
	@$(CC) $(CFLAGS) -MMD -MP -D_LANGUAGE_ASSEMBLY -c -x assembler-with-cpp -o $@ $<

%.o: %.c
	@printf ' CC\t%s\n' $@
	@$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	@echo "Cleaning..."
	@rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET).elf $(TARGET).elf.orig $(TARGET).map

# Rebuild an object when a header or an #included .c file changes (-MMD
# writes each object's dependencies to a .d file), or when this Makefile does.
$(OBJS): Makefile
-include $(OBJS:.o=.d)
