/*
 * d2x debug area, module side: fault points (see d2xdbg.h).
 * Only in -DD2X_DEBUG builds; events are the D2XDBG_EV() macro alone.
 */
#ifdef D2X_DEBUG

#include "d2xdbg.h"
#include "syscalls.h"
#include "timer.h"
#include "tools.h"
#include "types.h"

/* Ask MLOAD whether a fault is set for this point. The generic actions
   (delay, block, spin) happen here; the rest go back to the module. */
u32 Dbg_Point(u32 mod, u32 point, u32 a)
{
	u32 r = Swi_MLoad(D2XDBG_SWI_POINT, (mod << 8) | point, a, 0);

	switch (D2XDBG_FAULT_ACTION(r)) {
	case D2XDBG_F_DELAY:
		msleep(D2XDBG_FAULT_ARG(r));
		return 0;

	case D2XDBG_F_BLOCK:
		os_thread_stop(os_get_thread_id());
		/* fall through, should it ever come back */
	case D2XDBG_F_SPIN:
		for (;;)
			;
	}

	return r;
}

#endif
