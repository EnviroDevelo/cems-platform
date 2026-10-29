/* inst.h - third-party instruments (gas analysers, flow meters, opacity monitors ...): the
 * core is the Modbus TCP master (client) and polls them (docs/MODBUS_ROLES.md, role 2).
 *
 * The instruments sit on the I/O network (the W5500 port, with the I/O boards). An instrument
 * that only speaks Modbus RTU is reached through a Modbus TCP to RTU gateway: the gateway's IP
 * and port, and the instrument's own address as the unit id. Several instruments behind one
 * gateway share one connection.
 *
 * Configuration (parameters 500-649, all in the parameter table, so the web page, the
 * touchscreen and a backup file handle them like any other setting):
 *   instrument k (INST_MAX)   enable, IP, port, unit id, poll period, reply timeout, the most
 *                             registers one request may read, the gap two points may have and
 *                             still be read in one request, a name; optionally a status point
 *                             and three masks that turn its bits into the quality of every
 *                             point of that instrument (fault, maintenance, calibration)
 *   point p (INST_POINTS)     the instrument (0 = not used), function (1 coils, 2 discrete
 *                             inputs, 3 holding, 4 input registers), address (0-based, as on
 *                             the wire), data type, word/byte order, bit, scaling
 *                             value = raw x gain + offset, valid range lo..hi, a name and a unit
 *
 * Polling. Every poll period each enabled instrument's points are read with as few requests as
 * the settings allow (points of the same function, sorted by address, merged while the request
 * stays within max_regs and the hole between two points is no more than gap registers). One
 * request is outstanding per connection at a time (gateways and many analysers take no more).
 * Nothing here waits: inst_cycle() starts a connection or a request and comes back for the
 * answer in a later cycle, so a missing analyser never delays the I/O boards.
 *
 * Quality of each point (PLAT_Q_*, plat_state.h):
 *   not used, its instrument off   UNAVAILABLE (value NaN)
 *   never read                     UNAVAILABLE
 *   the last read failed           STALE (the last value kept), then UNAVAILABLE once the
 *                                  instrument is lost (INST_LOST_FAILS polls in a row failed)
 *   exception 01-04 for its read   FAULT (a setting is wrong: function, address or count)
 *   its instrument's status point  FAULT, CAL or MAINT when a masked bit is set (in that order)
 *   NaN, infinite or outside lo..hi RANGE
 *   otherwise                      VALID
 * Exceptions 0A and 0B (the gateway has no path, or the instrument behind it did not answer)
 * count as a failed read, not a fault.
 *
 * Register explorer (commissioning): read any registers of any device on the I/O network once
 * and show the raw words, so the right address, type and word order can be found by trying
 * (the web page decodes the words every way). Reads only: functions 1-4.
 *
 * Threading
 *   inst_cycle()                       the iobus task, every cycle (after iob_cycle)
 *   inst_scan()                        the scan task (from plat_hw_io_read): settings in, values out
 *   inst_ex_start(), inst_ex_view()    any task
 *   inst_point()                       any task (the published values)
 */
#ifndef INST_H
#define INST_H
#include <stdbool.h>
#include <stdint.h>

#define INST_MAX         8u     /* instruments (or units behind gateways) */
#define INST_POINTS      32u    /* values read from them, in all */
#define INST_CONN_MAX    3u     /* TCP connections at once (W5500 sockets 4-6; socket 7 is the explorer's) */
#define INST_LOST_FAILS  3u     /* polls in a row that failed: the instrument is lost */
#define INST_RETRY_MS    5000u  /* a lost instrument is tried this often (or its period, if longer) */
#define INST_NAME_LEN    24u
#define INST_UNIT_LEN    12u
#define INST_EX_MAX      125u   /* registers the explorer reads at once (coils: up to 2000 bits) */

/* data types */
enum {
    INST_T_U16 = 0, INST_T_I16,
    INST_T_U32, INST_T_I32, INST_T_F32,    /* two registers */
    INST_T_BIT,                            /* one bit (point bit 0-15) of a register: 0 or 1 */
    INST_T_COUNT
};
/* byte order of the value as it comes off the wire. A B C D are the bytes in the order they
 * arrive (register n high, n low, register n+1 high, n+1 low); the name says which order they
 * make the value in, most significant first. 16-bit values use the first two letters. */
enum {
    INST_O_ABCD = 0,   /* big-endian, high word first: the Modbus convention */
    INST_O_CDAB,       /* low word first ("word swapped"; many analysers and PLCs) */
    INST_O_BADC,       /* bytes swapped inside each register */
    INST_O_DCBA,       /* little-endian throughout */
    INST_O_COUNT
};
/* function codes a point (or the explorer) reads with */
enum { INST_FC_COILS = 1, INST_FC_DISCRETE = 2, INST_FC_HOLDING = 3, INST_FC_INPUT = 4 };

