/* iobus.h - the RS-485 I/O bus: Modbus RTU master for the CEMS I/O boards (decision D27).
 *
 * Each I/O board is a Modbus RTU slave with 8 outputs of 0.6 A (bank A), 8 outputs of
 * 1 A (bank B), 2 socketed relays and 8 isolated inputs. Up to IOB_BOARDS boards share
 * one isolated RS-485 pair at 115200 baud, 8 data bits, no parity, 1 stop bit. The
 * register map below is the contract with the node firmware (I/O module requirements
 * document); IOB_MAP_VERSION changes when it does.
 *
 * Channels: board k (1..3) fills the platform's generic channels (plat_io.h), so the
 * application does not change:
 *   outputs  first_do .. first_do+17   bank A 1-8, bank B 9-16, relays 17-18
 *   inputs   first_di .. first_di+7
 * Defaults: board 1 outputs 1-18 / inputs 1-8, board 2 outputs 19-36 / inputs 9-16,
 * board 3 outputs 37-54 / inputs 17-24. A board's inputs replace those channels; its
 * outputs are taken from the image after the application and the alarm relays.
 *
 * Cycle: every PLAT_IOB_PERIOD_MS each running board gets one function 23 request,
 * which writes the outputs (registers 0..2) and reads the status block (16..27). At
 * 115200 baud that takes about 5 ms per board.
 *
 * Safe state, three layers:
 *   - the node holds its outputs when the link goes quiet, and turns every output off
 *     when no valid write has come for its hold time (parameter iob_hold, default
 *     5 min: a short break or a main-board restart does not drop the valves); its
 *     hardware gate turns them off within about 50 ms if the node firmware stops;
 *   - this master sends all-off when the scan has not produced outputs for
 *     PLAT_IOB_STALE_MS, so a stopped scan cannot keep valves open over the bus;
 *   - plat_io_safe_state() (fatal errors) sends all-off at once.
 * A board that gives no valid reply for PLAT_IOB_LOST_MS (1 s, whatever the hold time)
 * is "not responding": its inputs read 0 and every channel is marked faulty
 * (di_fault / do_fault), alarm IOB_LOST; the node may still be holding its outputs.
 *
 * Threading
 *   iob_cycle()                         the iobus task, every PLAT_IOB_PERIOD_MS
 *   plat_hw_io_read / plat_hw_io_write  the scan task (scan_hw_bsp)
 *   iob_safe_state()                    any task (fatal paths)
 *   iob_board_view()                    any task (a consistent copy) */
#ifndef IOBUS_H
#define IOBUS_H
#include <stdint.h>
#include <stdbool.h>
#include "plat_io.h"

#define IOB_BOARDS       3u
#define IOB_OUTS         18u   /* per board: 8 bank A + 8 bank B + 2 relays */
#define IOB_INS          8u
#define IOB_MAP_VERSION  1u
#define IOB_MAGIC        0x494Fu   /* "IO" */
#define IOB_BOOT_KEY     0xB007u   /* IOB_R_BOOTLOADER, over USB only */

