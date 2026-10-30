/* param.h - the shared parameter table (design spec "Shared parameter table",
 * "Control arbitration").
 *
 * Every setting the touch UI, web interface or Modbus can see or change is one
 * entry here, bound to the application tag that holds it. All three go through
 * the same checks, in this order:
 *     known ID -> writable -> access level -> local/remote mode -> type -> range
 *     -> application interlock (at apply time, in the scan)
 * so a limit or interlock can never be bypassed by choosing a different route.
 *
 * Threading
 *   param_check()            any task: everything except the interlock; use it
 *                            to answer a Modbus/web request at once, then post.
 *   param_apply()            scan task only (called for each queued command).
 *   param_publish()          scan task only, inside the snapshot seqlock.
 *   param_read()             any task: the value as of the end of the last scan.
 *
 * Persistence of PF_PERSIST entries is done by param_store (param_store.h).
 *
 * ID ranges: 1..999 platform (PARAM_ID_* below), 1000..65535 application. */
#ifndef PARAM_H
#define PARAM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "plat_cmdq.h"
#include "plat_seqlock.h"

#ifndef PARAM_MAX
#define PARAM_MAX 512
#endif

/* C type of the bound tag (for arrays: of each element) */
typedef enum { CT_BOOL = 0, CT_U8, CT_I8, CT_U16, CT_I16, CT_U32, CT_I32, CT_F32, CT_BLOB } param_ctype_t;

/* External (wire) type.
 * Scalars: PT_BOOL, PT_INT, PT_UINT, PT_FLOAT.
 * Blocks ("blobs", .size bytes at .ptr):
 *   PT_BYTES  raw bytes (CT_BLOB), e.g. a UniLogic BUFFER
 *   PT_TEXT   8-bit text, NUL-terminated within .size (CT_BLOB); printable ASCII only
 *   PT_TEXT16 UTF-16LE text, NUL-terminated within .size (CT_BLOB); e.g. alarm descriptions
 *   PT_ARRAY  array of .ctype elements, each range-checked with vmin..vmax */
typedef enum { PT_BOOL = 0, PT_INT, PT_UINT, PT_FLOAT, PT_BYTES, PT_TEXT, PT_TEXT16, PT_ARRAY } param_ptype_t;
#define PARAM_IS_BLOB(d) ((d)->ptype >= PT_BYTES)

/* Flags */
#define PF_RO          0x0001u  /* never written from outside (status values) */
#define PF_PERSIST     0x0002u  /* kept in FRAM */
#define PF_FACTORY     0x0004u  /* factory class: calibration, serial, hw rev.
                                   Separate FRAM image, never reset by "restore defaults" */
#define PF_ANY_MODE    0x0008u  /* writable from any source in any mode (e.g. alarm ack) */
#define PF_LOCAL_ONLY  0x0010u  /* only from the touchscreen */
#define PF_NO_MODBUS   0x0020u  /* never from Modbus (network and security settings) */
#define PF_CONFIRM     0x0040u  /* the touchscreen asks "are you sure?" before changing it (critical setpoints) */
#define PF_SECRET      0x0080u  /* write-only: never read back (web, Modbus, touchscreen), never in a backup file
                                   (e.g. the mail server password); the platform reads it with param_read_secret */
#define PF_RECORD      0x0200u  /* a structure (sequence, schedule) written only whole with param_record_post, after the
                                   application's check_block has accepted the complete new image (D26) */
#define PF_DEV         0x0100u  /* developer authority: no ordinary write at any level, not restored from a backup;
                                   changed only through plat_serial_assign with a signed token (D25) */

typedef int (*param_interlock_fn)(uint16_t id, plat_val_t v);  /* 0 = allow */
/* Records: the whole new image, before it replaces the tag. 0 = accept; otherwise put
 * the reason in why (shown to the operator, e.g. "Period 4: inlet 7 is not fitted"). */
typedef int (*param_block_check_fn)(uint16_t id, const void *image, char *why, size_t cap);

