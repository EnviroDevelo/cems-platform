/* plat_state.c - see plat_state.h */
#include <math.h>
#include <string.h>
#include "plat_state.h"
#include "plat_config.h"
#include "plat_port.h"
#include "param.h"
#include "plat_alarm.h"
#include "plat_clock.h"
#include "plat_log.h"
#include "bsp.h"

/* tags (parameters 300-303) */
uint8_t  plat_state_tag = PLAT_ST_STARTING;
bool     plat_maint_tag;
uint32_t plat_state_since_tag;
uint8_t  plat_meas_q[PLAT_MEAS_MAX];

static const plat_meas_def_t *M;
static size_t   n_meas;
static uint32_t stamp[PLAT_MEAS_MAX];        /* bsp ms of the last update (0 = never) */
static uint8_t  own[PLAT_MEAS_MAX];
static uint8_t  app_st = PLAT_ST_READY;      /* what the application last set */
static bool     app_cal, plat_cal_on, safe_mode;
static uint16_t stops[16];                   /* alarms marked AL_STOPS */
static size_t   n_stops;
static const char *refusal = "";

/* the quality each state gives a reading (MEASURING: the value's own) */
static const uint8_t state_q[PLAT_ST_COUNT] = {
    [PLAT_ST_STARTING] = PLAT_Q_UNAVAILABLE, [PLAT_ST_INITIALISING] = PLAT_Q_UNAVAILABLE,
    [PLAT_ST_WARMING_UP] = PLAT_Q_WARMUP,    [PLAT_ST_READY] = PLAT_Q_UNAVAILABLE,
    [PLAT_ST_MEASURING] = PLAT_Q_VALID,      [PLAT_ST_MAINTENANCE] = PLAT_Q_MAINT,
    [PLAT_ST_CALIBRATION] = PLAT_Q_CAL,      [PLAT_ST_FAULT] = PLAT_Q_FAULT,
    [PLAT_ST_SAFE_MODE] = PLAT_Q_UNAVAILABLE,
};

static const char *const st_name[PLAT_ST_COUNT] = {
    "STARTING", "INITIALISING", "WARMING UP", "READY", "MEASURING", "MAINTENANCE", "CALIBRATION", "FAULT", "SAFE MODE" };
static const char *const st_key[PLAT_ST_COUNT] = {
    "STARTING", "INITIALISING", "WARMING_UP", "READY", "MEASURING", "MAINTENANCE", "CALIBRATION", "FAULT", "SAFE_MODE" };
static const char *const q_name[PLAT_Q_COUNT] = {
    "", "stale", "unavailable", "out of range", "warming up", "maintenance", "calibration", "fault" };
static const char *const q_code[PLAT_Q_COUNT] = {
    "valid", "stale", "unavailable", "range", "warmup", "maint", "cal", "fault" };

const char *plat_state_name(uint8_t st)  { return st < PLAT_ST_COUNT ? st_name[st] : "?"; }
const char *plat_state_key(uint8_t st)   { return st < PLAT_ST_COUNT ? st_key[st] : "?"; }
const char *plat_quality_name(uint8_t q) { return q < PLAT_Q_COUNT ? q_name[q] : "?"; }
const char *plat_quality_code(uint8_t q) { return q < PLAT_Q_COUNT ? q_code[q] : "?"; }
uint8_t  plat_state(void)       { return plat_state_tag; }
uint32_t plat_state_since(void) { return plat_state_since_tag; }

static bool numeric(uint8_t ct) { return ct != CT_BLOB; }

int plat_state_init(const plat_meas_def_t *meas, size_t n) {
    M = meas; n_meas = 0;
    memset(stamp, 0, sizeof stamp);
    memset(plat_meas_q, PLAT_Q_UNAVAILABLE, sizeof plat_meas_q);
    for (size_t i = 0; i < n; ++i) {
        const param_def_t *d = param_find(meas[i].id);
        if (i >= PLAT_MEAS_MAX || !d) return (int)i + 1;
        if (meas[i].index == PLAT_MEAS_NO_INDEX) {
            if (d->ptype > PT_FLOAT) return (int)i + 1;
        } else if (d->ptype != PT_ARRAY || !numeric(d->ctype) || meas[i].index >= d->size / param_ctype_size(d->ctype)) {
            return (int)i + 1;
        }
        for (size_t k = 0; k < i; ++k)
            if (meas[k].id == meas[i].id && meas[k].index == meas[i].index) return (int)i + 1;
    }
    n_meas = n;
    n_stops = 0;
    for (size_t i = 0; i < alarm_count() && n_stops < sizeof stops / sizeof stops[0]; ++i) {
        const alarm_def_t *a = alarm_def_at(i);
        if (a && (a->flags & AL_STOPS)) stops[n_stops++] = a->id;
    }
    plat_state_tag = PLAT_ST_INITIALISING;
    return 0;
}

void plat_app_state(uint8_t st) {
    if (st == PLAT_ST_CALIBRATION) { app_cal = true; return; }
    if (st == PLAT_ST_WARMING_UP || st == PLAT_ST_READY || st == PLAT_ST_MEASURING) { app_st = st; app_cal = false; }
}

void plat_meas_update(unsigned i, uint8_t own_q) {
    if (i >= n_meas) return;
    uint32_t now = bsp_ms();
    stamp[i] = now ? now : 1u;
    own[i] = own_q < PLAT_Q_COUNT ? own_q : PLAT_Q_FAULT;
}

void plat_state_safe_mode(bool on) { safe_mode = on; }
void plat_state_cal(bool on)       { plat_cal_on = on; }