/* instrument states (parameter inst_state) */
enum {
    INST_S_OFF = 0,      /* not enabled, or no point uses it */
    INST_S_CONNECTING,   /* first connection or poll not finished yet */
    INST_S_OK,           /* the last poll worked */
    INST_S_FAILING,      /* the last poll failed (fewer than INST_LOST_FAILS in a row) */
    INST_S_LOST,         /* INST_LOST_FAILS polls in a row failed: retried every INST_RETRY_MS */
    INST_S_BAD_SETTINGS  /* no IP, or a point's read cannot be made (too long for max_regs) */
};
/* why the last request failed (parameter inst_last_err) */
enum {
    INST_E_NONE = 0,
    INST_E_CONNECT,      /* no connection: nothing at that IP and port, or refused */
    INST_E_TIMEOUT,      /* no reply within the timeout */
    INST_E_EXCEPTION,    /* a Modbus exception (inst_last_exc) */
    INST_E_REPLY,        /* a reply that does not match the request */
    INST_E_CLOSED,       /* the instrument closed the connection */
    INST_E_NO_PORT       /* the I/O port is not working (no chip) */
};

/* ---- the network: non-blocking TCP client connections 0..INST_CONN_MAX (the last is the
 * explorer's). inst_w5500.h gives it on the I/O port; tests give their own. ---- */
enum { INST_L_CLOSED = 0, INST_L_CONNECTING, INST_L_UP };
typedef struct inst_net {
    bool     (*ready)(struct inst_net *n);                                  /* the port works */
    bool     (*connect)(struct inst_net *n, unsigned c, uint32_t ip, uint16_t port);   /* start */
    uint8_t  (*state)(struct inst_net *n, unsigned c);                      /* INST_L_* */
    bool     (*send)(struct inst_net *n, unsigned c, const uint8_t *buf, uint16_t len);
    uint16_t (*recv)(struct inst_net *n, unsigned c, uint8_t *buf, uint16_t cap);   /* what has arrived */
    void     (*close)(struct inst_net *n, unsigned c);
    void *ctx;
} inst_net_t;

/* ---- settings and results: the parameter table's tags (param.c) ---- */
typedef struct {
    bool     enable[INST_MAX];
    uint32_t ip[INST_MAX];              /* host order (0: not set) */
    uint16_t port[INST_MAX];            /* TCP port (502) */
    uint8_t  unit[INST_MAX];            /* unit id: the instrument's address behind a gateway (1) */
    uint16_t period_ms[INST_MAX];       /* poll period (1000) */
    uint16_t timeout_ms[INST_MAX];      /* reply and connect timeout (1000) */
    uint8_t  max_regs[INST_MAX];        /* registers (or 16 x coils) one request may read (64) */
    uint8_t  gap[INST_MAX];             /* registers between two points still read in one request (0) */
    uint8_t  status_pt[INST_MAX];       /* point 1..INST_POINTS holding the instrument's status (0: none) */
    uint16_t fault_mask[INST_MAX], maint_mask[INST_MAX], cal_mask[INST_MAX];   /* bits of the status point's raw value */
    char     name[INST_MAX][INST_NAME_LEN];
    /* RO */
    uint8_t  state[INST_MAX];           /* INST_S_* */
    uint32_t good[INST_MAX];            /* polls that worked */
    uint32_t errors[INST_MAX];          /* requests that failed */
    uint8_t  last_err[INST_MAX];        /* INST_E_* */
    uint8_t  last_exc[INST_MAX];        /* the last exception code */
    uint16_t resp_ms[INST_MAX];         /* how long the last poll took */
} inst_tags_t;

typedef struct {
    uint8_t  inst[INST_POINTS];         /* 1..INST_MAX (0: not used) */
    uint8_t  fc[INST_POINTS];           /* INST_FC_* (3) */
    uint16_t addr[INST_POINTS];         /* 0-based register (or coil) address */
    uint8_t  type[INST_POINTS];         /* INST_T_* */
    uint8_t  order[INST_POINTS];        /* INST_O_* */
    uint8_t  bit[INST_POINTS];          /* INST_T_BIT: 0..15 */
    float    gain[INST_POINTS], offset[INST_POINTS];   /* value = raw x gain + offset (1, 0) */
    float    lo[INST_POINTS], hi[INST_POINTS];         /* valid range (lo >= hi: none) */
    char     name[INST_POINTS][INST_NAME_LEN];
    char     unit[INST_POINTS][INST_UNIT_LEN];
    /* RO */
    float    value[INST_POINTS];        /* NaN until read */
    uint8_t  q[INST_POINTS];            /* PLAT_Q_* */
    uint32_t raw[INST_POINTS];          /* the raw bits after the byte order (16-bit: low half) */
} inst_pt_tags_t;