typedef struct {
    uint16_t   id;
    uint8_t    ctype;       /* param_ctype_t */
    uint8_t    ptype;       /* param_ptype_t */
    uint16_t   flags;
    uint8_t    level;       /* plat_level_t needed to write */
    uint8_t    rsvd;
    void      *ptr;         /* the application tag */
    plat_val_t vmin, vmax, vdef;
    const char *name;       /* key for JSON / labels: [A-Za-z0-9_] */
    const char *unit;       /* may be NULL */
    param_interlock_fn interlock;   /* may be NULL (scalars only) */
    uint16_t   size;        /* blobs: bytes at ptr */
    const void *dflt;       /* blobs: default content (NULL = zeros) */
    param_block_check_fn check_block;   /* records: the application's check of a whole new image (may be NULL) */
} param_def_t;

/* Result codes (mapped to Modbus exceptions and HTTP errors by those modules) */
typedef enum {
    PERR_OK = 0,
    PERR_NO_ID,        /* unknown parameter          -> Modbus 02 illegal address */
    PERR_READONLY,     /*                             -> 02 */
    PERR_ACCESS,       /* login level too low         -> 01 / HTTP 403 */
    PERR_MODE,         /* wrong local/remote mode     -> 04 / HTTP 409 */
    PERR_TYPE,         /* value kind not accepted     -> 03 */
    PERR_RANGE,        /* outside min..max, or NaN    -> 03 */
    PERR_INTERLOCK,    /* refused by the application  -> 04 / HTTP 409 */
    PERR_TABLE,        /* table error found at init */
    PERR_BUSY          /* write queue full, or a reader collided with the scan -> 06 / HTTP 503 */
} param_err_t;

