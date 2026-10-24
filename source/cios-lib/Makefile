# devkitARM path
DEVKITARM ?=	/opt/devkitARM

# Prefix
PREFIX	=	$(DEVKITARM)/bin/arm-eabi-

# Executables
CC	=	$(PREFIX)gcc
AR	=	$(PREFIX)ar

# Flags
ARCH	=	-mcpu=arm926ej-s -mthumb -mthumb-interwork -mbig-endian
CFLAGS	=	$(ARCH) -I. -ffreestanding -fno-builtin -ffunction-sections -fomit-frame-pointer -Os -Wall -Wstrict-prototypes
ARFLAGS	=	rcs

# Libraries
LIBS	=

# Target
TARGET	=	cios-lib

# Objects
OBJS	=	direct_syscalls.o	\
		fat_tools.o		\
		ios.o			\
		ipc.o			\
		isfs.o			\
		libc.o			\
		log.o			\
		mem.o			\
		sdhc_server.o	\
		sdio.o			\
		stealth.o		\
		str_utils.o		\
		swi_mload.o		\
		syscalls.o		\
		syscalls_dbg.o	\
		timer.o			\
		tools.o			\
		usbstorage.o	\
		vsprintf.o

OBJS2	=	libwbfs/libwbfs.o	\
		libwbfs/rijndael.o	\
		libwbfs/wiidisc.o


$(TARGET).a: $(OBJS) $(OBJS2)
	@echo -e " AR\t$@"
	@$(AR) $(ARFLAGS) $@ $(OBJS) $(OBJS2)

%.o: %.s
	@echo -e " CC\t$@"
	@$(CC) $(CFLAGS) -MMD -MP -D_LANGUAGE_ASSEMBLY -c -x assembler-with-cpp -o $@ $<

libwbfs/%.o: libwbfs/%.c
	@echo -e " CC\t$@"
	@$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.c
	@echo -e " CC\t$@"
	@$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	@echo -e "Cleaning..."
	@rm -f $(OBJS) $(OBJS2) $(OBJS:.o=.d) $(OBJS2:.o=.d) $(TARGET).a

# Rebuild an object when a header or an #included .c file changes (-MMD
# writes each object's dependencies to a .d file), or when this Makefile does.
$(OBJS) $(OBJS2): Makefile
-include $(OBJS:.o=.d) $(OBJS2:.o=.d)
