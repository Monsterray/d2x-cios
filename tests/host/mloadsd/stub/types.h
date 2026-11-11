/*
 * cios-lib's types.h with the sizes it has on the Wii (s32 is a long there,
 * 64 bits on this host) and the host's own bool. Found before cios-lib's by
 * the include path, so the real headers (syscalls.h, ipc.h, ...) build here.
 */
#ifndef _IOS_TYPES_H_
#define _IOS_TYPES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef char			s8;
typedef short			s16;
typedef int32_t			s32;
typedef unsigned char		u8;
typedef unsigned short		u16;
typedef uint32_t		u32;
typedef uint64_t		u64;
typedef volatile unsigned char	vu8;
typedef volatile unsigned short	vu16;
typedef volatile uint32_t	vu32;

typedef uint32_t		sec_t;

#define ATTRIBUTE_ALIGN(v)	__attribute__((aligned(v)))
#define ATTRIBUTE_PACKED	__attribute__((packed))

#endif
