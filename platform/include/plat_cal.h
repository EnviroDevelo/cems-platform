/* plat_cal.h - calibration records: who calibrated what, when, against which references, and
 * with what result; factory calibration kept apart from field calibration (Common Instrument
 * Platform Specification CIP-18, CIP-17.6, CIP-19.3).
 *
 * The application declares its calibrated channels (plat_cal_channel_t, in plat_app_t). Each
 * names the PF_FACTORY float parameters that hold its coefficients. The procedure itself
 * (gases, waits, the maths) stays in the application (CIP-18.7); the platform owns the record,
 * the permission, the state and the history:
 *
 *   1. BEGIN (touchscreen, web or the application: plat_cal_begin). Refused unless the login
 *      level is at least setting 321 (calibration level, default admin) and the state allows it
 *      (plat_state.h; at factory level also in FAULT, so a unit with lost calibration data can be
 *      recalibrated). The instrument is in CALIBRATION until the end.
 *   2. The application's logic, seeing plat_cal_active(), runs its procedure. For each reference
 *      point it calls plat_cal_point() (reference value, reading as found, reading as left) and
 *      at the end plat_cal_propose() with the new coefficients.
 *   3. END with accept (plat_cal_end(true, ...), from anywhere, or the application itself). The
 *      channel's check hook may reject the whole calibration (CIP-18.4); otherwise every
 *      coefficient is range-checked, all are written or none, and the record says PASS. A
 *      rejection is a FAIL record and the coefficients in force do not change. END without accept,
 *      a timeout (PLAT_CAL_TIMEOUT_MS) or a restart while calibrating is ABORTED.
 *
 * Factory or field (CIP-18.3): a calibration done at factory login level is a factory
 * calibration. It replaces the channel's factory record, which a field calibration never
 * touches. "Revert to factory" (factory level) puts the factory record's coefficients back.
 *
 * Kept in the FACTORY region of FRAM (survives firmware updates and restore defaults,
 * CIP-18.5): per channel the factory record and the record in force (the last PASS or revert),
 * and the last PLAT_CAL_HIST records of any outcome across all channels. Every start and end is
 * also in the event log (LOG_EV_CAL). A channel with an interval has a due date (the record in
 * force + interval); platform alarm 21 CAL_DUE (warning) while any channel is overdue.
 *
 * Threading: the record writers run in the scan task (commands arrive through parameter 320,
 * like every other write). Readers copy under a sequence counter and may run in any task. */
#ifndef PLAT_CAL_H
#define PLAT_CAL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "plat_config.h"

#ifndef PLAT_CAL_CH_MAX
#if PLAT_FRAM_SIZE >= 131072u
#define PLAT_CAL_CH_MAX  8u           /* calibrated channels */
#define PLAT_CAL_HIST    12u          /* records kept, all channels together, newest replaces oldest */
#else
#define PLAT_CAL_CH_MAX  2u           /* the 32 KB development FRAM: 1 KB factory region */
#define PLAT_CAL_HIST    3u
#endif
#endif
#define PLAT_CAL_COEF_MAX 6u
#define PLAT_CAL_PTS_MAX  4u
#ifndef PLAT_CAL_TIMEOUT_MS
#define PLAT_CAL_TIMEOUT_MS (2u * 3600u * 1000u)   /* a calibration left running is aborted */
#endif

enum { PLAT_CAL_NONE = 0, PLAT_CAL_FACTORY, PLAT_CAL_FIELD };                      /* kind */
enum { PLAT_CAL_PASS = 1, PLAT_CAL_FAIL, PLAT_CAL_ABORTED, PLAT_CAL_REVERTED };    /* outcome */
enum { PLAT_CAL_CMD_BEGIN = 1, PLAT_CAL_CMD_ACCEPT, PLAT_CAL_CMD_ABORT, PLAT_CAL_CMD_REVERT };

/* One calibration: 84 bytes. kind NONE = empty slot. */
typedef struct {
    uint32_t time;                      /* clock at the end (local s since 1970; 0 = clock not set) */
    uint8_t  channel, kind, outcome;
    uint8_t  level, src;                /* who: login level (plat_level_t) and where (plat_src_t) */
    uint8_t  n_coef, n_pts, rsvd;
    float    coef[PLAT_CAL_COEF_MAX];   /* PASS/REVERTED: now in force; FAIL: what was proposed */
    float    ref[PLAT_CAL_PTS_MAX];     /* per point: the reference value applied */
    float    found[PLAT_CAL_PTS_MAX];   /* the reading before adjustment (as found) */
    float    left[PLAT_CAL_PTS_MAX];    /* the reading after (as left; NaN = not taken) */
} plat_cal_record_t;

/* Accept or reject a whole calibration before anything is written: NULL to accept, or a short
 * reason ("span drift > 10 %") that goes in the event log. Scan task. */
typedef const char *(*plat_cal_check_fn)(unsigned channel, const plat_cal_record_t *proposed);

typedef struct {
    const char       *name;                       /* "NOx", for screens and the event log */
    uint16_t          coef_id[PLAT_CAL_COEF_MAX]; /* PF_FACTORY float parameters; 0 ends the list */
    uint16_t          interval_days;              /* due this long after the record in force; 0 = no due date */
    plat_cal_check_fn check;                      /* may be NULL */
} plat_cal_channel_t;

/* Start-up, after param_init and before param_store_init. Returns 0, or the index + 1 of the
 * first bad channel (no name, a coefficient that is missing, not a float or not PF_FACTORY,
 * too many channels). */
int  plat_cal_init(const plat_cal_channel_t *ch, size_t n);
void plat_cal_scan(uint32_t now_ms);              /* scan task, every scan (plat_sys pre-logic) */

/* ---- commands: any task (posted to parameter 320; applied by the scan) ---- */
int plat_cal_begin(unsigned channel, uint8_t src, uint8_t level);
int plat_cal_end(bool accept, uint8_t src, uint8_t level);
int plat_cal_revert(unsigned channel, uint8_t src, uint8_t level);   /* factory level */
/* Would this command be accepted now? A param_err_t (PERR_OK, PERR_ACCESS, PERR_INTERLOCK,
 * PERR_BUSY, PERR_RANGE); plat_cal_refusal() then says why in words, for the screen. */
int plat_cal_check(uint8_t cmd, unsigned channel, uint8_t level);
const char *plat_cal_refusal(void);

/* ---- the application's procedure: scan task, while plat_cal_active() ---- */
bool plat_cal_point(unsigned i, float ref, float found, float left);
bool plat_cal_propose(const float *coef, unsigned n);

/* ---- readers: any task ---- */
size_t      plat_cal_count(void);
const char *plat_cal_name(unsigned channel);
bool        plat_cal_active(unsigned *channel);   /* a calibration is running (and on which channel) */
bool        plat_cal_factory(unsigned channel, plat_cal_record_t *out);   /* false = none */
bool        plat_cal_in_force(unsigned channel, plat_cal_record_t *out);  /* false = none */
bool        plat_cal_history(unsigned i, plat_cal_record_t *out);         /* i = 0 newest; false = no more */
/* When the channel is next due (local s since 1970); 0 = no due date (no interval, nothing in
 * force, or the record has no time). */
uint32_t    plat_cal_due(unsigned channel);
bool        plat_cal_any_due(void);               /* alarm 21 */
const char *plat_cal_outcome_name(uint8_t outcome);   /* "pass", "fail", "aborted", "reverted" */

void plat_cal_reset_for_test(void);

#endif
