OUTPUT_FORMAT("elf32-bigarm")
OUTPUT_ARCH(arm)
ENTRY(_start)

/* The module lives high in MLOAD's 512 KB area (0x13700000-0x13780000), away
   from where loaders put their own ELFs (docs/mload.md). */
MEMORY {
	table		: ORIGIN = 0x0, LENGTH = 0x4000
	exe(rwx)	: ORIGIN = 0x13760000, LENGTH = 0x4000
}

SECTIONS {
	.ios_info_table : {
		KEEP (*(.ios_info_table))
	} > table

	.init : {
		*(.init)
		. = ALIGN(4);
	} > exe

	.text : {
		*(.text*)
		*(.glue_7)
		*(.glue_7t)
		. = ALIGN(4);
	} > exe

	.rodata : {
		*(.rodata*)
		. = ALIGN(4);
	} > exe

	.data : {
		*(.data*)
		. = ALIGN(4);
	} > exe

	.bss : {
		*(.bss*)
		*(COMMON)
		KEEP(*(.ios_bss))
		. = ALIGN(4);
	} > exe
}
