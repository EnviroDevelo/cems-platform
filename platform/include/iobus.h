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
 * Map 2 (the H563 board): the same, plus the pump PWM (written with the outputs, 0..4, and read
 * back with the status block, 16..31) and up to IOB_TEMPS temperatures, read every
 * PLAT_IOB_TEMP_MS with one FC03 of 80..92. A map 1 board still runs, without them.
 *   temperatures -> plat_io_in_t rtd_cdeg/rtd_status, channels first_temp .. first_temp+5, with the
 *                   per-channel calibration (offset, gain) applied here; status PLAT_AIN_FAULT
 *                   while the board does not answer, or the channel reports a fault (IOB_TS_*)
 *   pump PWM     <- plat_io_out_t ao_raw[first_ao - 1]: duty in 0.1 %, -1000..1000, the sign
 *                   is the DIR output (negative = DIR on). Off with the outputs (stale, safe).
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
#include "iob_net.h"
#include "plat_cmdq.h"         /* plat_val_t */

#define IOB_BOARDS       3u
#define IOB_OUTS         18u   /* per board: 8 bank A + 8 bank B + 2 relays */
#define IOB_INS          8u
#define IOB_MAP_VERSION  2u        /* 2 adds the temperature inputs and the pump PWM; a map 1 board still runs */
#define IOB_TEMPS        6u        /* temperature channels per board: 3 connectors, 2 each (IOB_R_TEMP) */
#define IOB_TEMP_CONNS   3u
#define IOB_TEMP_INVALID ((int16_t)-32768)   /* IOB_R_TEMP when the channel has no reading */
#define IOB_MAGIC        0x494Fu   /* "IO" */
#define IOB_BOOT_KEY     0xB007u   /* IOB_R_BOOTLOADER, over USB only */
#define IOB_ADDR_MAX     245u      /* node addresses 1..245 */
#define IOB_COMMISSION_ADDR 246u   /* an RS-485 node without an address answers here (one new board at a time) */
#define IOB_USB_ADDR     247u      /* the node's USB port answers here whatever its address */
#define IOB_ADDR_SET_KEY 0xA55Au   /* first word of the address change (FW-61) */
#define IOB_SERIAL_REGS  8u        /* 16 characters */

