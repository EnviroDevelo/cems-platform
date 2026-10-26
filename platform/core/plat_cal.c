/* plat_cal.c - see plat_cal.h */
#include <string.h>
#include <math.h>
#include "plat_cal.h"
#include "param.h"
#include "param_store.h"
#include "plat_state.h"
#include "plat_log.h"
#include "plat_clock.h"
#include "plat_cmdq.h"
#include "bsp.h"

typedef struct {
    uint16_t head, count;               /* next slot to write; records held */
    uint32_t rsvd;
    plat_cal_record_t r[PLAT_CAL_HIST];
} hist_t;

/* FACTORY region: never reset by restore defaults (param_store.h) */
static plat_cal_record_t fac[PLAT_CAL_CH_MAX], cur[PLAT_CAL_CH_MAX];
static hist_t hist;

uint32_t plat_cal_cmd_tag;                      /* parameter 320: momentary command */
uint8_t  plat_cal_level_tag = PLAT_LVL_ADMIN;   /* parameter 321: login level that may calibrate */

static const retain_def_t blocks[] = {
    { .id = 902, .size = sizeof fac,  .ptr = fac,   .flags = RF_FACTORY, .name = "cal_factory" },
    { .id = 903, .size = sizeof cur,  .ptr = cur,   .flags = RF_FACTORY, .name = "cal_in_force" },
    { .id = 904, .size = sizeof hist, .ptr = &hist, .flags = RF_FACTORY, .name = "cal_history" },
};
const retain_def_t *plat_cal_blocks(size_t *n) { *n = sizeof blocks / sizeof blocks[0]; return blocks; }

static const plat_cal_channel_t *C;
static size_t n_ch;
static volatile uint32_t gen;                   /* odd while the scan task changes a record */
static volatile bool     running;
static volatile uint8_t  run_ch;
static plat_cal_record_t work;                  /* the calibration in progress */
static bool     proposed;
static uint32_t started_ms;
static unsigned slot;                           /* its history slot (written ABORTED at the start) */
static volatile bool any_due;
static const char *refusal = "";

extern param_err_t (*param_on_cal_cmd)(uint32_t cmd, uint8_t src, uint8_t level);   /* param.c */

static unsigned n_coef(unsigned ch) {
    unsigned n = 0;
    while (n < PLAT_CAL_COEF_MAX && C[ch].coef_id[n]) ++n;
    return n;
}

static void begin_write(void) { ++gen; }
static void end_write(void)   { ++gen; }

static uint32_t clock_now(void) { uint32_t t; return plat_clock_now(&t) ? t : 0u; }

static void log_cal(const plat_cal_record_t *r, const char *what) {
    char text[23];
    size_t k = 0;
    for (const char *p = C[r->channel].name; *p && k < 10; ++p) text[k++] = *p;
    text[k++] = ' ';
    for (const char *p = what; *p && k + 1 < sizeof text; ++p) text[k++] = *p;
    text[k] = 0;
    plat_log_event(LOG_EV_CAL, r->src, r->level, r->channel, r->kind, r->outcome, text);
}

/* ---- commands (scan task, from parameter 320) ---- */
int plat_cal_check(uint8_t cmd, unsigned ch, uint8_t level) {
    refusal = "";
    switch (cmd) {
    case PLAT_CAL_CMD_BEGIN:
        if (ch >= n_ch) { refusal = "no such channel"; return PERR_RANGE; }
        if (level < plat_cal_level_tag) { refusal = "login level too low"; return PERR_ACCESS; }
        if (running) { refusal = "a calibration is running"; return PERR_BUSY; }
        if (!plat_state_allows(PLAT_ACT_CALIBRATE) && !(level >= PLAT_LVL_FACTORY && plat_state() == PLAT_ST_FAULT)) {
            refusal = plat_state_refusal();
            return PERR_INTERLOCK;
        }
        return PERR_OK;
    case PLAT_CAL_CMD_ACCEPT:
    case PLAT_CAL_CMD_ABORT:
        if (!running) { refusal = "no calibration running"; return PERR_RANGE; }
        if (level < plat_cal_level_tag && level < work.level) { refusal = "login level too low"; return PERR_ACCESS; }
        return PERR_OK;
    case PLAT_CAL_CMD_REVERT:
        if (ch >= n_ch) { refusal = "no such channel"; return PERR_RANGE; }
        if (level < PLAT_LVL_FACTORY) { refusal = "factory login needed"; return PERR_ACCESS; }
        if (running) { refusal = "a calibration is running"; return PERR_BUSY; }
        if (fac[ch].kind != PLAT_CAL_FACTORY) { refusal = "no factory calibration"; return PERR_INTERLOCK; }
        return PERR_OK;
    default:
        refusal = "unknown command";
        return PERR_RANGE;
    }
}

const char *plat_cal_refusal(void) { return refusal; }

