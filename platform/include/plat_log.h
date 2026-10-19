/* plat_log.h - event log and data logger on serial NOR flash (design spec
 * "Data logger"; Analysis 70_Alarms_Logging section 8, 48_Logging faults L1-L8).
 *
 * Two rings in the flash, so routine data never pushes out the audit trail:
 *   event ring  power-up / reset cause, parameter changes (old -> new, who, from
 *               where), mode changes, logins, clock changes, alarms, firmware
 *               updates, storage faults
 *   data ring   one record per interval: the mean of each channel over the
 *               interval and its quality (OK / FLAG / BAD / N/A), plus a schema
 *               record (channel names and units) at start-up and on any change
 *
 * Fixes against the old program: the log wraps (overwrites the oldest sector)
 * instead of stopping (L1); a configuration change writes a new schema and the
 * CSV gets a new header, nothing is ever cleared (L3); values are real readings
 * averaged over the interval, not instantaneous samples (L5); every value has a
 * quality code (L6); timestamps are seconds since 1970 from the RTC (L7).
 *
 * Flash format: fixed 64-byte slots (a record is 1..17 consecutive slots), each
 * with its own CRC-32, so a power cut loses at most the record being written. A
 * slot is programmed only into erased flash; the sector after the one being
 * written is erased ahead of time (decision D12: overwrite oldest, one sector of
 * 4 KB of headroom). The write position is kept in FRAM (two copies) so start-up
 * is instant; if the FRAM copy is missing or disagrees with the flash, the
 * position is rebuilt by scanning the flash (status LSTAT_REBUILT).
 *
 * Threading
 *   plat_log_event()      any task (not ISR): queued in RAM, written by the store task
 *   plat_log_put()        scan task: one sample of a data channel
 *   plat_log_scan()       scan task, once per scan (plat_sys pre_logic)
 *   plat_log_init/poll()  store task (owner of the flash and the FRAM)
 *   plat_log_read_*()     any one task at a time: raw slots for CSV download */
#ifndef PLAT_LOG_H
#define PLAT_LOG_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nor.h"
#include "nvm.h"

#define LOG_SLOT        64u
#define LOG_MAX_CHANS   32u    /* data channels */
#define LOG_VALS_PER_SLOT 8u
#ifndef PLAT_LOG_QSLOTS
#define PLAT_LOG_QSLOTS 64u    /* RAM queue between producers and the store task (4 KB) */
#endif

/* Slot layout (little-endian):
 *   0 magic 0xC5 | 1 type (low 7 bits) + 0x80 if the clock was not set
 *   2 part | 3 nparts | 4 seq (u32, per ring, shared by the parts of a record)
 *   8 time (u32 s since 1970, or uptime s if the clock was not set)
 *  12..59 payload | 60 CRC-32 of bytes 0..59 */
#define LOG_MAGIC       0xC5u
enum { LOG_T_SCHEMA = 1, LOG_T_DATA = 2, LOG_T_EVENT = 3 };
#define LOG_F_NOCLOCK   0x80u
enum { LOG_RING_EVENTS = 0, LOG_RING_DATA = 1, LOG_RINGS = 2 };

/* Quality codes (worst over the interval wins; N/A = no valid sample) */
enum { LQ_OK = 0, LQ_FLAG = 1, LQ_BAD = 2, LQ_NA = 3 };

/* A data channel, defined by the application. */
typedef struct {
    const char *name;     /* up to 16 characters in the schema */
    const char *unit;     /* up to 6, may be NULL */
    uint8_t     dec;      /* decimals in the CSV */
} plat_log_chan_t;

/* Event codes. 1..99 platform, 100+ application. */
typedef enum {
    LOG_EV_POWER_UP = 1,     /* arg = reset cause (bsp_reset_cause_t), text = firmware / fault task */
    LOG_EV_PARAM,            /* arg = parameter ID, old -> new, text = name */
    LOG_EV_MODE,             /* LOCAL/REMOTE: old -> new */
    LOG_EV_RESTORE_DEFAULTS,
    LOG_EV_TIME_SET,         /* old -> new, s since 1970 */
    LOG_EV_LOGIN,            /* level, text = where from */
    LOG_EV_LOGIN_FAIL,
    LOG_EV_LOGOUT,
    LOG_EV_PASSWORD,         /* arg = level whose password changed */
    LOG_EV_RESTORE_BACKUP,   /* parameter backup file loaded */
    LOG_EV_ALARM_RAISE,      /* arg = alarm ID, text = name */
    LOG_EV_ALARM_CLEAR,
    LOG_EV_ALARM_ACK,
    LOG_EV_FW_UPDATE,        /* text = new version */
    LOG_EV_FW_ROLLBACK,
    LOG_EV_LOG_FAULT,        /* arg = LSTAT bits */
    LOG_EV_STORE_FAULT,      /* arg = PSTAT bits */
    LOG_EV_LOST,             /* arg = events lost because the RAM queue was full */
    LOG_EV_APP = 100
} log_event_code_t;

