/* plat_state.h - the instrument's operating state and the quality of its live values
 * (Common Instrument Platform Specification CIP-09 and CIP-10, section 7).
 *
 * State. One state for the whole instrument, shown on every interface and logged when it
 * changes. The application sets WARMING_UP, READY and MEASURING, and asks for CALIBRATION
 * (plat_app_state); the operator selects MAINTENANCE (parameter 301, or plat_state_request);
 * the platform forces FAULT (no valid calibration data, or an active alarm the application
 * marks AL_STOPS) and SAFE_MODE (reboot loop). Precedence, highest first:
 *     SAFE_MODE > FAULT > CALIBRATION > MAINTENANCE > the application's state
 * STARTING lasts until the parameter table is up, INITIALISING until the first scan.
 *
 * Quality. Each measurement the application declares (plat_meas_def_t, in plat_app_t) carries
 * one of eight codes. The application calls plat_meas_update() whenever it writes the value,
 * with the value's own quality (usually VALID). Every scan the platform works out the quality
 * that is reported:
 *     not MEASURING           -> what the state gives (table in plat_state.c)
 *     never updated           -> UNAVAILABLE
 *     its own quality         -> as given, if not VALID
 *     older than max_age_ms   -> STALE   (so a stalled application cannot leave a good-looking number)
 *     outside lo..hi, or NaN  -> RANGE
 *     otherwise               -> VALID
 * The result is in parameter 303 (an array, element i = measurement i, for a Modbus status
 * block), in param_quality() for the touchscreen and in /api/values.
 *
 * Writers: the scan task. Readers: any task (single bytes and words, published each scan). */
#ifndef PLAT_STATE_H
#define PLAT_STATE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

enum {
    PLAT_ST_STARTING = 0,
    PLAT_ST_INITIALISING,
    PLAT_ST_WARMING_UP,
    PLAT_ST_READY,
    PLAT_ST_MEASURING,
    PLAT_ST_MAINTENANCE,
    PLAT_ST_CALIBRATION,
    PLAT_ST_FAULT,
    PLAT_ST_SAFE_MODE,
    PLAT_ST_COUNT
};

enum {
    PLAT_Q_VALID = 0,
    PLAT_Q_STALE,
    PLAT_Q_UNAVAILABLE,
    PLAT_Q_RANGE,
    PLAT_Q_WARMUP,
    PLAT_Q_MAINT,
    PLAT_Q_CAL,
    PLAT_Q_FAULT,
    PLAT_Q_COUNT
};

/* Actions the state may refuse (section 7). */
enum {
    PLAT_ACT_FW_RESTART = 0,     /* restart into a new firmware image */
    PLAT_ACT_RESTORE_DEFAULTS,
    PLAT_ACT_CALIBRATE,          /* start a calibration */
    PLAT_ACT_APP_LOGIC           /* run the application's logic (not in SAFE_MODE) */
};

/* One measured value the application reports. */
#define PLAT_MEAS_NO_INDEX 0xFFFFu
typedef struct {
    uint16_t id;                 /* parameter-table ID (a scalar, or an array with index) */
    uint16_t index;              /* array element, or PLAT_MEAS_NO_INDEX for a scalar */
    uint32_t max_age_ms;         /* stale when not updated for this long (0 = never stale) */
    float    lo, hi;             /* valid range (lo >= hi: no range check) */
} plat_meas_def_t;

/* Start-up (plat_start, after param_init): the measurement table. Returns 0, or the index + 1
 * of the first bad entry (unknown ID, not numeric, index out of range, duplicate, too many:
 * PLAT_MEAS_MAX). */
int  plat_state_init(const plat_meas_def_t *meas, size_t n);
void plat_state_scan(uint32_t now_ms);            /* scan task, every scan (plat_sys pre-logic) */

/* ---- the application (scan task) ---- */
void plat_app_state(uint8_t st);                  /* WARMING_UP, READY, MEASURING or CALIBRATION; others ignored */
void plat_meas_update(unsigned i, uint8_t own_q); /* measurement i was just written, with its own quality */

/* ---- the platform ---- */
void plat_state_safe_mode(bool on);               /* the reboot-loop check (CIP-08.5) */
void plat_state_cal(bool on);                     /* a platform calibration is running (plat_cal.h) */
bool plat_state_allows(uint8_t act);              /* PLAT_ACT_*: false = refused in the current state */
const char *plat_state_refusal(void);             /* "not allowed during CALIBRATION" for the last refusal */

/* ---- readers (any task) ---- */
uint8_t     plat_state(void);
uint32_t    plat_state_since(void);               /* clock (local s since 1970) when it was entered; 0 = clock not set */
const char *plat_state_name(uint8_t st);          /* "WARMING UP": for people */
const char *plat_state_key(uint8_t st);           /* "WARMING_UP": for JSON and logs */
uint8_t     param_quality(uint16_t id, uint16_t index);   /* PLAT_Q_*; VALID for anything not declared */
bool        param_has_quality(uint16_t id, uint16_t index); /* index PLAT_MEAS_NO_INDEX on an array: any element */
const char *plat_quality_name(uint8_t q);         /* "out of range"; "" for valid: for people */
const char *plat_quality_code(uint8_t q);         /* "range": for JSON */
size_t      plat_meas_count(void);
const plat_meas_def_t *plat_meas_at(size_t i);
uint8_t     plat_meas_quality(size_t i);

/* Supervisory link (CIP-15.2): ms since the last Modbus or web request; UINT32_MAX if none
 * since start (param.c keeps it). */
uint32_t plat_link_age_ms(uint32_t now_ms);

/* Enter or leave MAINTENANCE for the touchscreen and web: st MAINTENANCE enters, READY leaves.
 * Posts parameter 301; returns a param_err_t. */
int plat_state_request(uint8_t st, uint8_t src, uint8_t level);

void plat_state_reset_for_test(void);

#endif