extern inst_tags_t    inst_tags;
extern inst_pt_tags_t inst_pt;

/* parameter IDs (param.h keeps the platform's list; these are the instruments' block) */
enum {
    PARAM_ID_INST_ENABLE = 500, PARAM_ID_INST_IP, PARAM_ID_INST_PORT, PARAM_ID_INST_UNIT,
    PARAM_ID_INST_PERIOD, PARAM_ID_INST_TIMEOUT, PARAM_ID_INST_MAX_REGS, PARAM_ID_INST_GAP,
    PARAM_ID_INST_STATUS_PT, PARAM_ID_INST_FAULT_MASK, PARAM_ID_INST_MAINT_MASK, PARAM_ID_INST_CAL_MASK,
    PARAM_ID_INST_STATE = 515, PARAM_ID_INST_GOOD, PARAM_ID_INST_ERRORS, PARAM_ID_INST_LAST_ERR,
    PARAM_ID_INST_LAST_EXC, PARAM_ID_INST_RESP_MS,
    PARAM_ID_INST_NAME_FIRST = 521,       /* 521..528: instrument k's name */
    PARAM_ID_PT_INST = 530, PARAM_ID_PT_FC, PARAM_ID_PT_ADDR, PARAM_ID_PT_TYPE, PARAM_ID_PT_ORDER,
    PARAM_ID_PT_BIT, PARAM_ID_PT_GAIN, PARAM_ID_PT_OFFSET, PARAM_ID_PT_LO, PARAM_ID_PT_HI,
    PARAM_ID_PT_VALUE = 540, PARAM_ID_PT_Q, PARAM_ID_PT_RAW,
    PARAM_ID_PT_NAME_FIRST = 550,         /* 550..581: point p's name */
    PARAM_ID_PT_UNIT_FIRST = 590          /* 590..621: point p's unit */
};

void inst_init(inst_net_t *net);        /* plat_start (NULL: no network, every instrument reports no port) */
void inst_cycle(uint32_t now_ms);       /* iobus task */
void inst_scan(uint32_t now_ms);        /* scan task */

/* A point's value and quality as of the last scan (false: p out of range). Any task. */
bool inst_point(unsigned p, float *value, uint8_t *q);

/* Decode a point's registers (or a coil) as the poller does: w = the words from the point's
 * address on (two for a 32-bit type). raw gets the bits after the byte order. Exposed for the tests. */
float inst_decode(uint8_t type, uint8_t order, uint8_t bit, const uint16_t *w, uint32_t *raw);
uint16_t inst_type_regs(uint8_t type);  /* registers a type takes: 1 or 2 */
const char *inst_state_name(uint8_t s);
const char *inst_err_name(uint8_t e);
const char *inst_type_name(uint8_t t);  /* "f32" */
const char *inst_order_name(uint8_t o); /* "CDAB" */

/* ---- register explorer ---- */
enum { INST_EX_IDLE = 0, INST_EX_BUSY, INST_EX_OK, INST_EX_NO_PORT, INST_EX_CONNECT,
       INST_EX_TIMEOUT, INST_EX_EXCEPTION, INST_EX_REPLY };
typedef struct {
    uint8_t  result;          /* INST_EX_* */
    uint8_t  exception;
    uint32_t ip;
    uint16_t port, addr, count;
    uint8_t  unit, fc;
    uint16_t ms;              /* how long it took */
    uint16_t nbytes;          /* data bytes in the reply */
    uint8_t  data[2 * INST_EX_MAX + 2];   /* as received: registers high byte first; coils packed LSB first */
    uint32_t seq;             /* counts finished reads */
} inst_ex_view_t;
/* Start one read; false when one is still running or an argument is out of range
 * (fc 1-4, count 1..INST_EX_MAX registers or 1..2000 bits, addr + count within 65536). */
bool inst_ex_start(uint32_t ip, uint16_t port, uint8_t unit, uint8_t fc, uint16_t addr, uint16_t count);
void inst_ex_view(inst_ex_view_t *v);
const char *inst_ex_result_name(uint8_t r);

void inst_reset(void);                  /* tests: forget everything (settings stay) */

#endif