static void hist_put(unsigned at, const plat_cal_record_t *r) {
    begin_write();
    hist.r[at] = *r;
    end_write();
}

static unsigned hist_new(const plat_cal_record_t *r) {
    unsigned at = hist.head % PLAT_CAL_HIST;
    begin_write();
    hist.r[at] = *r;
    hist.head = (uint16_t)((at + 1u) % PLAT_CAL_HIST);
    if (hist.count < PLAT_CAL_HIST) ++hist.count;
    end_write();
    return at;
}

/* write all of the channel's coefficients, or none (each range-checked first) */
static bool write_coefs(unsigned ch, const float *coef, unsigned n) {
    for (unsigned k = 0; k < n; ++k)
        if (!isfinite(coef[k]) || param_check(C[ch].coef_id[k], PLAT_VAL_F32, (plat_val_t){ .f = coef[k] },
                                              PLAT_SRC_INTERNAL, PLAT_LVL_FACTORY) != PERR_OK) return false;
    for (unsigned k = 0; k < n; ++k) {
        plat_cmd_t c = { .id = C[ch].coef_id[k], .src = PLAT_SRC_INTERNAL, .kind = PLAT_VAL_F32,
                         .level = PLAT_LVL_FACTORY, .v.f = coef[k] };
        (void)param_apply(&c);
    }
    return true;
}

static void finish(uint8_t outcome, const char *why) {
    work.outcome = outcome;
    work.time = clock_now();
    if (outcome == PLAT_CAL_PASS) {
        begin_write();
        cur[work.channel] = work;
        if (work.kind == PLAT_CAL_FACTORY) fac[work.channel] = work;
        end_write();
    }
    hist_put(slot, &work);
    running = false;
    plat_state_cal(false);
    log_cal(&work, why);
}

static param_err_t on_cmd(uint32_t cmd, uint8_t src, uint8_t level) {
    uint8_t op = (uint8_t)(cmd >> 8);
    unsigned ch = cmd & 0xFFu;
    param_err_t e = (param_err_t)plat_cal_check(op, ch, level);
    if (e != PERR_OK) return e;
    if (op == PLAT_CAL_CMD_ACCEPT || op == PLAT_CAL_CMD_ABORT) ch = work.channel;
    switch (op) {
    case PLAT_CAL_CMD_BEGIN:
        memset(&work, 0, sizeof work);
        for (unsigned k = 0; k < PLAT_CAL_PTS_MAX; ++k) work.ref[k] = work.found[k] = work.left[k] = NAN;
        work.channel = (uint8_t)ch;
        work.kind = level >= PLAT_LVL_FACTORY ? PLAT_CAL_FACTORY : PLAT_CAL_FIELD;
        work.outcome = PLAT_CAL_ABORTED;            /* what a restart part way through leaves */
        work.level = level;
        work.src = src;
        work.n_coef = (uint8_t)n_coef(ch);
        work.time = clock_now();
        proposed = false;
        started_ms = bsp_ms();
        slot = hist_new(&work);
        run_ch = (uint8_t)ch;
        running = true;
        plat_state_cal(true);
        log_cal(&work, work.kind == PLAT_CAL_FACTORY ? "factory start" : "field start");
        return PERR_OK;
    case PLAT_CAL_CMD_ABORT:
        finish(PLAT_CAL_ABORTED, "aborted");
        return PERR_OK;
    case PLAT_CAL_CMD_ACCEPT: {
        const char *why = NULL;
        if (!proposed) why = "no result";
        if (!why && C[ch].check) why = C[ch].check(ch, &work);
        if (!why && !write_coefs(ch, work.coef, work.n_coef)) why = "coef out of range";
        if (why) { finish(PLAT_CAL_FAIL, why); return PERR_OK; }   /* the command was carried out: the result is FAIL */
        finish(PLAT_CAL_PASS, work.kind == PLAT_CAL_FACTORY ? "factory pass" : "field pass");
        return PERR_OK;
    }
    case PLAT_CAL_CMD_REVERT: {
        plat_cal_record_t r = fac[ch];
        if (!write_coefs(ch, r.coef, r.n_coef)) { refusal = "factory coef out of range"; return PERR_RANGE; }
        r.outcome = PLAT_CAL_REVERTED;
        r.level = level;
        r.src = src;
        r.time = clock_now();
        begin_write();
        cur[ch] = r;
        end_write();
        (void)hist_new(&r);
        log_cal(&r, "reverted to factory");
        return PERR_OK;
    }
    default:
        return PERR_RANGE;
    }
}

static int post(uint8_t op, unsigned ch, uint8_t src, uint8_t level) {
    int e = plat_cal_check(op, ch, level);
    if (e != PERR_OK) return e;
    return param_post(PARAM_ID_CAL_CMD, PLAT_VAL_U32, (plat_val_t){ .u = ((uint32_t)op << 8) | (ch & 0xFFu) }, src, level);
}