/* ---------------- platform parameters ---------------- */
enum {
    PARAM_ID_CTRL_MODE        = 1,   /* 0 = LOCAL, 1 = REMOTE. Runtime; starts LOCAL. Touchscreen only */
    PARAM_ID_REMOTE_LINK_LOST = 2,   /* RO status: remote mode and no remote traffic for the timeout */
    PARAM_ID_COMMS_TIMEOUT_S  = 3,   /* remote-link watchdog, s (0 = off) */
    PARAM_ID_MODBUS_WRITE_EN  = 4,   /* 0 = Modbus is read-only */
    PARAM_ID_MODBUS_ALLOW_IP  = 5,
    PARAM_ID_HTTP_REDIRECT    = 6,   /* port 80 answers with a redirect to https:// (else closed) */   /* 0 = any client; else only this IPv4 (host order) */
    PARAM_ID_NET_DHCP         = 10,
    PARAM_ID_NET_IP           = 11,  /* IPv4, host byte order */
    PARAM_ID_NET_MASK         = 12,
    PARAM_ID_NET_GW           = 13,
    PARAM_ID_NET_STATUS       = 14,  /* RO: NETST_* bits, written by the network task */
    PARAM_ID_NET_LINK_MODE    = 15,  /* what the port offers the switch: 0 auto (10/100, half/full), 1 100 Mbit only, 2 10 Mbit only (long or poor cable) */
    PARAM_ID_RESTORE_DEFAULTS = 20,  /* write 1: user-class parameters back to defaults */
    PARAM_ID_SERIAL           = 50,  /* developer authority only (PF_DEV, plat_serial.h) */
    PARAM_ID_HW_REV           = 51,  /* factory */
    PARAM_ID_ALARM_ACK        = 40,  /* write an alarm ID (or 65535 = all) to acknowledge it; any mode, no login */
    PARAM_ID_ALARM_ACTIVE     = 41,  /* RO: alarms active */
    PARAM_ID_ALARM_UNACKED    = 42,  /* RO: alarms waiting for an acknowledge */
    PARAM_ID_ALARM_ACTIVE_BITS = 43, /* RO: 16 words, bit (ID % 16) of word (ID / 16) = alarm ID active */
    PARAM_ID_ALARM_UNACK_BITS = 44,  /* RO: same layout, alarm ID unacknowledged */
    PARAM_ID_ALARM_COMMON     = 45,  /* RO: ACOM_* bits (Modbus common alarm, relay states) */
    PARAM_ID_ALARM_ROUTE      = 46,  /* 256 bytes, one per alarm ID: AR_* actions (plat_alarm.h) */
    PARAM_ID_ALARM_RELAY_DO   = 47,  /* 4 bytes: digital output 1..64 for alarm relay 1..4 (0 = not used) */
    PARAM_ID_ALARM_RELAY_MODE = 48,  /* 4 bytes: AR_MODE_* */
    PARAM_ID_ALARM_RELAY_INV  = 49,  /* 4 bools: fail-safe (energised when there is no alarm) */
    PARAM_ID_MAIL_ENABLE      = 70,  /* alarm email on/off (off: the instrument makes no outbound connection) */
    PARAM_ID_MAIL_SERVER      = 71,  /* mail server (SMTP) IPv4, host order: the site's mail relay */
    PARAM_ID_MAIL_PORT        = 72,  /* 25, 587 (STARTTLS) or 465 (TLS) */
    PARAM_ID_MAIL_SECURITY    = 73,  /* MAIL_SEC_*: none, STARTTLS, TLS */
    PARAM_ID_MAIL_PIN         = 74,  /* 32 bytes: SHA-256 of the server's certificate to insist on (zeros = encrypt only) */
    PARAM_ID_MAIL_USER        = 75,  /* login (empty = no login) */
    PARAM_ID_MAIL_PASS        = 76,  /* PF_SECRET */
    PARAM_ID_MAIL_FROM        = 77,
    PARAM_ID_MAIL_TO          = 78,  /* up to 4 addresses, separated by commas */
    PARAM_ID_MAIL_ON_CLEAR    = 79,  /* also email when an alarm clears */
    PARAM_ID_MAIL_GAP_S       = 80,  /* collect alarms this long before sending, so a burst is one email */
    PARAM_ID_SITE_NAME        = 81,  /* the unit's name in emails and on the web page */
    PARAM_ID_TIME_UTC_OFFSET  = 82,  /* minutes the clock (local standard time) is ahead of UTC */
    PARAM_ID_SNTP_SERVER      = 83,  /* site time server IPv4 (0 = no SNTP) */
    PARAM_ID_SNTP_INTERVAL    = 84,  /* minutes between SNTP queries */
    PARAM_ID_CLOCK_SET        = 85,  /* write local seconds since 1970 to set the clock (logged; 0 = nothing) */
    PARAM_ID_CLOCK_UNIX       = 86,  /* RO: the clock, local seconds since 1970 (0 = never set) */
    PARAM_ID_FW_LOCAL_CONFIRM = 87,  /* a firmware restart asked from the web must be confirmed on the touchscreen */
    PARAM_ID_FW_MIN_VERSION   = 88,  /* factory: images older than this (major<<16|minor<<8|patch) are refused */
    PARAM_ID_LOG_INTERVAL_S   = 30,  /* data-log averaging interval, s (0 = data log off) */
    PARAM_ID_STORE_STATUS     = 60,  /* RO: param_store status bits (param_store.h) */
    PARAM_ID_LOG_STATUS       = 61,  /* RO: plat_log status bits (plat_log.h) */
    PARAM_ID_IOB_HOLD         = 100, /* I/O boards (iobus.h): hold time, s; a node with no valid write this long turns its outputs off */
    PARAM_ID_IOB_FILTER       = 101, /* input filter, ms */
    PARAM_ID_IOB_LINK         = 102, /* the link to the boards: 0 RS-485, 1 Ethernet on the main port, 2 the I/O port (W5500, iob_w5500.h) */
    PARAM_ID_IOB_PORT_IP      = 103, /* the I/O port's own IP (its own network, not the main port's) */
    PARAM_ID_IOB_PORT_PREFIX  = 104, /* the I/O port's subnet, prefix length 8..30 */
    PARAM_ID_IOB_PORT_STATUS  = 105, /* RO: IOBP_* bits (chip found, link up) */
    PARAM_ID_IOB_FIRST        = 110, /* board k (0..2) at 110 + 10k: */
    /*   +0 enable, +1 Modbus address, +2 first output channel (1-based), +3 first input channel,
         +4 open-circuit check (bank A outputs 1-8, bits), +5 wire-break check (inputs 1-8, bits),
         +6 RO status (IOBS_*), +7 RO output faults (bits 1-18), +8 RO input faults, +9 RO failed requests */
    PARAM_ID_IOB_LIVE_FIRST   = 140, /* board k at 140 + 2k: +0 RO inputs (bits), +1 RO outputs as driven (bits) */
    PARAM_ID_IOB_IP_FIRST     = 146, /* board k at 146 + k: its IP on Ethernet */
    PARAM_ID_IOB_T_FIRST      = 160, /* board k at 160 + 16k: temperature inputs and pump PWM (map 2, iobus.h): */
    /*   +0 first temperature channel (0 = not mapped), +1 analog output for the pump PWM (0 = none),
         +2 sensor type per connector (3 bytes, IOB_TT_*), +3 NTC R25 (3 x 10 ohm, 0 = 10k), +4 NTC B (3 x K, 0 = 3435),
         +5 calibration offset (6 x 0.01 degC), +6 calibration span (gain - 1, 6 x 0.01 %), +7 PWM frequency (Hz),
         +8 tacho pulses per revolution, +9 RO temperatures (6 x 0.1 degC), +10 RO channel states (6 x IOB_TS_*),
         +11 RO cold junction (0.1 degC), +12 RO pump speed (rpm), +13 RO pump duty as driven (0.1 %) */
    PARAM_ID_STATE            = 300, /* RO: instrument state, PLAT_ST_* (plat_state.h) */
    PARAM_ID_MAINTENANCE      = 301, /* operator selects MAINTENANCE (1) or leaves it (0) */
    PARAM_ID_STATE_SINCE      = 302, /* RO: clock when the state was entered (local s since 1970; 0 = not set) */
    PARAM_ID_MEAS_Q           = 303, /* RO: one PLAT_Q_* per declared measurement (a Modbus status block) */
    PARAM_ID_SUPPLY_MV        = 305, /* RO: the 24 V supply, mV (0 = not measured; plat_power.h) */
    PARAM_ID_HEARTBEAT        = 306, /* RO: +1 every 100 ms (the platform's Modbus block, modbus.h) */
    PARAM_ID_LINK_AGE_S       = 307, /* RO: s since the last Modbus or web request (65535 = none or longer) */
    PARAM_ID_MB_BLOCK_VERSION = 308, /* RO: layout version of the platform's Modbus block */
    PARAM_ID_MB_MAP_VERSION   = 309, /* RO: the application's register map version */
    PARAM_ID_FW_VERSION_NUM   = 310, /* RO: firmware version, major<<16 | minor<<8 | patch */
    PARAM_ID_API_VERSION      = 311, /* RO: web API version (/api/info "api") */
    PARAM_ID_UPTIME_S         = 312, /* RO: s since start */
    PARAM_ID_PRODUCT          = 313, /* RO text: product (Modbus identity) */
    PARAM_ID_VENDOR           = 314, /* RO text: vendor */
    PARAM_ID_FW_TEXT          = 315, /* RO text: firmware version as text */
    PARAM_ID_CLOCK_SYNCED     = 316, /* RO: the clock is synchronised (plat_clock.h, CIP-02.6) */
    PARAM_ID_CLOCK_SYNC_TIME  = 317, /* RO: when it was last set or confirmed (local s since 1970; 0 = not since start) */
    PARAM_ID_CLOCK_SOURCE     = 318, /* RO: PLAT_CLK_SRC_* of that */
    PARAM_ID_BOARD_REV        = 319, /* RO: board revision from its straps (65535 = no straps; fw_update.h, CIP-01.3) */
    PARAM_ID_CAL_CMD          = 320, /* calibration command (plat_cal.h): command << 8 | channel; momentary */
    PARAM_ID_CAL_LEVEL        = 321, /* factory: login level that may calibrate (CIP-19.3; default admin) */
    PARAM_ID_RUN_HOURS        = 330, /* RO: powered hours x 10 (plat_service.h, CIP-22.2) */
    PARAM_ID_SERVICE_HOURS    = 331, /* RO: hours since the last service log entry x 10 */
    PARAM_ID_CONFIG_CRC       = 332, /* RO: checksum of the user settings (CIP-05.4) */
    PARAM_ID_SELFTEST         = 333, /* RO: start-up self-test failures, bits (plat_selftest.h, CIP-11.1) */
    PARAM_ID_MB_FORCED        = 334, /* RO: values forced on Modbus now (modbus.h mb_force_*) */
    PARAM_ID_PLAT_BLOCK_FIRST = 900, /* 900..999: platform retained blocks, not in the table */
    PARAM_ID_APP_FIRST        = 1000
};