/* ---------------- node register map (one register space; FC03 and FC04 read it) ---------------- */
enum {
    /* written every cycle (FC23 write part) */
    IOB_R_OUT_CMD     = 0,   /* bits 0-7 bank A, bits 8-15 bank B (1 = on) */
    IOB_R_RELAY_CMD   = 1,   /* bits 0-1 relays 1-2 */
    IOB_R_CMD_SEQ     = 2,   /* the master's write counter; echoed in IOB_R_CMD_ECHO once applied */
    /* status block, read every cycle (FC23 read part) */
    IOB_R_STATUS      = 16,  /* IOB_ST_* */
    IOB_R_INPUTS      = 17,  /* bits 0-7: inputs 1-8 after the filter (1 = active: current flowing, contact
                                closed). Every change is shown at least once to the RS-485 master: a read
                                returns, for an input that changed since the master's previous read, the
                                other state from the one it last saw (so a short opening is not missed) */
    IOB_R_OUT_APPLIED = 18,  /* outputs as driven now (0 while the gate is closed) */
    IOB_R_RELAY_APPLIED = 19,
    IOB_R_A_OPEN      = 20,  /* bank A open circuit, bits 0-7 */
    IOB_R_A_OVERLOAD  = 21,  /* bank A overload or short circuit */
    IOB_R_A_OVERTEMP  = 22,  /* bank A channel over-temperature */
    IOB_R_B_FAULT     = 23,  /* IOB_BF_* (bank B has one fault line for all 8 channels) */
    IOB_R_WIREBREAK   = 24,  /* inputs with a broken wire, bits 0-7 */
    IOB_R_CMD_ECHO    = 25,  /* IOB_R_CMD_SEQ of the last write applied */
    IOB_R_RX_ERRORS   = 26,  /* frames with CRC or framing errors since reset (wraps) */
    IOB_R_RESETS      = 27,  /* node resets since its own last power-up count clear (wraps) */
    /* configuration, written after connecting and whenever a setting changes */
    IOB_R_HOLD_DS     = 32,  /* hold time without a valid output write, 0.1 s units: 1..6000 (0.1 s to 10 min; default 3000 = 5 min) */
    IOB_R_FILTER_MS   = 33,  /* input filter, 0..100 ms (default 8, as the old panel; the node uses the nearest it supports at or above) */
    IOB_R_WB_ENABLE   = 34,  /* wire-break check per input, bits 0-7 (default 0: off) */
    IOB_R_OL_ENABLE   = 35,  /* open-circuit check per bank A output, bits 0-7 (default 0xFF) */
    IOB_R_FAULT_CLEAR = 36,  /* write bit 0: clear TIMEOUT and WDG_RESET; bit 1: clear COMMS_LOST
                                (sent when IOB_LOST is acknowledged); 0..3, reads 0 */
    IOB_R_BOOTLOADER  = 37,  /* USB only (the node's USB-C port): write 0xB007 to start ST's USB bootloader;
                                refused over RS-485 (exception 02), never written by this master */
    /* identification, read once after connecting */
    IOB_R_MAGIC       = 64,  /* IOB_MAGIC */
    IOB_R_MAP_VERSION = 65,  /* IOB_MAP_VERSION */
    IOB_R_HW_REV      = 66,
    IOB_R_FW_VERSION  = 67,  /* major << 8 | minor */
    IOB_R_FW_BUILD    = 68,
    IOB_R_UID         = 69,  /* 6 registers: the node MCU's 96-bit unique ID */
    IOB_R_N_OUT_A     = 75,  /* 8 */
    IOB_R_N_OUT_B     = 76,  /* 8 */
    IOB_R_N_RELAYS    = 77,  /* 2 */
    IOB_R_N_INPUTS    = 78,  /* 8 */
    IOB_R_STATUS_N    = 12,  /* registers in the status block */
    IOB_R_IDENT_N     = 15,
    IOB_R_CONFIG_N    = 4    /* HOLD_DS .. OL_ENABLE */
};

/* IOB_R_STATUS bits */
#define IOB_ST_OUT_ENABLED   0x0001u  /* the safe-state gate is open: outputs can switch */
#define IOB_ST_TIMEOUT       0x0002u  /* latched: the hold time ran out and turned the outputs off */
#define IOB_ST_FIELD_A_OK    0x0004u  /* field 24 V for bank A and the inputs is present */
#define IOB_ST_FIELD_B_OK    0x0008u  /* field 24 V for bank B is present */
#define IOB_ST_WDG_RESET     0x0010u  /* latched: the last reset was the node's watchdog */
#define IOB_ST_CONFIG_NEEDED 0x0020u  /* configuration not written since the node started */
#define IOB_ST_IN_CHIP       0x0040u  /* the input chip does not answer or reports a fault */
#define IOB_ST_OUT_A_CHIP    0x0080u  /* the bank A chip does not answer or reports a fault */
#define IOB_ST_COMMS_LOST    0x0100u  /* latched: the node lost its link to the master (no valid request
                                         for 1 s) since the last FAULT_CLEAR bit 1 */
#define IOB_FC_EVENTS        0x0001u  /* IOB_R_FAULT_CLEAR bits */
#define IOB_FC_COMMS         0x0002u
/* IOB_R_B_FAULT bits */
#define IOB_BF_FAULT         0x0001u  /* bank B fault line: overload or over-temperature on a channel */
#define IOB_BF_WDG           0x0002u  /* bank B's own watchdog turned its outputs off */

/* ---------------- master ---------------- */
typedef struct iob_port {
    /* Send tx, then wait up to timeout_ms for a reply and return its length (0 = none).
     * The reply ends when the line goes idle. */
    uint16_t (*xfer)(struct iob_port *p, const uint8_t *tx, uint16_t tx_len,
                     uint8_t *rx, uint16_t rx_cap, uint32_t timeout_ms);
    void *ctx;
} iob_port_t;

