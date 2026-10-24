/* cios-lib's heap, for a host build (fat_unit.c implements it). */
#ifndef FAT_STUB_MEM_H
#define FAT_STUB_MEM_H
#include "types.h"
s32 Mem_Init(u32 *heapspace, u32 heapspaceSize);
void *Mem_Alloc(u32 size);
void Mem_Free(void *ptr);
#endif
