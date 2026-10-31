/* inst_w5500.h - the instruments' network (inst.h) on the I/O port: W5500 sockets 4-7 beside the
 * I/O boards' 0-3, on the same chip and set-up (iob_w5500.h). Connection c of the poller is
 * socket INST_W5500_SOCK0 + c; nothing waits. Only the iobus task calls it. */
#ifndef INST_W5500_H
#define INST_W5500_H
#include "inst.h"
#include "iob_w5500.h"

#define INST_W5500_SOCK0 4u

typedef struct {
    inst_net_t   net;               /* inst_init(&t.net) */
    iob_w5500_t *io;                /* the I/O port (owns the chip) */
    uint16_t     lport;
} inst_w5500_t;

void inst_w5500_init(inst_w5500_t *t, iob_w5500_t *io);

#endif