/* board states */
enum { IOB_S_OFF = 0,        /* not enabled */
       IOB_S_CONNECTING,     /* reading the identification / writing the configuration */
       IOB_S_RUN,            /* answering every cycle */
       IOB_S_LOST,           /* not answering: retried every PLAT_IOB_RETRY_MS */
       IOB_S_WRONG_TYPE,     /* answers but is not a matching I/O board: retried every PLAT_IOB_RETRY_MS */
       IOB_S_BAD_SETTINGS }; /* channel range does not fit or overlaps another board's */

/* parameter iobN_status bits (what the screens show) */
#define IOBS_ONLINE      0x0001u
#define IOBS_LOST        0x0002u
#define IOBS_WRONG_TYPE  0x0004u
#define IOBS_BAD_SET     0x0008u
#define IOBS_GATE_OPEN   0x0010u
#define IOBS_FIELD_A_OK  0x0020u
#define IOBS_FIELD_B_OK  0x0040u
#define IOBS_CONNECTING  0x0080u
#define IOBS_WDG_SEEN    0x0100u  /* the node restarted from its watchdog since this unit started */
#define IOBS_TIMEOUT_SEEN 0x0200u /* the node's hold time ran out since this unit started */
#define IOBS_COMMS_LATCHED 0x0400u /* the node holds IOB_ST_COMMS_LOST: its link was lost, not yet acknowledged */

typedef struct {
    uint8_t  state;          /* IOB_S_* */
    uint16_t status;         /* IOBS_* */
    uint8_t  inputs;         /* inputs 1-8 */
    uint32_t outputs;        /* outputs 1-18 as driven */
    uint32_t out_faults;     /* outputs 1-18 with a fault */
    uint8_t  in_faults;      /* inputs 1-8 unreliable */
    uint8_t  a_open, a_overload, a_overtemp, b_fault, wirebreak;
    uint32_t errors;         /* failed requests since start */
    uint32_t requests;
    uint16_t fw_version, hw_rev, node_rx_errors, node_resets;
    uint16_t uid[6];
    uint8_t  last_error;     /* MBR_* of the last failed request */
    uint8_t  last_exception;
} iob_view_t;

void iob_init(iob_port_t *port);       /* plat_start (NULL: no RS-485 port, boards report not responding) */
void iob_cycle(uint32_t now_ms);       /* iobus task */
void iob_safe_state(void);             /* any task: send all-off from now on (until the next reset) */
bool iob_board_view(unsigned board, iob_view_t *v);   /* board 0..IOB_BOARDS-1 */
const char *iob_state_name(uint8_t state);
/* What is wrong with a board, in words, for the screens (up to max lines; returns how many). */
#define IOB_LINE 40
int iob_fault_lines(const iob_view_t *v, uint8_t ol_mask, uint8_t wb_mask, char lines[][IOB_LINE], int max);

/* The scan's hardware I/O (scan_hw_bsp): the main board's pins plus the I/O boards. */
void plat_hw_io_read(plat_io_in_t *in);
void plat_hw_io_write(const plat_io_out_t *out);
void plat_io_safe_state(void);         /* bsp_io_safe_state() + iob_safe_state() */

/* Scan task, from plat_hw_io_read (tests call it directly): settings in, status out,
 * alarms. now_ms stamps the output image for the stale check. */
void iob_scan_in(plat_io_in_t *in, uint32_t now_ms);
void iob_scan_out(const plat_io_out_t *out, uint32_t now_ms);

/* Settings (parameters 100..139, param.c). first_do/first_di are 1-based channel numbers. */
typedef struct {
    bool     enable;
    uint8_t  address;        /* 1..247 */
    uint8_t  first_do;       /* 1..PLAT_DO_MAX-17 */
    uint8_t  first_di;       /* 1..PLAT_DI_MAX-7 */
    uint8_t  ol_mask;        /* open-circuit check, bank A outputs 1-8 */
    uint8_t  wb_mask;        /* wire-break check, inputs 1-8 */
    uint16_t status;         /* RO tags, published by the scan */
    uint32_t out_faults;
    uint8_t  in_faults;
    uint32_t errors;
    uint8_t  inputs;
    uint32_t outputs;
} iob_board_tags_t;
extern iob_board_tags_t iob_tags[IOB_BOARDS];
extern uint16_t iob_hold_s;          /* parameter 100: node hold time, s (1..600) */
extern uint8_t  iob_filter_ms;       /* parameter 101 */

/* Test hook: forget all state (boards back to OFF, safe state cleared). */
void iob_reset(void);

#endif
