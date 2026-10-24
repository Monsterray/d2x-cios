/* cios-lib's heap, for a host build (usbs_unit.c implements it). */
#ifndef USBS_STUB_MEM_H
#define USBS_STUB_MEM_H
#include "types.h"
void *Mem_Alloc(u32 size);
void Mem_Free(void *ptr);
#endif
