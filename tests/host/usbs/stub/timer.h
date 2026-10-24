/* cios-lib's timer, for a host build: sleeping advances the simulated clock. */
#ifndef USBS_STUB_TIMER_H
#define USBS_STUB_TIMER_H
#include "types.h"
#define usleep glue_usleep      /* keep clear of libc's usleep */
void glue_usleep(u32 us);
#endif
