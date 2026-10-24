/*
 * FatFs's integer types with the sizes they have on the Wii. integer.h makes
 * DWORD an unsigned long, 64 bits on this host; including this first defines
 * its guard, so FatFs and the module use these instead.
 */
#ifndef FAT_TYPES_H
#define FAT_TYPES_H
#include <stdint.h>
#define _INTEGER
typedef int		INT;
typedef unsigned int	UINT;
typedef char		CHAR;
typedef unsigned char	UCHAR;
typedef unsigned char	BYTE;
typedef short		SHORT;
typedef unsigned short	USHORT;
typedef unsigned short	WORD;
typedef unsigned short	WCHAR;
typedef int32_t		LONG;
typedef uint32_t	ULONG;
typedef uint32_t	DWORD;
#endif
