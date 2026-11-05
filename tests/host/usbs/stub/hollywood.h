/* cios-lib's hollywood.h for a host build: the Hollywood timer is the
 * simulated clock (glue_sim.c). */
#ifndef USBS_STUB_HOLLYWOOD_H
#define USBS_STUB_HOLLYWOOD_H
unsigned int glue_timer(void);
#define HW_TIMER 0
#define usbs_get_timer() glue_timer()
#endif
