/* plat_net.h - the socket API the servers use: POSIX on the PC, lwIP on the H5.
 * lwIP must be built with LWIP_SOCKET=1, LWIP_COMPAT_SOCKETS=1,
 * LWIP_SO_RCVTIMEO=1, LWIP_SO_SNDTIMEO=1 and SO_REUSE=1 (port/stm32h5/lwipopts.h). */
#ifndef PLAT_NET_H
#define PLAT_NET_H

#if defined(PLAT_HOST) || defined(PLAT_POSIX_SOCKETS)   /* PC builds, incl. the FreeRTOS POSIX run */
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #define closesocket(s) close(s)
  #define plat_fcntl(s, c, v) fcntl((s), (c), (v))
#else
  #include "lwip/sockets.h"
  #include "lwip/inet.h"
  #define plat_fcntl(s, c, v) lwip_fcntl((s), (c), (v))
#endif

#include <stdbool.h>
#include <stdint.h>

/* The Ethernet port's link and error counters (Service page, /api/info "eth"). The H5 port has it
 * (ethernetif.c); PC builds don't, so callers take it weak and leave it out when it is missing.
 * Counters run from power-up. Where the loss is shows which counter moves: PHY symbol errors and
 * CRC errors = the cable, magnetics or PHY; missed/overflow/pool empty = this firmware too slow. */
typedef struct {
    bool     link_up;
    uint16_t phy_mbps, mac_mbps;        /* what the PHY negotiated, what the MAC was set to */
    bool     phy_full, mac_full;        /* duplex, the same two */
    uint16_t phy_symbol_errors;         /* LAN8742 symbol error counter (wraps) */
    uint32_t rx_good_unicast, tx_good;
    uint32_t rx_crc_errors, rx_align_errors;
    uint32_t rx_missed;                 /* DMA: no free receive descriptor */
    uint32_t rx_overflow;               /* MAC receive FIFO overflow */
    uint32_t rx_pool_empty;             /* receive buffer pool empty */
    uint32_t rx_kicks;                  /* receive DMA restarted by the poll */
} plat_eth_diag_t;
bool plat_eth_diag(plat_eth_diag_t *d);

#endif