/* Status bits (also parameter PARAM_ID_LOG_STATUS) */
#define LSTAT_VOLATILE   0x0001u  /* RAM stand-in: nothing survives power-off */
#define LSTAT_NOR_FAULT  0x0002u  /* ALARM (LOG_STORAGE_FAULT): an erase, write or read-back failed */
#define LSTAT_REBUILT    0x0004u  /* write position rebuilt from the flash (FRAM copy missing/stale) */
#define LSTAT_LOST       0x0008u  /* records were lost: the RAM queue was full */
#define LSTAT_NO_CLOCK   0x0010u  /* the RTC is not set: times are uptime seconds */
#define LSTAT_TOO_SMALL  0x0020u  /* flash too small for two rings: logging off */

/* A decoded event (from plat_log_decode_event). */
typedef struct {
    uint32_t seq, time;
    bool     clock_set;
    uint16_t code, ms;
    uint8_t  src, level;
    uint32_t arg;
    double   old_v, new_v;
    char     text[23];
} log_event_t;

/* ---- producers ---- */
void plat_log_event(uint16_t code, uint8_t src, uint8_t level, uint32_t arg,
                    double old_v, double new_v, const char *text);
void plat_log_put(unsigned ch, float v, uint8_t quality);   /* scan task, any number per scan */
void plat_log_scan(uint32_t now_ms);                        /* scan task, once per scan */

/* ---- store task ---- */
typedef struct {
    nor_dev_t             *nor;        /* NULL: logging off */
    nvm_dev_t             *fram;       /* write pointer (NULL: always rebuild from flash) */
    uint32_t               fram_addr;  /* 64 bytes: STORE_META_ADDR */
    const plat_log_chan_t *chans;
    unsigned               n_chans;
    const char            *fw_version; /* in the power-up event and the schema */
    uint8_t                reset_cause;
    const char            *reset_note; /* e.g. the task the watchdog caught (may be NULL) */
    uint32_t               unix_now;   /* clock at start-up, 0 if not set */
} plat_log_cfg_t;
bool plat_log_init(const plat_log_cfg_t *cfg, uint32_t now_ms);
void plat_log_poll(uint32_t now_ms);           /* store task: drain the queue, run read jobs */
uint32_t plat_log_status(void);

/* ---- reading (one reader at a time; the store task does the flash access) ---- */
typedef struct {
    uint32_t nslots;       /* ring size */
    uint32_t oldest, wr;   /* slot indexes: oldest record .. next write */
    uint32_t used;         /* slots from oldest to wr */
    uint32_t next_seq;
} log_ring_info_t;
bool plat_log_info(uint8_t ring, log_ring_info_t *out);   /* false if logging is off */
/* Copy n raw slots starting at slot index pos (wrapping) into buf (n * 64 bytes).
 * Returns -1 if a job is already running or logging is off. Then poll _done.
 * n = 0 (buf may be NULL) completes once everything queued so far is in the flash. */
int  plat_log_read_start(uint8_t ring, uint32_t pos, uint32_t n, uint8_t *buf);
bool plat_log_read_done(int32_t *result);     /* result: slots copied, or -1 on a read error */

/* ---- decoding helpers (any task) ---- */
bool plat_log_slot_ok(const uint8_t *slot);   /* magic + CRC */
bool plat_log_decode_event(const uint8_t *slot, log_event_t *e);
uint16_t plat_log_schema_hash(const plat_log_chan_t *c, unsigned n, uint16_t interval_s);
const char *plat_log_event_name(uint16_t code);
uint32_t plat_unix_from_civil(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s);
void     plat_civil_from_unix(uint32_t t, unsigned *y, unsigned *mo, unsigned *d, unsigned *h, unsigned *mi, unsigned *s);

/* For tests: the queue and channel state are module statics; this resets them. */
void plat_log_reset_for_test(void);

#endif
