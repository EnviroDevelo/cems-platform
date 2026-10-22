/* iob_node_sim.h - a simulated CEMS I/O board (Modbus RTU slave), host only.
 *
 * Implements the register map in iobus.h exactly as the node firmware must (I/O module
 * requirements document), so it is both the test partner for the master and a
 * reference for the node firmware. A bus of several nodes plugs into iob_port_t.
 *
 * Behaviour (the node requirements, in code):
 *  - one register space 0..79; FC03 and FC04 read it, reserved addresses read 0;
 *    FC06, FC16 and FC23 write registers 0..2 and 32..36 only (else exception 02);
 *    out-of-range values give exception 03 and change nothing; other functions 01
 *  - FC23 writes before it reads; a request with any error changes nothing
 *  - a valid write that includes register 0 restarts the hold timer; when it runs out
 *    (HOLD_DS x 0.1 s) every output and relay goes off, IOB_ST_TIMEOUT latches and the
 *    gate stays closed until the next valid write of register 0
 *  - CONFIG_NEEDED is set at every start and cleared by any write to 32..35
 *  - open-circuit and wire-break bits are reported only for channels enabled in 35/34
 *  - no reply to another address, a bad CRC or a broadcast (address 0) */
#ifndef IOB_NODE_SIM_H
#define IOB_NODE_SIM_H
#include <stdint.h>
#include <stdbool.h>
#include "iobus.h"

typedef struct {
    uint8_t  address;
    bool     powered;            /* false: never answers */
    /* registers the master writes */
    uint16_t out_cmd, relay_cmd, seq, echo;
    uint16_t hold_ds, filter_ms, wb_en, ol_en;
    /* node state */
    bool     config_needed, timeout_latched, wdg_latched, gate;
    uint32_t last_write_ms;
    uint16_t rx_errors, resets;
    /* the outside world (tests set these) */
    uint8_t  inputs;             /* raw input states */
    uint8_t  a_open, a_overload, a_overtemp, wirebreak;
    uint16_t b_fault;            /* IOB_BF_* */
    bool     field_a, field_b, in_chip_fault, out_a_chip_fault;
    /* identity (tests may change it to provoke a mismatch) */
    uint16_t magic, map_version, n_out_a, n_out_b, n_relays, n_inputs, fw_version, hw_rev;
    uint16_t uid[6];
    /* statistics */
    uint32_t frames, replies, writes0;
} iob_node_t;

void     iob_node_init(iob_node_t *n, uint8_t address);       /* powered, fresh start */
void     iob_node_restart(iob_node_t *n, bool by_watchdog, uint32_t now_ms);
void     iob_node_tick(iob_node_t *n, uint32_t now_ms);        /* the comms timeout */
uint16_t iob_node_frame(iob_node_t *n, uint32_t now_ms, const uint8_t *rx, uint16_t len,
                        uint8_t *tx, uint16_t cap);            /* reply length, 0 = no reply */
uint16_t iob_node_reg(const iob_node_t *n, uint16_t reg);      /* what a read of reg returns */

/* A simulated RS-485 bus of up to 4 nodes. */
typedef struct {
    iob_port_t  port;
    iob_node_t *nodes[4];
    unsigned    n;
    uint32_t  (*now)(void);      /* clock for the nodes (NULL: bsp_ms) */
    unsigned    drop_next;       /* the next N requests get no reply */
    unsigned    corrupt_next;    /* the next N replies have a bad CRC */
    uint32_t    requests;
} iob_simbus_t;

void iob_simbus_init(iob_simbus_t *b);
void iob_simbus_add(iob_simbus_t *b, iob_node_t *n);

#endif
