/* w5500_sim.h - a simulated W5500 for the host builds: it decodes the SPI frames the driver
 * (w5500.h) sends and keeps the registers and buffers the way the chip does (commands, socket
 * states, interrupt bits, TX/RX pointers that wrap in 2 KB buffers, the UDP datagram header).
 * What goes out on the wire is handed to a backend; w5500_sim_simnet() connects it to the
 * simulated I/O network of iob_node_sim.h, so the master runs over driver, transport and chip
 * against the simulated boards. */
#ifndef W5500_SIM_H
#define W5500_SIM_H
#include <stdbool.h>
#include <stdint.h>
#include "w5500.h"
#include "iob_node_sim.h"

typedef struct w5500_sim {
    w5500_bus_t bus;                 /* for w5500_init */
    bool     present;                /* false: nothing answers (MISO reads 0xFF) */
    bool     link;                   /* PHY link */
    uint8_t  common[0x40];
    uint8_t  sreg[W5500_SOCKETS][0x30];
    uint8_t  tx[W5500_SOCKETS][2048], rx[W5500_SOCKETS][2048];
    uint16_t tx_rd[W5500_SOCKETS], rx_wr[W5500_SOCKETS];
    /* the frame being clocked */
    bool     sel;
    unsigned pos;
    uint8_t  hdr[3];
    uint16_t addr;
    /* backend: what the chip puts on the wire (NULL: nothing answers) */
    bool (*tcp_connect)(struct w5500_sim *c, uint8_t s, uint32_t ip, uint16_t port);
    void (*tcp_send)(struct w5500_sim *c, uint8_t s, const uint8_t *d, uint16_t n);
    void (*udp_send)(struct w5500_sim *c, uint8_t s, uint32_t ip, uint16_t port, const uint8_t *d, uint16_t n);
    void (*poll)(struct w5500_sim *c, uint8_t s);   /* before Sn_RX_RSR is read: datagrams may arrive */
    void *ctx;
    uint32_t (*clock)(void);         /* bus.ms (NULL: none) */
    void (*lock)(bool on);           /* netsim: the console thread changes the boards (NULL: none) */
    /* statistics and test hooks */
    uint32_t spi_bytes, commands, connects, resets;
    unsigned refuse_connects;        /* the next N CONNECTs fail */
} w5500_sim_t;

void w5500_sim_init(w5500_sim_t *c);
/* bytes arriving on socket s: TCP stream bytes, or one UDP datagram from ip:port */
void w5500_sim_rx_push(w5500_sim_t *c, uint8_t s, const uint8_t *d, uint16_t n);
void w5500_sim_udp_push(w5500_sim_t *c, uint8_t s, uint32_t ip, uint16_t port, const uint8_t *d, uint16_t n);
void w5500_sim_peer_close(w5500_sim_t *c, uint8_t s);   /* the board closed the connection */
uint32_t w5500_sim_ip(const w5500_sim_t *c);             /* SIPR */
/* backend onto a simulated I/O network: Modbus TCP to the board at the connected IP, discovery
 * broadcasts; the boards see the chip's own IP as the supervisor's */
void w5500_sim_simnet(w5500_sim_t *c, iob_simnet_t *net);

#endif