/* ---------------- node register map (one register space; FC03 and FC04 read it) ---------------- */
enum {
    /* written every cycle (FC23 write part) */
    IOB_R_OUT_CMD     = 0,   /* bits 0-7 bank A, bits 8-15 bank B (1 = on) */
    IOB_R_RELAY_CMD   = 1,   /* bits 0-1 relays 1-2 */
    IOB_R_CMD_SEQ     = 2,   /* the master's write counter; echoed in IOB_R_CMD_ECHO once applied */
    /* map 2, written with 0..2 when the board has a PWM output (IOB_R_N_TEMP_PWM high byte) */
    IOB_R_PWM_CMD     = 3,   /* pump PWM duty, 0..1000 (0.1 %); goes to 0 with the outputs (hold time, gate) */
    IOB_R_PWM_DIR     = 4,   /* bit 0: the DIR output (1 = on); off with the outputs */
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
    /* map 2: read with the status block (16..31) */
    IOB_R_PWM_APPLIED = 28,  /* PWM duty driven now, 0..1000 (0 while the gate is closed) */
    IOB_R_TACH_RPM    = 29,  /* pump speed from the tacho input, rpm (0 = no pulses for 2 s) */
    IOB_R_TEMP_FLAGS  = 30,  /* IOB_TF_*: the temperature front end as a whole */
    IOB_R_TEMP_COUNT  = 31,  /* finished measurement rounds (wraps): stops counting when the front end stops */
    /* configuration, written after connecting and whenever a setting changes */
    IOB_R_HOLD_DS     = 32,  /* hold time without a valid output write, 0.1 s units: 1..6000 (0.1 s to 10 min; default 3000 = 5 min) */
    IOB_R_FILTER_MS   = 33,  /* input filter, 0..100 ms (default 8, as the old panel; the node uses the nearest it supports at or above) */
    IOB_R_WB_ENABLE   = 34,  /* wire-break check per input, bits 0-7 (default 0: off) */
    IOB_R_OL_ENABLE   = 35,  /* open-circuit check per bank A output, bits 0-7 (default 0xFF) */
    IOB_R_FAULT_CLEAR = 36,  /* write bit 0: clear TIMEOUT and WDG_RESET; bit 1: clear COMMS_LOST
                                (sent when IOB_LOST is acknowledged); 0..3, reads 0 */
    IOB_R_BOOTLOADER  = 37,  /* USB only (the node's USB-C port): write 0xB007 to start ST's USB bootloader;
                                refused over RS-485 (exception 02), never written by this master */
    IOB_R_NODE_ADDR   = 38,  /* the node's own Modbus address, 0..IOB_ADDR_MAX (0 = not set: on RS-485 it then
                                answers only at IOB_COMMISSION_ADDR). Kept in the board's F-RAM, so a firmware
                                update never changes it. Written directly over USB only (factory); over RS-485
                                only through IOB_R_ADDR_SET below (a direct write is exception 02) */
    /* address change over RS-485 or USB (node requirements FW-61): ONE FC16 write of the 3 registers
     * 40-42 = IOB_ADDR_SET_KEY, the new address (1..IOB_ADDR_MAX), the new address XOR 0xFFFF.
     * Wrong key, wrong complement or out of range: exception 03, nothing changed. The node saves the
     * address (and reads it back) before replying (save failed: 04, nothing changed). The reply comes
     * from the old address; the node answers at the new one from the next request. Any other write
     * into 40-42 (FC06, FC23, another start or length) is exception 02; the registers read 0. */
    IOB_R_ADDR_SET    = 40,
    /* the board's serial number (FW-62): IOB_SERIAL_REGS registers, 2 ASCII characters each, high byte
     * first, NUL padded; all 0 = not set. Read from anywhere. Written over USB only (factory), FC16 to all
     * 8 registers, printable ASCII; once set it cannot be changed after the board has been powered off
     * (until then a typo can be corrected): exception 02. Kept in the board's F-RAM. */
    IOB_R_SERIAL      = 52,
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
    IOB_R_N_TEMP_PWM  = 79,  /* map 2: temperature channels (low byte, 0 or IOB_TEMPS) and PWM outputs (high byte, 0 or 1);
                                a map 1 board reads 0 here */
    /* map 2: temperatures, read every PLAT_IOB_TEMP_MS (FC03 80..92) */
    IOB_R_TEMP        = 80,  /* IOB_TEMPS registers: channel 2c+1 = connector c+1 pair A, 2c+2 = pair B (thermocouples;
                                an RTD or NTC is on the A channel), signed 0.1 degC, IOB_TEMP_INVALID without a reading */
    IOB_R_TEMP_STATE  = 86,  /* IOB_TEMPS registers: IOB_TS_* (why a channel has no reading, or OK) */
    IOB_R_CJ_TEMP     = 92,  /* the thermocouple cold junction, signed 0.1 degC (IOB_TEMP_INVALID: none) */
    /* map 2 configuration, written after connecting and whenever a setting changes (FC16 96..105) */
    IOB_R_TEMP_TYPE   = 96,  /* IOB_TEMP_CONNS registers: IOB_TT_* per connector */
    IOB_R_NTC_R25     = 99,  /* IOB_TEMP_CONNS registers: an NTC's resistance at 25 degC, 10 ohm units (default 1000 = 10k) */
    IOB_R_NTC_BETA    = 102, /* IOB_TEMP_CONNS registers: an NTC's B25/85 constant, K (default 3435) */
    IOB_R_PWM_FREQ    = 105, /* PWM frequency, Hz, 10..25000 (default 1000) */
    IOB_R_TACH_PPR    = 106, /* tacho pulses per revolution, 1..12 (default 2) */
    IOB_R_STATUS_N    = 12,  /* registers in the status block (map 1) */
    IOB_R_STATUS_N2   = 16,  /* map 2 */
    IOB_R_IDENT_N     = 16,  /* MAGIC .. N_TEMP_PWM (a map 1 board answers 0 at 79) */
    IOB_R_CONFIG_N    = 4,   /* HOLD_DS .. OL_ENABLE */
    IOB_R_TEMPS_N     = 13,  /* TEMP .. CJ_TEMP */
    IOB_R_CONFIG2_N   = 11   /* TEMP_TYPE .. TACH_PPR */
};