enum { PARAM_MODE_LOCAL = 0, PARAM_MODE_REMOTE = 1 };
enum { MAIL_SEC_NONE = 0, MAIL_SEC_STARTTLS = 1, MAIL_SEC_TLS = 2 };

/* PARAM_ID_NET_STATUS bits */
#define NETST_LINK      0x0001u  /* Ethernet cable connected */
#define NETST_CONFLICT  0x0002u  /* ALARM: another device answers at our static address (not taken; re-checked every 30 s) */
#define NETST_PROBING   0x0004u  /* checking the address before using it (about 2-7 s) */
#define NETST_READY     0x0008u  /* address in use: static and checked, or leased by DHCP */
extern uint32_t plat_net_status_word;

/* ---------------- definition helpers for application tables ----------------
 *   PARAM_INT (1001, "num_valves", NumberOfValves, CT_I16, 1, 20, 10, PLAT_LVL_ADMIN, PF_PERSIST, NULL)
 *   PARAM_REAL(1002, "flow_low",   FlowLow, 0.0f, 100.0f, 5.0f, "l/min", PLAT_LVL_OPERATOR, PF_PERSIST)
 *   PARAM_BIT (1003, "start",      StartButton, PLAT_LVL_OPERATOR, 0)                                     */
#define PARAM_INT(ID, NAME, VAR, CT, MIN, MAX, DEF, LVL, FLAGS, UNIT) \
    { .id = (ID), .ctype = (CT), .ptype = PT_INT, .flags = (FLAGS), .level = (LVL), .ptr = &(VAR), \
      .vmin.i = (MIN), .vmax.i = (MAX), .vdef.i = (DEF), .name = (NAME), .unit = (UNIT) }
