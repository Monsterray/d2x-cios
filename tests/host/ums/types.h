/* The types and macros tinyehci.c defines before it includes ehci.c, for a
 * little-endian host build of usbstorage.c. */
#ifndef HOST_TYPES_H
#define HOST_TYPES_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;

/* The module runs big-endian and swaps USB's little-endian fields. */
#define cpu_to_le32(a) (a)
#define le32_to_cpu(a) (a)
#define cpu_to_le16(a) (a)
#define le16_to_cpu(a) (a)
#define cpu_to_be32(a) __builtin_bswap32(a)
#define be32_to_cpu(a) __builtin_bswap32(a)

#define debug_printf(...) ((void)0)
#define print_hex_dump_bytes(a, b, c, d) ((void)0)

#endif
