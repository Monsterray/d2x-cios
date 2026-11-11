/*
 * mloadtest: the smallest module the SD loader can start. Same layout as
 * source/mload-module/start.s: an ios_info_table that stripios turns into
 * the module's NOTE segment, then the code. See ../../../../docs/mload.md.
 */
	.section ".init"
	.arm

	.EQU	ios_thread_arg,		4
	.EQU	ios_thread_priority,	0x79
	.EQU	ios_thread_stacksize,	0x1000

	.global _start
_start:
	mov	r0, #0		@ int argc
	mov	r1, #0		@ char *argv[]
	ldr	r3, =main
	bx	r3

/*
 * Thread stack (decrements from the high address)
 */
	.section ".ios_bss", "a", %nobits

	.space	ios_thread_stacksize
	.global ios_thread_stack
ios_thread_stack:

/*
 * IOS module table
 */
	.section ".ios_info_table", "ax", %progbits

	.global ios_info_table
ios_info_table:
	.long	0x0
	.long	0x28			@ numentries * 0x28
	.long	0x6

	.long	0xB
	.long	ios_thread_arg

	.long	0x9
	.long	_start

	.long	0x7D
	.long	ios_thread_priority

	.long	0x7E
	.long	ios_thread_stacksize

	.long	0x7F
	.long	ios_thread_stack