#define PARAM_UINT(ID, NAME, VAR, CT, MIN, MAX, DEF, LVL, FLAGS, UNIT) \
    { .id = (ID), .ctype = (CT), .ptype = PT_UINT, .flags = (FLAGS), .level = (LVL), .ptr = &(VAR), \
      .vmin.u = (MIN), .vmax.u = (MAX), .vdef.u = (DEF), .name = (NAME), .unit = (UNIT) }
#define PARAM_REAL(ID, NAME, VAR, MIN, MAX, DEF, UNIT, LVL, FLAGS) \
    { .id = (ID), .ctype = CT_F32, .ptype = PT_FLOAT, .flags = (FLAGS), .level = (LVL), .ptr = &(VAR), \
      .vmin.f = (MIN), .vmax.f = (MAX), .vdef.f = (DEF), .name = (NAME), .unit = (UNIT) }
#define PARAM_BIT_D(ID, NAME, VAR, DEF, LVL, FLAGS) \
    { .id = (ID), .ctype = CT_BOOL, .ptype = PT_BOOL, .flags = (FLAGS), .level = (LVL), .ptr = &(VAR), \
      .vmin.b = false, .vmax.b = true, .vdef.b = (DEF), .name = (NAME), .unit = NULL }
#define PARAM_BIT(ID, NAME, VAR, LVL, FLAGS) PARAM_BIT_D(ID, NAME, VAR, false, LVL, FLAGS)

/* Blocks: the variable must be an array; its size is taken from it. */
#define PARAM_TEXT(ID, NAME, VAR, DFLT, LVL, FLAGS) \
    { .id = (ID), .ctype = CT_BLOB, .ptype = PT_TEXT, .flags = (FLAGS), .level = (LVL), .ptr = (VAR), \
      .size = (uint16_t)sizeof(VAR), .dflt = (DFLT), .name = (NAME) }
#define PARAM_TEXT16(ID, NAME, VAR, DFLT, LVL, FLAGS) \
    { .id = (ID), .ctype = CT_BLOB, .ptype = PT_TEXT16, .flags = (FLAGS), .level = (LVL), .ptr = (VAR), \
      .size = (uint16_t)sizeof(VAR), .dflt = (DFLT), .name = (NAME) }
#define PARAM_BYTES(ID, NAME, VAR, LVL, FLAGS) \
    { .id = (ID), .ctype = CT_BLOB, .ptype = PT_BYTES, .flags = (FLAGS), .level = (LVL), .ptr = (VAR), \
      .size = (uint16_t)sizeof(VAR), .name = (NAME) }
