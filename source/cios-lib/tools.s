/*   
	Custom IOS Library

	Copyright (C) 2008 neimod.
	Copyright (C) 2009 WiiGator.
	Copyright (C) 2009 Waninkoko.
	Copyright (C) 2010 Hermes.
	Copyright (C) 2011 davebaol.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

	.text

	.align 4
	.code 32

/* Direct syscalls */
	.section .text.DCInvalidateRange,"ax",%progbits
	.balign 4
	.code 32
	.global DCInvalidateRange
DCInvalidateRange:
	mcr	p15, 0, r0, c7, c6, 1
	add	r0, #0x20
	subs	r1, #1
	bne	DCInvalidateRange
	bx	lr

	.section .text.DCFlushRange,"ax",%progbits
	.balign 4
	.code 32
	.global DCFlushRange
DCFlushRange:
	mcr	p15, 0, r0, c7, c10, 1
	add	r0, #0x20
	subs	r1, #1
	bne	DCFlushRange
	bx	lr

	.section .text.ICInvalidate,"ax",%progbits
	.balign 4
	.code 32
	.global ICInvalidate
ICInvalidate:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0
	bx	lr


/* MLoad syscalls */
	.section .text.Swi_MLoad,"ax",%progbits
	.balign 4
	.code 32
	.global Swi_MLoad
Swi_MLoad:
	svc	0xcc
	bx	lr


/* ARM permissions */
	.section .text.Perms_Read,"ax",%progbits
	.balign 4
	.code 32
	.global Perms_Read
Perms_Read:
	mrc	p15, 0, r0, c3, c0
	bx	lr

	.section .text.Perms_Write,"ax",%progbits
	.balign 4
	.code 32
	.global Perms_Write
Perms_Write:
	mcr	p15, 0, r0, c3, c0
	bx	lr


/* MMU: the translation table base, and a TLB flush (the d2x debug area) */
	.section .text.MMU_GetTTB,"ax",%progbits
	.balign 4
	.code 32
	.global MMU_GetTTB
MMU_GetTTB:
	mrc	p15, 0, r0, c2, c0
	bx	lr

	.section .text.MMU_InvalidateTLB,"ax",%progbits
	.balign 4
	.code 32
	.global MMU_InvalidateTLB
MMU_InvalidateTLB:
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0
	bx	lr

	.section .text.DCDrainWriteBuffer,"ax",%progbits
	.balign 4
	.code 32
	.global DCDrainWriteBuffer
DCDrainWriteBuffer:
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4
	bx	lr


/* MEM2 routines */
	.section .text.MEM2_Prot,"ax",%progbits
	.balign 4
	.code 32
	.global MEM2_Prot
MEM2_Prot:
	ldr	r1, =0xD8B420A
	strh	r0, [r1]
	bx	lr


/* Tools */
	.section .text.VirtToPhys,"ax",%progbits
	.balign 4
	.code 32
	.global VirtToPhys
VirtToPhys:
	and	r0, #0x7fffffff
	bx	lr

	.section .text.PhysToVirt,"ax",%progbits
	.balign 4
	.code 32
	.global PhysToVirt
PhysToVirt:
	orr	r0, #0x80000000
	bx	lr
