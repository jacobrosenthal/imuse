#ifndef IMUSE_INTERNAL_OPL_BUFFER_H
#define IMUSE_INTERNAL_OPL_BUFFER_H
#include <opl3.h>

/* Nuked's overflow fallback applies writes without clocking envelopes. Leave
 * excess events in the outer FIFO until synthesis makes room for their edges. */
static unsigned int im_opl_write_space(const opl3_chip* chip) {
	unsigned int used = (chip->writebuf_last + OPL_WRITEBUF_SIZE - chip->writebuf_cur) % OPL_WRITEBUF_SIZE;
	if (!used && (chip->writebuf[chip->writebuf_last].reg & 0x200))
		return 0;
	return OPL_WRITEBUF_SIZE - used;
}
#endif