int plat_cal_begin(unsigned ch, uint8_t src, uint8_t level)   { return post(PLAT_CAL_CMD_BEGIN, ch, src, level); }
int plat_cal_end(bool accept, uint8_t src, uint8_t level)      { return post(accept ? PLAT_CAL_CMD_ACCEPT : PLAT_CAL_CMD_ABORT, run_ch, src, level); }
int plat_cal_revert(unsigned ch, uint8_t src, uint8_t level)  { return post(PLAT_CAL_CMD_REVERT, ch, src, level); }

/* ---- the application's procedure ---- */
bool plat_cal_point(unsigned i, float ref, float found, float left) {
    if (!running || i >= PLAT_CAL_PTS_MAX) return false;
    work.ref[i] = ref;
    work.found[i] = found;
    work.left[i] = left;
    if (i + 1u > work.n_pts) work.n_pts = (uint8_t)(i + 1u);
    return true;
}

bool plat_cal_propose(const float *coef, unsigned n) {
    if (!running || n != work.n_coef) return false;
    for (unsigned k = 0; k < n; ++k) work.coef[k] = coef[k];
    proposed = true;
    return true;
}

/* ---- start-up and scan ---- */
int plat_cal_init(const plat_cal_channel_t *ch, size_t n) {
    C = ch;
    n_ch = 0;
    running = false;
    param_on_cal_cmd = on_cmd;
    for (size_t i = 0; i < n; ++i) {
        if (i >= PLAT_CAL_CH_MAX || !ch[i].name || !ch[i].coef_id[0]) return (int)i + 1;
        for (unsigned k = 0; k < PLAT_CAL_COEF_MAX && ch[i].coef_id[k]; ++k) {
            const param_def_t *d = param_find(ch[i].coef_id[k]);
            if (!d || d->ptype != PT_FLOAT || !(d->flags & PF_FACTORY) || !(d->flags & PF_PERSIST)) return (int)i + 1;
        }
    }
    n_ch = n;
    return 0;
}

uint32_t plat_cal_due(unsigned ch) {
    plat_cal_record_t r;
    if (ch >= n_ch || !C[ch].interval_days || !plat_cal_in_force(ch, &r) || !r.time) return 0;
    return r.time + (uint32_t)C[ch].interval_days * 86400u;
}

void plat_cal_scan(uint32_t now_ms) {
    if (running && (uint32_t)(now_ms - started_ms) > PLAT_CAL_TIMEOUT_MS) finish(PLAT_CAL_ABORTED, "timed out");
    uint32_t t = clock_now();
    bool due = false;
    for (unsigned ch = 0; ch < n_ch && t; ++ch) {
        uint32_t d = plat_cal_due(ch);
        if (d && t >= d) due = true;
    }
    any_due = due;
}

/* ---- readers ---- */
static bool copy(const plat_cal_record_t *src, plat_cal_record_t *out) {
    for (int tries = 0; tries < 8; ++tries) {
        uint32_t g = gen;
        if (g & 1u) continue;
        *out = *src;
        if (gen == g) return true;
    }
    *out = *src;            /* the scan task kept writing: take it as it is */
    return true;
}

size_t      plat_cal_count(void) { return n_ch; }
const char *plat_cal_name(unsigned ch) { return ch < n_ch ? C[ch].name : ""; }
bool plat_cal_active(unsigned *ch) { bool r = running; if (r && ch) *ch = run_ch; return r; }
bool plat_cal_any_due(void) { return any_due; }

bool plat_cal_factory(unsigned ch, plat_cal_record_t *out) {
    if (ch >= n_ch) return false;
    copy(&fac[ch], out);
    return out->kind != PLAT_CAL_NONE;
}

bool plat_cal_in_force(unsigned ch, plat_cal_record_t *out) {
    if (ch >= n_ch) return false;
    copy(&cur[ch], out);
    return out->kind != PLAT_CAL_NONE;
}

bool plat_cal_history(unsigned i, plat_cal_record_t *out) {
    unsigned n = hist.count, head = hist.head;
    if (i >= n || n > PLAT_CAL_HIST) return false;
    copy(&hist.r[(head + PLAT_CAL_HIST - 1u - i) % PLAT_CAL_HIST], out);
    return out->kind != PLAT_CAL_NONE;
}

const char *plat_cal_outcome_name(uint8_t o) {
    static const char *const s[] = { "", "pass", "fail", "aborted", "reverted" };
    return o < sizeof s / sizeof s[0] ? s[o] : "";
}

void plat_cal_reset_for_test(void) {
    memset(fac, 0, sizeof fac); memset(cur, 0, sizeof cur); memset(&hist, 0, sizeof hist);
    C = NULL; n_ch = 0; running = false; proposed = false; any_due = false; refusal = "";
    plat_cal_cmd_tag = 0; plat_cal_level_tag = PLAT_LVL_ADMIN;
    param_on_cal_cmd = NULL;
}
