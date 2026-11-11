OUTPUT_FORMAT("elf32-bigarm")
OUTPUT_ARCH(arm)
ENTRY(_start)
 
/* Sections area.
 * exe and ram together fill 0x137b5000 to 0x137dd000, as before; the split
 * moved 0x1000 from ram to exe when exFAT came in (docs/fatfs.md). */ 
MEMORY {
	table		: ORIGIN = 0x0, LENGTH = 0x4000
	exe(rwx)	: ORIGIN = 0x137b5000, LENGTH = 0x6000
	ram(rw)		: ORIGIN = 0x137bb000, LENGTH = 0x22000
}

__exe_start_phys__		= 0x137b5000;
__ram_start_phys__		= 0x137bb000;


SECTIONS {
	.ios_info_table :  {
		KEEP (*(.ios_info_table))
	} > table

	.init : AT (__exe_start_phys__) {
		*(.init)
		. = ALIGN(4);
	} > exe

	.text : { 
		*(.text*) 
		*(.gnu.warning) 
		*(.gnu.linkonce.t.*) 
		*(.init) 
		*(.glue_7) 
		*(.glue_7t)
		. = ALIGN(4);
	} > exe

	.data : AT (__ram_start_phys__) {
		*(.data*)
		*(.data.*)
		*(.gnu.linkonce.d.*)
		. = ALIGN(4);
	} > ram

	.rodata : { 
		*(.rodata)
		*all.rodata*(*)
		*(.roda)
		*(.rodata.*) 
		*(.gnu.linkonce.r.*)
		. = ALIGN(4);
	} > ram

	.bss : {
		*(.dynsbss)
		*(.gnu.linkonce.sb.*)
		*(.bss*)
		*(COMMON)
		KEEP(*(.ios_bss))
		. = ALIGN(4);
	} > ram
}
