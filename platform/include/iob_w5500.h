/* iob_w5500.h - the I/O bus on the I/O port: the Modbus TCP client and the discovery socket the
 * master needs (iob_port_t, iob_dgram_t; the protocol is in iob_net.h), on the W5500's hardware
 * sockets (w5500.h). Sockets 0-2 are the boards' connections, socket 3 discovery; 4-7 belong to the instruments (inst_w5500.h).
 *
 * Same behaviour as iob_tcp.c on lwIP: a connection opens on first use and is opened again after a
 * failure; a late reply to an earlier request is dropped. On top of that it keeps the chip itself:
 * set up (with iob_port_ip / iob_port_prefix and the second MAC) on first use, set up again at most
 * once a second while it does not answer, given a new IP when the setting changes, and
 * iob_port_status kept current. Only the iobus task calls it. */
#ifndef IOB_W5500_H
#define IOB_W5500_H
#include <stdint.h>
#include "iobus.h"
#include "w5500.h"

#define IOB_W5500_DISC_SOCK 3u

typedef struct {
    iob_port_t   port;               /* iob_init_ioport(&t.port, &t.disc) */
    iob_dgram_t  disc;
    w5500_t      chip;
    w5500_bus_t *bus;
    uint8_t      mac[6];
    uint16_t     tcp_port, disc_port;   /* IOB_TCP_PORT, IOB_DISC_PORT (tests change them) */
    uint32_t     disc_to;            /* where FIND and SET go: the limited broadcast */
    uint32_t     target;
    struct { uint32_t ip; bool open; uint32_t used; } conn[IOB_BOARDS];
    bool         udp;
    uint16_t     tid, lport;
    uint32_t     init_at;            /* last set-up try (ms) */
    bool         tried;
    uint32_t     connects, failures, use;   /* statistics */
} iob_w5500_t;

/* bus: the chip's SPI (bsp); mac: the I/O port's own MAC (see iob_w5500_mac). No SPI traffic yet. */
void iob_w5500_init(iob_w5500_t *t, w5500_bus_t *bus, const uint8_t mac[6]);
/* The I/O port's MAC from the chip's 96-bit unique ID: the main port's (locally administered,
 * plat_net_lwip.c, D10) with bit 1 of the last byte flipped, so the two never collide. */
void iob_w5500_mac(const uint8_t uid[12], uint8_t mac[6]);
/* The chip set up with the current I/O port settings (set up again at most once a second while
 * it does not answer); false while it does not answer. For the instrument poller (inst_w5500.h),
 * which shares the chip on sockets 4-7. iobus task only. */
bool iob_w5500_ready(iob_w5500_t *t);

#endif