/* A record: a whole C structure (a sequence, a schedule), up to PARAM_RECORD_MAX bytes.
 * Written only whole (param_record_post, the editors, POST /api/record), applied in one
 * scan after CHECK (may be NULL) has accepted the new image. Not on Modbus. */
#define PARAM_RECORD(ID, NAME, VAR, CHECK, LVL, FLAGS) \
    { .id = (ID), .ctype = CT_BLOB, .ptype = PT_BYTES, .flags = (uint16_t)((FLAGS) | PF_RECORD | PF_NO_MODBUS), \
      .level = (LVL), .ptr = &(VAR), .size = (uint16_t)sizeof(VAR), .name = (NAME), .check_block = (CHECK) }
#define PARAM_ARRAY_INT(ID, NAME, VAR, CT, MIN, MAX, LVL, FLAGS) \
    { .id = (ID), .ctype = (CT), .ptype = PT_ARRAY, .flags = (FLAGS), .level = (LVL), .ptr = (VAR), \
      .vmin.i = (MIN), .vmax.i = (MAX), .size = (uint16_t)sizeof(VAR), .name = (NAME) }
#define PARAM_ARRAY_UINT(ID, NAME, VAR, CT, MIN, MAX, LVL, FLAGS) \
    { .id = (ID), .ctype = (CT), .ptype = PT_ARRAY, .flags = (FLAGS), .level = (LVL), .ptr = (VAR), \
      .vmin.u = (MIN), .vmax.u = (MAX), .size = (uint16_t)sizeof(VAR), .name = (NAME) }
#define PARAM_ARRAY_REAL(ID, NAME, VAR, MIN, MAX, LVL, FLAGS) \
    { .id = (ID), .ctype = CT_F32, .ptype = PT_ARRAY, .flags = (FLAGS), .level = (LVL), .ptr = (VAR), \
      .vmin.f = (MIN), .vmax.f = (MAX), .size = (uint16_t)sizeof(VAR), .name = (NAME) }
#define PARAM_ARRAY_BOOL(ID, NAME, VAR, LVL, FLAGS) \
    { .id = (ID), .ctype = CT_BOOL, .ptype = PT_ARRAY, .flags = (FLAGS), .level = (LVL), .ptr = (VAR), \
      .vmin.b = false, .vmax.b = true, .size = (uint16_t)sizeof(VAR), .name = (NAME) }

/* ---------------- API ---------------- */

/* Build the table from the platform entries plus the application's. Checks for
 * duplicate IDs, bad types, min > max and defaults out of range; returns PERR_OK
 * or PERR_TABLE (and param_table_error() says which ID). Does not touch tags. */
param_err_t param_init(const param_def_t *app_defs, size_t n_app);
uint16_t    param_table_error(void);

/* Every entry's tag := its default. persisted values are then loaded over it by
 * param_store. which: PARAM_ALL, PARAM_USER_ONLY (used by restore defaults). */
enum { PARAM_ALL = 0, PARAM_USER_ONLY = 1 };
void param_set_defaults(int which);

const param_def_t *param_find(uint16_t id);
size_t             param_count(void);
const param_def_t *param_at(size_t index);    /* iteration, sorted by ID */

/* Full check except the interlock, from any task. */
param_err_t param_check(uint16_t id, uint8_t kind, plat_val_t v, uint8_t src, uint8_t level);

/* Scan task: check again (mode may have changed since), run the interlock, write the tag. */
param_err_t param_apply(const plat_cmd_t *cmd);

/* Scan task, end of scan, inside the snapshot seqlock. */
void param_publish(void);

/* Any task: value at the end of the last scan. false if the ID is unknown. */
bool param_read(uint16_t id, plat_val_t *out);

/* Conversion helpers (also used by param_store). */
plat_val_t param_tag_get(const param_def_t *d);
void       param_tag_set(const param_def_t *d, plat_val_t v);
size_t     param_ctype_size(uint8_t ctype);

/* ---------------- servers: post writes, read blocks ----------------
 * Bind the queue the scan drains and the lock it publishes under (plat_start does
 * this; tests may pass NULL lock for direct reads). */
