/* iob_tcp.h - the I/O bus on Ethernet over sockets (plat_net.h: POSIX on the PC, lwIP on the H5):
 * the Modbus TCP client the master's iob_port_t needs, one connection per board, and the UDP
 * discovery socket (iob_dgram_t). The protocol is in iob_net.h.
 *
 * Sockets are opened on first use (the network may not be up at plat_start) from the iobus task
 * only. A connection that fails or sends garbage is closed and opened again at the next request
 * to that board; the master already retries a board that stopped answering once a second.
 *
 * A board that stops answering (cable out, board off) must not take the stack's memory with it:
 * every request into a dead connection stays queued for retransmission, and lwIP's TCP segments
 * are one pool for every connection (MEMP_NUM_TCP_SEG), so a few hundred ms of 20 ms requests
 * starved the web server on the DK bench (1 Nov). So a connection that missed IOB_TCP_MISS_MAX
 * replies in a row is aborted (RST: linger 0, its queued segments freed at once) and the next
 * request opens a new one; every close here is such an abort. */
#ifndef IOB_TCP_H
#define IOB_TCP_H
#include <stdint.h>
#include "iobus.h"

#define IOB_TCP_MISS_MAX 2u                /* replies missed in a row before the connection is aborted */

typedef struct {
    iob_port_t  port;              /* iob_init_eth(&t.port, &t.disc) */
    iob_dgram_t disc;
    uint16_t    tcp_port;          /* IOB_TCP_PORT (tests change it) */
    uint16_t    disc_port;         /* IOB_DISC_PORT (tests change it) */
    uint32_t    disc_to;           /* where FIND and SET go: 0xFFFFFFFF, the limited broadcast (tests: 127.0.0.1) */
    uint32_t    host;              /* 0: connect to the board's IP; else always here (a virtual board on the PC) */
    uint32_t    target;            /* the board the next xfer goes to */
    struct { uint32_t ip; int fd; uint32_t used; uint8_t miss; } conn[IOB_BOARDS];
    int         udp;
    uint16_t    tid;
    uint32_t    connects, failures, use, aborts;   /* statistics */
} iob_tcp_t;

void iob_tcp_init(iob_tcp_t *t);   /* no sockets yet */
void iob_tcp_close(iob_tcp_t *t);  /* closes every socket */

#endif
