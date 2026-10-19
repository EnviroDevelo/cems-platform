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

typedef int (*param_interlock_fn)(uint16_t id, plat_val_t v);  /* 0 = allow */

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
    PARAM_ID_RESTORE_DEFAULTS = 20,  /* write 1: user-class parameters back to defaults */
    PARAM_ID_SERIAL           = 50,  /* factory */
    PARAM_ID_HW_REV           = 51,  /* factory */
    PARAM_ID_LOG_INTERVAL_S   = 30,  /* data-log averaging interval, s (0 = data log off) */
    PARAM_ID_STORE_STATUS     = 60,  /* RO: param_store status bits (param_store.h) */
    PARAM_ID_LOG_STATUS       = 61,  /* RO: plat_log status bits (plat_log.h) */
    PARAM_ID_PLAT_BLOCK_FIRST = 900, /* 900..999: platform retained blocks, not in the table */
    PARAM_ID_APP_FIRST        = 1000
};

enum { PARAM_MODE_LOCAL = 0, PARAM_MODE_REMOTE = 1 };

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

/* Any task: len bytes from offset off, as of the end of the last scan. */
param_err_t param_read_blob(uint16_t id, uint16_t off, void *buf, uint16_t len);

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
/* Called in the scan task after an external write changed a value (the event log
 * sets it). Scalars: old and new value. Blocks and the restore command: values 0. */
extern void (*param_on_change)(const param_def_t *d, plat_val_t old_v, plat_val_t new_v, uint8_t src, uint8_t level);

#endif
