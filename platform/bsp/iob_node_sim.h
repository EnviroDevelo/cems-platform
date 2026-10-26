/* iob_node_sim.h - a simulated CEMS I/O board (Modbus RTU slave), host only.
 *
 * Implements the register map in iobus.h exactly as the node firmware must (I/O module
 * requirements document), so it is both the test partner for the master and a
 * reference for the node firmware. A bus of several nodes plugs into iob_port_t.
 *
 * Behaviour (the node requirements, in code):
 *  - one register space 0..106 (map 2; 0..79 with map_version 1); FC03 and FC04 read it, reserved
 *    addresses read 0; register 38 reads the node's address (set over the node's USB port only, so
 *    read-only here); FC06, FC16 and FC23 write registers 0..2 and 32..36 only, plus 3-4, 105-106
 *    with a PWM output and 96..104 with temperature inputs (else exception 02);
 *    out-of-range values give exception 03 and change nothing; other functions 01
 *  - FC23 writes before it reads; a request with any error changes nothing
 *  - a valid write that includes register 0 restarts the hold timer; when it runs out
 *    (HOLD_DS x 0.1 s) every output and relay goes off, IOB_ST_TIMEOUT latches and the
 *    gate stays closed until the next valid write of register 0
 *  - CONFIG_NEEDED is set at every start and cleared by any write to 32..35
 *  - open-circuit and wire-break bits are reported only for channels enabled in 35/34
 *  - no reply to another address, a bad CRC or a broadcast (address 0)
 *  - the link is up while a valid request to this node came within 1 s; once it has been
 *    up, losing it latches IOB_ST_COMMS_LOST until FAULT_CLEAR bit 1 or a restart
 *  - address 0 = not set: answers only at IOB_COMMISSION_ADDR, reads and the address change
 *    only (other writes: exception 04), not counted as the link
 *  - the address change (iobus.h, FW-61): one FC16 of 40-42 = key, address, address XOR 0xFFFF;
 *    the serial number (52..59) reads back serial[]; writing it is USB only, so exception 02 here
 *  - every change of an input is shown at least once to the master: a read of INPUTS
 *    returns, for an input that changed since the previous read, the other state from the
 *    one last shown (the outside world is sampled at every tick and frame)
 *  - map 2: the PWM follows PWM_CMD only while the gate is open; the temperature registers read
 *    the outside world below as it is (the real node measures them from the sensor types) */
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
    uint16_t pwm_cmd, pwm_dir, pwm_hz, tach_ppr;                   /* map 2 */
    uint16_t temp_type[IOB_TEMP_CONNS], ntc_r25[IOB_TEMP_CONNS], ntc_beta[IOB_TEMP_CONNS];
    /* node state */
    bool     config_needed, timeout_latched, wdg_latched, gate;
    uint32_t last_write_ms;
    bool     comms_seen, comms_lost; /* link watchdog (FW-58) */
    uint32_t last_comms_ms;
    uint8_t  in_rep, in_pend;    /* inputs last shown to the master, changes since (FW-33) */
    uint8_t  serial[16];         /* the board's serial number (tests set it), NUL padded */
    uint16_t rx_errors, resets;
    /* the outside world (tests set these) */
    uint8_t  inputs;             /* raw input states */
    uint8_t  a_open, a_overload, a_overtemp, wirebreak;
    uint16_t b_fault;            /* IOB_BF_* */
    bool     field_a, field_b, in_chip_fault, out_a_chip_fault;
    int16_t  temp[IOB_TEMPS];    /* map 2: as the board reports them (0.1 degC, IOB_TEMP_INVALID) */
    uint8_t  temp_st[IOB_TEMPS]; /* IOB_TS_* */
    int16_t  cj;
    uint16_t temp_flags, temp_count, tach_rpm;
    /* identity (tests may change it to provoke a mismatch) */
    uint16_t magic, map_version, n_out_a, n_out_b, n_relays, n_inputs, fw_version, hw_rev;
    uint16_t n_temp_pwm;         /* IOB_R_N_TEMP_PWM: IOB_TEMPS | 1 << 8 (a map 1 board: map_version 1, 0) */
    uint16_t uid[6];
    /* Ethernet (iob_net.h): settings as the board keeps them; ip 0 = new, serves no Modbus TCP.
     * iob_node_init gives address k the IP 192.168.1.(60 + k), mask /24, no supervisor IP yet. */
    uint32_t ip, mask, gw, master_ip;
    uint8_t  mac[6];
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
/* A simulated Ethernet of up to 4 nodes: Modbus TCP (every frame goes through the MBAP
 * conversion both ways, and only the node with the targeted IP answers) and discovery. */
typedef struct {
    iob_port_t  port;
    iob_dgram_t disc;
    iob_node_t *nodes[4];
    unsigned    n;
    uint32_t  (*now)(void);
    uint32_t    target_ip;
    uint32_t    master_ip;       /* the supervisor's address, as the nodes see it */
    uint8_t     q[8][IOB_DISC_LEN];
    unsigned    qn, qi;          /* discovery answers waiting to be received */
    unsigned    drop_next;
    uint32_t    requests, datagrams;
} iob_simnet_t;
void iob_simnet_init(iob_simnet_t *s, uint32_t master_ip);
void iob_simnet_add(iob_simnet_t *s, iob_node_t *n);

void iob_simbus_add(iob_simbus_t *b, iob_node_t *n);

#endif
