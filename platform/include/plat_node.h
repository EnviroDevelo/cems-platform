/* plat_node.h - the NODE role (plat_role.h): the board is an I/O board for a CORE.
 *
 * The node core is node/node.c (register map 2, write rules, hold time, gate, comms watchdog) and
 * node/node_net.c (Modbus TCP and discovery, iob_net.h), the same code the host tests run the
 * master against. This module only moves bytes between them and the platform's sockets
 * (plat_net.h: lwIP on the H5, POSIX on the PC), and gives the core its hardware:
 *
 *   - outputs 1-16 = the board's digital outputs 0-15 (bsp_io_write), relays 1-2 = outputs 16-17;
 *     inputs 1-8 = the board's digital inputs 0-7 (bsp_io_read), debounced with FILTER_MS.
 *     No diagnostics yet (no open circuit, overload, wire break; both field supplies present):
 *     the shared PCB's chips come with its drivers (node/drivers).
 *   - the address, serial number and IP settings in the backup registers (bsp_backup_read/write:
 *     TAMP on the H5, kept over a reset, lost at power-off; the shared PCB moves them to F-RAM).
 *     With nothing saved: address PLAT_NODE_ADDRESS (default 1) and, when PLAT_NODE_DEFAULT_IP is
 *     defined, that IP (PLAT_NODE_DEFAULT_MASK, no gateway, any supervisor), so a bench board
 *     answers as board 1 straight away. Otherwise it is a new board: discovery only, no Modbus TCP,
 *     until the CORE's "Find boards" gives it an IP.
 *   - the CORE's application never runs (plat_start gives the scan no hardware), so only the CORE,
 *     through the node core's gate and hold time, switches anything here.
 *
 * Threads: everything runs in the one task that calls plat_node_poll (the iobus task on the
 * target); plat_node_ipcfg may be called from any task (the net task: the board's IP). */
#ifndef PLAT_NODE_H
#define PLAT_NODE_H
#include <stdbool.h>
#include <stdint.h>
#include "node.h"
#include "node_net.h"

#ifndef PLAT_NODE_ADDRESS
#define PLAT_NODE_ADDRESS 1u
#endif
#ifndef PLAT_NODE_DEFAULT_MASK
#define PLAT_NODE_DEFAULT_MASK 0xFFFFFF00u
#endif
#ifndef PLAT_NODE_HW_REV
#define PLAT_NODE_HW_REV 90u            /* a bench board (the supervisor shows it as such) */
#endif

typedef struct {
    node_t      node;
    node_net_t  net;
    int         lfd, cfd, ufd;          /* -1: closed */
    uint16_t    tcp_port, disc_port;    /* IOB_TCP_PORT, IOB_DISC_PORT (tests change them) */
    uint32_t    disc_to;                /* discovery answers go here: 0xFFFFFFFF (tests: 127.0.0.1) */
    uint32_t    accepted, dropped;      /* statistics */
} plat_node_t;

/* the node's own backup words (the port's: TAMP BKP0R.. on the H5) */
uint32_t bsp_backup_read(unsigned idx);
bool     bsp_backup_write(unsigned idx, uint32_t v);   /* written and read back */

void plat_node_init(plat_node_t *p, uint32_t now_ms);  /* the node core, its saved settings; no sockets yet */
/* Sockets (opened here on first use, once the network is up), then the node core's pass.
 * Waits up to wait_ms for network traffic; call it at least every millisecond or two. */
void plat_node_poll(plat_node_t *p, uint32_t now_ms, unsigned wait_ms);
void plat_node_close(plat_node_t *p);
/* The IP settings the board should have now (ip 0: none, a new board). */
bool plat_node_ipcfg(node_ipcfg_t *c);
const node_hw_t *plat_node_hw(void);   /* the board's pins as node_hw_t */
/* The board's MAC (iob_mac_from_uid of the chip ID): discovery reports it, and in the NODE role the
 * Ethernet interface uses it too (plat_net_mac), so "Find boards" matches the switch and ARP. */
void plat_node_mac(uint8_t mac[6]);

#endif