void param_bind(plat_cmdq_t *cmdq, plat_seqlock_t *snap_lock);

/* Check and queue a scalar write, from any task. PERR_BUSY if the queue is full. */
param_err_t param_post(uint16_t id, uint8_t kind, plat_val_t v, uint8_t src, uint8_t level);

/* Blocks. A write replaces len bytes at byte offset off. whole = true replaces the
 * entire content from offset 0 and zero-fills the rest (how text is written);
 * text entries only accept whole writes. Arrays: off and len must be whole
 * elements, and each element is range-checked. */
#define PARAM_BLOB_MAX_WRITE 128u
param_err_t param_blob_check(uint16_t id, uint16_t off, const void *data, uint16_t len, bool whole,
                             uint8_t src, uint8_t level);
param_err_t param_blob_post(uint16_t id, uint16_t off, const void *data, uint16_t len, bool whole,
                            uint8_t src, uint8_t level);
void        param_blob_drain(void);       /* scan task: apply queued block writes */

/* Batches (Modbus FC15/16, a web form): every item is checked first, then all are
 * queued at once or none is (PERR_BUSY if they don't all fit). The scan applies a
 * batch in one scan. Scalars and blocks are separate batches. */
typedef struct { uint16_t id, off, len; bool whole; const void *data; } param_blob_write_t;
param_err_t param_post_many(const plat_cmd_t *cmds, size_t n);
param_err_t param_blob_post_many(const param_blob_write_t *w, size_t n, uint8_t src, uint8_t level);

/* Records (PF_RECORD). Any task: queue a whole new image (len = the record's size).
 * One record write is in flight at a time (PERR_BUSY otherwise). The scan applies it at
 * its next command point: access checks again, then the record's check_block, then the
 * copy, all in one scan. *ticket identifies the write; param_record_result() reports
 * the outcome once applied (false = not yet): PERR_OK, or PERR_INTERLOCK with the
 * check's reason in why. */
#define PARAM_RECORD_MAX 1024u
param_err_t param_record_post(uint16_t id, const void *data, uint16_t len, uint8_t src, uint8_t level, uint32_t *ticket);
bool        param_record_result(uint32_t ticket, param_err_t *e, char *why, size_t cap);
param_err_t param_record_check(uint16_t id, uint8_t src, uint8_t level);   /* may this source/level write it now? */
void        param_record_drain(void);     /* scan task (called by param_blob_drain) */

/* Any task: len bytes from offset off, as of the end of the last scan.
 * PF_SECRET entries: PERR_ACCESS (param_read_secret reads them, for the platform only). */
param_err_t param_read_blob(uint16_t id, uint16_t off, void *buf, uint16_t len);
param_err_t param_read_secret(uint16_t id, void *buf, uint16_t len);

/* Validation helpers (also used by param_store on load). */
bool param_blob_valid(const param_def_t *d, const uint8_t *data, uint16_t off, uint16_t len);
bool param_blob_image_ok(const param_def_t *d, const uint8_t *data);

/* ---------------- arbitration and the remote-link watchdog ---------------- */
uint8_t param_ctrl_mode(void);                 /* PARAM_MODE_LOCAL / REMOTE, any task */
void    param_comms_activity(uint8_t src, uint32_t now_ms); /* Modbus/HTTP: a valid request arrived (any task) */
bool    param_remote_link_lost(void);          /* any task; the application acts on it */
void    param_scan_service(uint32_t now_ms);   /* scan task, every scan */

/* Called by param_apply when PARAM_ID_RESTORE_DEFAULTS is written (set by param_store). */
extern void (*param_on_restore_defaults)(void);
/* Called by param_apply when PARAM_ID_ALARM_ACK is written (set by alarm_init). */
extern void (*param_on_alarm_ack)(uint16_t id, uint8_t src, uint8_t level);
extern param_err_t (*param_on_cal_cmd)(uint32_t cmd, uint8_t src, uint8_t level);   /* plat_cal.c */
/* Called in the scan task after an external write changed a value (the event log
 * sets it). Scalars: old and new value. Blocks and the restore command: values 0. */
extern void (*param_on_change)(const param_def_t *d, plat_val_t old_v, plat_val_t new_v, uint8_t src, uint8_t level);

#endif