/* the value of measurement i as a float (the scan task owns the tags) */
static float value_of(size_t i) {
    const param_def_t *d = param_find(M[i].id);
    if (!d) return NAN;
    param_def_t t = *d;
    if (M[i].index != PLAT_MEAS_NO_INDEX) {
        t.ptr = (uint8_t *)d->ptr + (size_t)M[i].index * param_ctype_size(d->ctype);
        t.ptype = d->ctype == CT_F32 ? PT_FLOAT : d->ctype == CT_BOOL ? PT_BOOL
                : (d->ctype == CT_I8 || d->ctype == CT_I16 || d->ctype == CT_I32) ? PT_INT : PT_UINT;
    }
    plat_val_t v = param_tag_get(&t);
    switch (t.ptype) {
    case PT_FLOAT: return v.f;
    case PT_INT:   return (float)v.i;
    case PT_BOOL:  return v.b ? 1.0f : 0.0f;
    default:       return (float)v.u;
    }
}

static uint8_t effective(void) {
    if (safe_mode) return PLAT_ST_SAFE_MODE;
    bool fault = alarm_active(ALARM_ID_CAL_INVALID);
    for (size_t k = 0; k < n_stops && !fault; ++k) fault = alarm_active(stops[k]);
    if (fault) return PLAT_ST_FAULT;
    if (app_cal || plat_cal_on) return PLAT_ST_CALIBRATION;
    if (plat_maint_tag) return PLAT_ST_MAINTENANCE;
    return app_st;
}

void plat_state_scan(uint32_t now_ms) {
    uint8_t st = effective();
    if (st != plat_state_tag) {
        uint8_t was = plat_state_tag;
        plat_state_tag = st;
        uint32_t t;
        plat_state_since_tag = plat_clock_now(&t) ? t : 0u;
        if (was != PLAT_ST_INITIALISING || st != PLAT_ST_READY)    /* the normal start is not news */
            plat_log_event(LOG_EV_STATE, PLAT_SRC_INTERNAL, 0, st, was, st, st_key[st]);
    }
    for (size_t i = 0; i < n_meas; ++i) {
        uint8_t q = state_q[st];
        if (q == PLAT_Q_VALID) {
            float v;
            if (!stamp[i]) q = PLAT_Q_UNAVAILABLE;
            else if (own[i] != PLAT_Q_VALID) q = own[i];
            else if (M[i].max_age_ms && (uint32_t)(now_ms - stamp[i]) > M[i].max_age_ms) q = PLAT_Q_STALE;
            else if (M[i].lo < M[i].hi && !((v = value_of(i)) >= M[i].lo && v <= M[i].hi)) q = PLAT_Q_RANGE;
        }
        plat_meas_q[i] = q;
    }
}

bool plat_state_allows(uint8_t act) {
    uint8_t st = plat_state_tag;
    bool ok = true;
    switch (act) {
    case PLAT_ACT_FW_RESTART:       ok = st != PLAT_ST_CALIBRATION; break;
    case PLAT_ACT_RESTORE_DEFAULTS: ok = st != PLAT_ST_CALIBRATION; break;
    case PLAT_ACT_CALIBRATE:        ok = st != PLAT_ST_WARMING_UP && st != PLAT_ST_FAULT && st != PLAT_ST_SAFE_MODE
                                         && st != PLAT_ST_STARTING && st != PLAT_ST_INITIALISING; break;
    case PLAT_ACT_APP_LOGIC:        ok = st != PLAT_ST_SAFE_MODE; break;
    default: break;
    }
    if (!ok) {
        static char why[40];
        size_t k = 0;
        for (const char *p = "not allowed in "; *p; ++p) why[k++] = *p;
        for (const char *p = st_name[st]; *p && k + 1 < sizeof why; ++p) why[k++] = *p;
        why[k] = 0;
        refusal = why;
    }
    return ok;
}

const char *plat_state_refusal(void) { return refusal; }

/* ---- readers ---- */
static int find(uint16_t id, uint16_t index) {
    for (size_t i = 0; i < n_meas; ++i)
        if (M[i].id == id && (M[i].index == index || index == PLAT_MEAS_NO_INDEX)) return (int)i;
    return -1;
}

uint8_t param_quality(uint16_t id, uint16_t index) {
    int i = find(id, index);
    return i < 0 ? PLAT_Q_VALID : plat_meas_q[i];
}
bool param_has_quality(uint16_t id, uint16_t index) { return find(id, index) >= 0; }
size_t plat_meas_count(void) { return n_meas; }
const plat_meas_def_t *plat_meas_at(size_t i) { return i < n_meas ? &M[i] : NULL; }
uint8_t plat_meas_quality(size_t i) { return i < n_meas ? plat_meas_q[i] : PLAT_Q_VALID; }

int plat_state_request(uint8_t st, uint8_t src, uint8_t level) {
    if (st != PLAT_ST_MAINTENANCE && st != PLAT_ST_READY) return PERR_RANGE;
    return param_post(PARAM_ID_MAINTENANCE, PLAT_VAL_BOOL, (plat_val_t){ .b = st == PLAT_ST_MAINTENANCE }, src, level);
}

void plat_state_reset_for_test(void) {
    M = NULL; n_meas = 0; n_stops = 0;
    app_st = PLAT_ST_READY; app_cal = false; plat_cal_on = false; safe_mode = false;
    plat_state_tag = PLAT_ST_STARTING; plat_maint_tag = false; plat_state_since_tag = 0;
    memset(stamp, 0, sizeof stamp); memset(own, 0, sizeof own);
    memset(plat_meas_q, PLAT_Q_UNAVAILABLE, sizeof plat_meas_q);
    refusal = "";
}