/* IOB_R_TEMP_TYPE: the sensor on a connector (wiring: hardware notes of the board, temperature sheet) */
enum {
    IOB_TT_NONE = 0,                        /* nothing fitted: both channels IOB_TS_OFF */
    IOB_TT_TC_K = 1, IOB_TT_TC_J, IOB_TT_TC_T, IOB_TT_TC_E, IOB_TT_TC_S,   /* two thermocouples, A and B */
    IOB_TT_PT100_2W = 10, IOB_TT_PT100_3W, IOB_TT_PT100_4W,                /* one RTD, on channel A */
    IOB_TT_PT1000_2W = 13, IOB_TT_PT1000_3W, IOB_TT_PT1000_4W,
    IOB_TT_NTC = 20                         /* one NTC, 2-wire, on channel A (IOB_R_NTC_R25, IOB_R_NTC_BETA) */
};
/* IOB_R_TEMP_STATE */
enum {
    IOB_TS_OK = 0,
    IOB_TS_OFF,          /* no sensor configured on this channel */
    IOB_TS_WAIT,         /* configured, not measured yet */
    IOB_TS_OPEN,         /* open circuit: broken sensor or wire (burn-out check, or an RTD/NTC reading at full scale) */
    IOB_TS_SHORT,        /* RTD or NTC shorted (resistance far below the sensor's range) */
    IOB_TS_UNDER,        /* below the sensor type's range */
    IOB_TS_OVER,         /* above the sensor type's range */
    IOB_TS_ADC,          /* the converter does not answer, or its supply or reference has failed */
    IOB_TS_CJ            /* thermocouple without a cold-junction temperature */
};
/* IOB_R_TEMP_FLAGS */
#define IOB_TF_ADC       0x0001u   /* the ADS124S08 does not answer (or bad CRC twice) */
#define IOB_TF_SUPPLY    0x0002u   /* the isolated supply reports a fault (TEMP_EN_FLT low) */
#define IOB_TF_CJ        0x0004u   /* no cold-junction temperature */
#define IOB_TF_REF       0x0008u   /* the reference failed (REF low flags) */

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
     * The reply ends when the line goes idle. Frames are RTU frames (address, PDU, CRC) on
     * either link: the Ethernet port turns them into Modbus TCP and back (iob_net.h). */
    uint16_t (*xfer)(struct iob_port *p, const uint8_t *tx, uint16_t tx_len,
                     uint8_t *rx, uint16_t rx_cap, uint32_t timeout_ms);
    /* Ethernet only (NULL on RS-485): the board the next xfer goes to, by its IP (host order) */
    void (*target)(struct iob_port *p, uint32_t ip);
    void *ctx;
} iob_port_t;

/* Ethernet discovery (iob_net.h): UDP broadcasts on IOB_DISC_PORT. */
typedef struct iob_dgram {
    bool     (*send)(struct iob_dgram *d, const uint8_t *buf, uint16_t len);          /* broadcast */
    uint16_t (*recv)(struct iob_dgram *d, uint8_t *buf, uint16_t cap, uint32_t timeout_ms);  /* 0 = none */
    void *ctx;
} iob_dgram_t;

/* the link to the boards (parameter 102 iob_link): RS-485, Ethernet on the main (client) port
 * through lwIP, or Ethernet on the I/O port (the W5500, a separate network: iob_w5500.h). Both
 * Ethernet links carry the same Modbus TCP and discovery. */
enum { IOB_LINK_RS485 = 0, IOB_LINK_ETH = 1, IOB_LINK_IOPORT = 2 };
#define IOB_LINK_IS_ETH(l) ((l) == IOB_LINK_ETH || (l) == IOB_LINK_IOPORT)

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
#define IOBS_TEMP_FAULT  0x0800u  /* map 2: a configured temperature channel has no reading */

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
    /* map 2 */
    uint8_t  map_version;    /* of the board, once identified */
    uint8_t  n_temps, n_pwm; /* what the board has (0 on a map 1 board) */
    int16_t  temp[IOB_TEMPS];        /* as the board reports them, 0.1 degC (no calibration) */
    uint8_t  temp_st[IOB_TEMPS];     /* IOB_TS_*; IOB_TS_WAIT until read */
    int16_t  cj;
    uint16_t temp_flags, temp_count;
    uint16_t pwm_applied, rpm;
} iob_view_t;

