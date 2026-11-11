/*------------------------------------------------------------------------*/
/* OS dependent controls for FatFs (after ChaN's ffsystem.c)           */
/* (C)ChaN, 2010                                                          */
/*------------------------------------------------------------------------*/

#include "ff.h"
#include "mem.h"


#if FF_USE_LFN == 3	/* LFN with a working buffer on the heap */
/*------------------------------------------------------------------------*/
/* Allocate a memory block                                                */
/*------------------------------------------------------------------------*/
/* If a NULL is returned, the file function fails with FR_NOT_ENOUGH_CORE.
*/

void* ff_memalloc (	/* Returns pointer to the allocated memory block */
	UINT size		/* Number of bytes to allocate */
)
{
	return Mem_Alloc(size);
}


/*------------------------------------------------------------------------*/
/* Free a memory block                                                    */
/*------------------------------------------------------------------------*/

void ff_memfree(
	void* mblock	/* Pointer to the memory block to free */
)
{
	Mem_Free(mblock);
}

#endif
