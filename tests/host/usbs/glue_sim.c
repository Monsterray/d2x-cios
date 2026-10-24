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
int glue_bulk(unsigned char ep, unsigned short len, void *buf)
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
	return ehci_control_message(&usbs_dev, type, req, value, index, len, buf);
}

void glue_usleep(unsigned int us)
{
	ehci_usleep(us);
}