void iob_init(iob_port_t *port);       /* plat_start (NULL: no RS-485 port, boards report not responding) */
void iob_init_eth(iob_port_t *tcp, iob_dgram_t *disc);   /* plat_start: the Ethernet link (NULL: none) */
void iob_init_ioport(iob_port_t *tcp, iob_dgram_t *disc);   /* plat_start: the I/O port link (NULL: none) */
int  iob_link_interlock(uint16_t id, plat_val_t v);           /* setting 102: refuses a link with no port */
void iob_cycle(uint32_t now_ms);       /* iobus task */
void iob_safe_state(void);             /* any task: send all-off from now on (until the next reset) */
bool iob_board_view(unsigned board, iob_view_t *v);   /* board 0..IOB_BOARDS-1 */
const char *iob_state_name(uint8_t state);
/* What is wrong with a board, in words, for the screens (up to max lines; returns how many). */
#define IOB_LINE 40
int iob_fault_lines(const iob_view_t *v, uint8_t ol_mask, uint8_t wb_mask, char lines[][IOB_LINE], int max);
const char *iob_temp_type_name(uint8_t type);     /* "K", "PT100 3-wire" ... ("?" for an unknown code) */
const char *iob_temp_state_name(uint8_t state);   /* "ok", "open circuit" ... */
/* calibration: raw 0.1 degC -> 0.01 degC with offset (0.01 degC) and gain - 1 (0.01 %) */
int32_t iob_temp_cal(int16_t raw_ds, int16_t offset_cd, int16_t gain_adj);

/* The scan's hardware I/O (scan_hw_bsp): the main board's pins plus the I/O boards. */
void plat_hw_io_read(plat_io_in_t *in);
void plat_hw_io_write(const plat_io_out_t *out);
void plat_io_safe_state(void);         /* bsp_io_safe_state() + iob_safe_state() */

/* Scan task, from plat_hw_io_read (tests call it directly): settings in, status out,
 * alarms. now_ms stamps the output image for the stale check. */
void iob_scan_in(plat_io_in_t *in, uint32_t now_ms);
void iob_scan_out(const plat_io_out_t *out, uint32_t now_ms);

/* Settings (parameters 100..148 and 160..223, param.c). first_do/first_di/first_temp/first_ao are
 * 1-based channel numbers (first_temp and first_ao 0: not used). */
