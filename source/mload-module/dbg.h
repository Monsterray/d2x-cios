/*
 * d2x debug area in MLOAD (dbg.c), -DD2X_DEBUG builds only.
 */
#ifndef _DBG_H_
#define _DBG_H_

#include "types.h"

void Dbg_Init(void);
void Dbg_SwiEntry(const u8 *addr, u32 arg0, u32 arg1);
s32  Dbg_Swi(u32 cmd, u32 arg1, u32 arg2, u32 arg3);
s32  Dbg_Ioctl(u32 cmd, u32 *in, u32 inlen, u32 *out, u32 outlen);

#endif
