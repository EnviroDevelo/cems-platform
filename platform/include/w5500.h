/* w5500.h - WIZnet W5500 driver: the I/O port (a second, separate Ethernet for the I/O boards,
 * docs/HARDWARE.md H5). The chip runs TCP/IP itself in 8 hardware sockets, so the I/O traffic
 * never enters lwIP and lwIP never sees this network.
 *
 * Portable C over a small bus interface (chip select and full-duplex SPI): port/stm32h5 gives
 * the STM32 SPI, platform/bsp/w5500_sim.c a simulated chip for the host tests. Single user:
 * only the iobus task calls it. Variable-length data mode (VDM), SPI mode 0 or 3. */
#ifndef W5500_H
#define W5500_H
#include <stdbool.h>
#include <stdint.h>

#define W5500_SOCKETS 8

typedef struct w5500_bus {
    void (*cs)(struct w5500_bus *b, bool active);
    /* clock n bytes: send tx (NULL: zeros), keep what comes back in rx (NULL: drop it) */
    void (*xfer)(struct w5500_bus *b, const uint8_t *tx, uint8_t *rx, uint16_t n);
    void (*reset)(struct w5500_bus *b, bool active);   /* RSTn (active = held in reset); NULL: none */
    void (*wait)(struct w5500_bus *b, uint32_t ms);     /* sleep up to ms (or until INTn); NULL: none */
    uint32_t (*ms)(void);                               /* a millisecond clock */
    void *ctx;
} w5500_bus_t;

typedef struct {
    w5500_bus_t *bus;
    bool         ok;            /* found (VERSIONR 4) and set up */
    uint32_t     ip, mask, gw;  /* as set, host order */
    uint8_t      mac[6];
    uint32_t     resets;        /* times set up */
} w5500_t;

/* Socket states (Sn_SR) */
enum { W5500_SOCK_CLOSED = 0x00, W5500_SOCK_INIT = 0x13, W5500_SOCK_SYNSENT = 0x15,
       W5500_SOCK_ESTABLISHED = 0x17, W5500_SOCK_CLOSE_WAIT = 0x1C, W5500_SOCK_UDP = 0x22 };

/* Reset (RSTn if there is one, then the software reset), check the chip and set MAC, IP, mask and
 * gateway, ARP/TCP retries (short: the boards are on one segment), 2 KB buffers per socket.
 * False: no chip answers (w->ok false). */
bool w5500_init(w5500_t *w, w5500_bus_t *bus, const uint8_t mac[6], uint32_t ip, uint32_t mask, uint32_t gw);
bool w5500_set_addr(w5500_t *w, uint32_t ip, uint32_t mask, uint32_t gw);   /* a new IP without a reset */
bool w5500_link_up(w5500_t *w);                         /* PHY link (cable in, switch on) */

/* TCP client. connect waits up to timeout_ms for the connection (false: refused, nothing at that
 * IP, or the time ran out; the socket is closed again). */
bool     w5500_tcp_connect(w5500_t *w, uint8_t s, uint32_t ip, uint16_t port, uint16_t local_port, uint32_t timeout_ms);
bool     w5500_tcp_ok(w5500_t *w, uint8_t s);           /* still established */
bool     w5500_send(w5500_t *w, uint8_t s, const uint8_t *buf, uint16_t n, uint32_t timeout_ms);   /* whole or nothing */
uint16_t w5500_rx_avail(w5500_t *w, uint8_t s);
uint16_t w5500_recv(w5500_t *w, uint8_t s, uint8_t *buf, uint16_t cap);   /* up to cap of what has arrived */
void     w5500_close(w5500_t *w, uint8_t s);

/* UDP */
bool     w5500_udp_open(w5500_t *w, uint8_t s, uint16_t local_port);
bool     w5500_udp_send(w5500_t *w, uint8_t s, uint32_t ip, uint16_t port, const uint8_t *buf, uint16_t n, uint32_t timeout_ms);
/* one datagram: its length (cut to cap), 0 = none; from_ip and from_port may be NULL */
uint16_t w5500_udp_recv(w5500_t *w, uint8_t s, uint8_t *buf, uint16_t cap, uint32_t *from_ip, uint16_t *from_port);

/* Register access (exposed for the simulated chip's tests). block: W5500_COMMON, or
 * W5500_SREG(s), W5500_STX(s), W5500_SRX(s). */
#define W5500_COMMON   0u
#define W5500_SREG(s)  ((uint8_t)((s) * 4u + 1u))
#define W5500_STX(s)   ((uint8_t)((s) * 4u + 2u))
#define W5500_SRX(s)   ((uint8_t)((s) * 4u + 3u))
void w5500_read(w5500_t *w, uint8_t block, uint16_t addr, uint8_t *buf, uint16_t n);
void w5500_write(w5500_t *w, uint8_t block, uint16_t addr, const uint8_t *buf, uint16_t n);

/* common registers */
#define W5500_MR        0x0000u
#define W5500_GAR       0x0001u
#define W5500_SUBR      0x0005u
#define W5500_SHAR      0x0009u
#define W5500_SIPR      0x000Fu
#define W5500_RTR       0x0019u
#define W5500_RCR       0x001Bu
#define W5500_PHYCFGR   0x002Eu
#define W5500_VERSIONR  0x0039u
/* socket registers */
#define W5500_Sn_MR       0x00u
#define W5500_Sn_CR       0x01u
#define W5500_Sn_IR       0x02u
#define W5500_Sn_SR       0x03u
#define W5500_Sn_PORT     0x04u
#define W5500_Sn_DIPR     0x0Cu
#define W5500_Sn_DPORT    0x10u
#define W5500_Sn_RXBUF    0x1Eu
#define W5500_Sn_TXBUF    0x1Fu
#define W5500_Sn_TX_FSR   0x20u
#define W5500_Sn_TX_WR    0x24u
#define W5500_Sn_RX_RSR   0x26u
#define W5500_Sn_RX_RD    0x28u
/* Sn_MR, Sn_CR, Sn_IR values */
#define W5500_MR_TCP      0x01u
#define W5500_MR_UDP      0x02u
#define W5500_MR_ND       0x20u
#define W5500_CR_OPEN     0x01u
#define W5500_CR_CONNECT  0x04u
#define W5500_CR_DISCON   0x08u
#define W5500_CR_CLOSE    0x10u
#define W5500_CR_SEND     0x20u
#define W5500_CR_RECV     0x40u
#define W5500_IR_CON      0x01u
#define W5500_IR_DISCON   0x02u
#define W5500_IR_RECV     0x04u
#define W5500_IR_TIMEOUT  0x08u
#define W5500_IR_SENDOK   0x10u

#endif
