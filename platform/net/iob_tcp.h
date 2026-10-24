/* iob_tcp.h - the I/O bus on Ethernet over sockets (plat_net.h: POSIX on the PC, lwIP on the H5):
 * the Modbus TCP client the master's iob_port_t needs, one connection per board, and the UDP
 * discovery socket (iob_dgram_t). The protocol is in iob_net.h.
 *
 * Sockets are opened on first use (the network may not be up at plat_start) from the iobus task
 * only. A connection that fails or sends garbage is closed and opened again at the next request
 * to that board; the master already retries a board that stopped answering once a second. */
#ifndef IOB_TCP_H
#define IOB_TCP_H
#include <stdint.h>
#include "iobus.h"

typedef struct {
    iob_port_t  port;              /* iob_init_eth(&t.port, &t.disc) */
    iob_dgram_t disc;
    uint16_t    tcp_port;          /* IOB_TCP_PORT (tests change it) */
    uint16_t    disc_port;         /* IOB_DISC_PORT (tests change it) */
    uint32_t    disc_to;           /* where FIND and SET go: 0xFFFFFFFF, the limited broadcast (tests: 127.0.0.1) */
    uint32_t    target;            /* the board the next xfer goes to */
    struct { uint32_t ip; int fd; uint32_t used; } conn[IOB_BOARDS];
    int         udp;
    uint16_t    tid;
    uint32_t    connects, failures, use;   /* statistics */
} iob_tcp_t;

void iob_tcp_init(iob_tcp_t *t);   /* no sockets yet */
void iob_tcp_close(iob_tcp_t *t);  /* closes every socket */

#endif
