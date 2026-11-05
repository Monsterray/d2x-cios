/*
 * The simulated drive (../ums/sim.c) behind the USBS module's USB calls.
 * Built with the simulator's types; usbs_unit.c, built with cios-lib's types,
 * calls these with plain C types only.
 */
#include "sim.h"
#include "ehci_dev.h"

static struct ehci_device usbs_dev;     /* the host's toggles for this drive */

void glue_reset(void)
{
	usbs_dev.toggles = 0;
	sim.dev = &usbs_dev;
}

static void swap_words(u8 *p, int words)
{
	for (int i = 0; i < words; i++) {
		u32 v;
		memcpy(&v, p + 4 * i, 4);
		v = __builtin_bswap32(v);
		memcpy(p + 4 * i, &v, 4);
	}
}

/*
 * The USBS code assumes a big-endian CPU: it stores the CBW's 32-bit fields
 * and loads the CSW's and READ CAPACITY's with native word accesses (and its
 * own Swap32() for USB's little-endian fields). On this little-endian host
 * those words come out byte-reversed, so swap them back at the boundary,
 * exactly where the Wii's byte order would have put them.
 */
static int glue_bulk_raw(unsigned char ep, unsigned short len, void *buf);

/*
 * IOS's USB driver reports a transfer as a byte count, or a negative code:
 * -7003 and -7004 are STALL (WiiBrew, /dev/usb/oh0, from gc-linux), other
 * -70xx codes are other transfer errors. The simulated EHCI side reports a
 * STALL as a short count with the endpoint halted, and no answer as -ETIMEDOUT
 * or -ENODEV; translate both to what the USBS code would have received.
 * -7022 stands for "no answer" (an assumption: the real code is not known).
 */
/* What the module sent, for the transfer-count tests: every bulk transfer
 * that is not a CBW or a CSW, in order. */
int glue_mem2;
unsigned glue_data_xfers;
unsigned glue_data_len[64];
unsigned glue_data_ptr_aligned[64];
/* n > 0: IOS reports the next data transfer n bytes short (the drive has
 * taken or sent all of it; only the count is short), once. */
unsigned glue_short_next;

int glue_bulk(unsigned char ep, unsigned short len, void *buf)
{
	int is_cbw = !(ep & 0x80) && len == 31 && memcmp(buf, "CBSU", 4) == 0;
	int is_csw = (ep & 0x80) && len == 13;
	if (!is_cbw && !is_csw) {
		if (glue_data_xfers < 64) {
			glue_data_len[glue_data_xfers] = len;
			glue_data_ptr_aligned[glue_data_xfers] = (((unsigned long) buf) & 0x1F) == 0;
		}
		glue_data_xfers++;
	}
	int r = glue_bulk_raw(ep, len, buf);
	if (r < 0)
		return -7022;
	if (!is_cbw && !is_csw && glue_short_next && r == len) {
		r -= glue_short_next;
		glue_short_next = 0;
		return r;
	}
	if (r < len && ((ep & 0x80) ? sim.halt_in : sim.halt_out))
		return -7004;
	return r;
}

/* 1: IOS's cancel-endpoint request also clears the endpoint's halt (not
 * known; REVIEW.md, USBS open question). 0: it does not. */
int glue_cancel_clears_halt = 1;

static int glue_bulk_raw(unsigned char ep, unsigned short len, void *buf)
{
	u8 *p = buf;
	if (!(ep & 0x80) && len == 31 && memcmp(p, "CBSU", 4) == 0) {
		u8 cbw[31];
		memcpy(cbw, p, sizeof(cbw));
		swap_words(cbw, 3);             /* signature, tag, data length */
		return ehci_bulk_message(&usbs_dev, ep, len, cbw);
	}
	s32 r = ehci_bulk_message(&usbs_dev, ep, len, buf);
	if ((ep & 0x80) && r == 13 && memcmp(p, "USBS", 4) == 0)
		swap_words(p, 3);               /* signature, tag, residue */
	else if ((ep & 0x80) && r == 8 && sim.last_cb_op == 0x25)
		swap_words(p, 2);               /* READ CAPACITY: last LBA, block size */
	return r;
}

int glue_ctrl(unsigned char type, unsigned char req, unsigned short value, unsigned short index,
	      unsigned short len, void *buf)
{
	int r = ehci_control_message(&usbs_dev, type, req, value, index, len, buf);
	/* IOS keeps the host side's toggle itself; a clear-halt puts it to DATA0
	 * with the drive's (USB 2.0 9.4.5) */
	if (type == 0x02 && req == 0x01 && value == 0)
		set_toggle(&usbs_dev, (u8) index, 0);
	return r;
}

void glue_usleep(unsigned int us)
{
	ehci_usleep(us);
}

/* The Hollywood timer (HW_TIMER) runs at 1898 ticks per ms */
unsigned int glue_timer(void)
{
	return (unsigned int) (sim.now_us * 1898 / 1000);
}