typedef struct {
    bool     enable;
    uint8_t  address;        /* 1..IOB_ADDR_MAX (246 and 247 are the commissioning and USB addresses); on Ethernet the unit id */
    uint32_t ip;             /* Ethernet: the board's IP (host order) */
    uint8_t  first_do;       /* 1..PLAT_DO_MAX-17 */
    uint8_t  first_di;       /* 1..PLAT_DI_MAX-7 */
    uint8_t  ol_mask;        /* open-circuit check, bank A outputs 1-8 */
    uint8_t  wb_mask;        /* wire-break check, inputs 1-8 */
    /* map 2 */
    uint8_t  first_temp;     /* plat_io_in_t temperature channel of the board's channel 1 (0: not mapped) */
    uint8_t  first_ao;       /* plat_io_out_t ao_raw channel that drives the pump PWM (0: PWM held off) */
    uint8_t  temp_type[IOB_TEMP_CONNS];   /* IOB_TT_* per connector (unknown codes: IOB_TT_NONE) */
    uint16_t ntc_r25[IOB_TEMP_CONNS];     /* 10 ohm units, 0 = 10k */
    uint16_t ntc_beta[IOB_TEMP_CONNS];    /* K, 0 = 3435 */
    int16_t  temp_offset[IOB_TEMPS];      /* calibration: added, 0.01 degC */
    int16_t  temp_gain[IOB_TEMPS];        /* calibration: gain - 1, 0.01 % (0 = 1.0000) */
    uint16_t pwm_hz;
    uint8_t  tach_ppr;
    int16_t  temps[IOB_TEMPS];            /* RO: calibrated, 0.1 degC (IOB_TEMP_INVALID: no reading) */
    uint8_t  temp_state[IOB_TEMPS];       /* RO: IOB_TS_* */
    int16_t  cj_temp;                     /* RO */
    uint16_t pump_rpm, pump_duty;         /* RO */
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
extern uint8_t  iob_link;            /* parameter 102: IOB_LINK_* (default the I/O port) */
extern uint32_t iob_port_ip;         /* parameter 103: the I/O port's own IP (W5500, its own network) */
extern uint8_t  iob_port_prefix;     /* parameter 104: its subnet as a prefix length (24 = 255.255.255.0) */
#define IOB_PORT_MASK(prefix) ((prefix) >= 8u && (prefix) <= 30u ? 0xFFFFFFFFu << (32u - (prefix)) : 0xFFFFFF00u)
extern uint8_t  iob_port_status;     /* parameter 105, RO: IOBP_* (written by the bus task) */
#define IOBP_FOUND 0x01u             /* the W5500 answers and is set up */
#define IOBP_LINK  0x02u             /* its Ethernet link is up (cable in, switch on) */

/* Commissioning from the web page: find a board and give it its address (node requirements FW-61).
 * A new board has no address and answers on RS-485 only at IOB_COMMISSION_ADDR, so only one new
 * board may be on the bus at a time. Asked from any task (one request at a time, else false); the
 * bus task does it in its next cycle, between the boards' own requests.
 *   iob_cm_find(addr)      read the identification and serial number of whatever answers at addr
 *   iob_cm_assign(from,to) the guarded FC16 of 40-42: the board at from (1..246) takes to (1..245).
 *                          Refused (IOB_CM_IN_USE) when something already answers at to; the master
 *                          reads the board back at to before it reports IOB_CM_OK. */
enum { IOB_CM_IDLE = 0, IOB_CM_BUSY, IOB_CM_OK, IOB_CM_NO_REPLY, IOB_CM_WRONG_TYPE,
       IOB_CM_IN_USE, IOB_CM_REFUSED, IOB_CM_NOT_CONFIRMED };
typedef struct {
    uint8_t  result;          /* IOB_CM_* of the last request */
    uint8_t  op;              /* 1 find, 2 assign */
    uint8_t  from, to;        /* addresses of the last request (find: from) */
    uint8_t  exception;       /* IOB_CM_REFUSED: the node's exception code */
    bool     found;           /* the identification was read (find, and assign's read-back) */
    uint16_t hw_rev, fw_version, fw_build;
    uint16_t uid[6];
    char     serial[2 * IOB_SERIAL_REGS + 1];
    uint32_t seq;             /* counts finished requests */
} iob_cm_view_t;
bool iob_cm_find(uint8_t addr);
bool iob_cm_assign(uint8_t from, uint8_t to);
void iob_cm_view(iob_cm_view_t *v);
const char *iob_cm_result_name(uint8_t r);

/* Ethernet commissioning from the web page (iob_net.h): list the boards on the network, whatever
 * their IP, and give one its IP and Modbus address. Same threading as iob_cm_*.
 *   iob_dc_find()    broadcast FIND, collect the HERE answers for PLAT_IOB_DISC_WAIT_MS
 *   iob_dc_set(...)  SET for the board with that UID: ip, the supervisor's mask, gateway and own IP,
 *                    the Modbus address. Refused (IOB_CM_IN_USE) when another board in the last
 *                    list, or the supervisor, has that IP; OK only when the board answers with
 *                    the new settings. */
#define IOB_DC_MAX 8
typedef struct {
    uint8_t    result;        /* IOB_CM_* of the last request */
    uint8_t    op;            /* 1 find, 2 set */
    uint8_t    n;             /* boards in the last list */
    iob_disc_t boards[IOB_DC_MAX];
    uint32_t   seq;           /* counts finished requests */
} iob_dc_view_t;
bool iob_dc_find(void);
bool iob_dc_set(const uint16_t uid[6], uint32_t ip, uint32_t mask, uint32_t gw, uint32_t master_ip, uint8_t address);
void iob_dc_view(iob_dc_view_t *v);

/* Test hook: forget all state (boards back to OFF, safe state cleared). */
void iob_reset(void);

#endif
