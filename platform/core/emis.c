/* emis.c - see emis.h. The formulas and their sources: docs/EMISSIONS_MATH.md. No parameters, no
 * HAL: the binding (emis_bind.c) feeds it and publishes its results. */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "emis.h"
#include "plat_state.h"

#define R_GAS        8.314462618    /* J/(mol K), CODATA 2018 (exact since the 2019 SI) */
#define H2O_MAX      80.0f          /* % vol: a water content above this is taken as a broken reading */
#define CO2_MIN      0.5f           /* % vol: below this a CO2 correction is not made */
#define WEEK_ABOVE_PCT   5u         /* EN 14181 6.5: more than 5 % of a week's values above the range ... */
#define WEEKS_ABOVE_MAX  5u         /* ... for 5 consecutive weeks, */
#define WEEK_ABOVE_ONE   40u        /* or more than 40 % in one week: a new QAL2 */
#define NO_PERIOD    0xFFFFFFFFu

/* ---------------- the formulas ---------------- */

/* Severity for combining: a value is as bad as the worst thing it was made from. A broken or
 * missing input outranks a stale one, which outranks the instrument's own modes. */
static const uint8_t q_rank[PLAT_Q_COUNT] = {
    [PLAT_Q_VALID] = 0, [PLAT_Q_RANGE] = 1, [PLAT_Q_WARMUP] = 2, [PLAT_Q_MAINT] = 3,
    [PLAT_Q_CAL] = 4,   [PLAT_Q_STALE] = 5, [PLAT_Q_UNAVAILABLE] = 6, [PLAT_Q_FAULT] = 7 };

uint8_t emis_q_worst(uint8_t a, uint8_t b) {
    if (a >= PLAT_Q_COUNT) a = PLAT_Q_FAULT;
    if (b >= PLAT_Q_COUNT) b = PLAT_Q_FAULT;
    return q_rank[a] >= q_rank[b] ? a : b;
}

float emis_molar_volume(float t_k, float p_kpa) { return (float)(R_GAS * t_k / p_kpa); }

float emis_ppm_to_mg(float ppm, float molar_mass, float t_k, float p_kpa) {
    return ppm * molar_mass / emis_molar_volume(t_k, p_kpa);
}
float emis_mg_to_ppm(float mg, float molar_mass, float t_k, float p_kpa) {
    return mg * emis_molar_volume(t_k, p_kpa) / molar_mass;
}
float emis_to_dry(float wet, float h2o_pct) { return wet * 100.0f / (100.0f - h2o_pct); }

float emis_norm_conc(float c_act, float t_k, float p_kpa, float t_ref_k, float p_ref_kpa) {
    return c_act * (t_k / t_ref_k) * (p_ref_kpa / p_kpa);
}
float emis_norm_flow(float q_act, float t_k, float p_kpa, float t_ref_k, float p_ref_kpa) {
    return q_act * (t_ref_k / t_k) * (p_kpa / p_ref_kpa);
}
float emis_o2_ref(float c, float o2_meas, float o2_ref) { return c * (21.0f - o2_ref) / (21.0f - o2_meas); }
float emis_co2_ref(float c, float co2_meas, float co2_ref) { return c * co2_ref / co2_meas; }

/* ---------------- configuration ---------------- */

void emis_cfg_default(emis_cfg_t *g) {
    memset(g, 0, sizeof *g);
    g->t_ref_k = 273.15f;  g->p_ref_kpa = 101.325f;
    g->diluent = EMIS_DIL_O2; g->ref_pct = 11.0f; g->o2_max = 20.0f;
    g->ch_o2 = 0; g->h2o_mode = EMIS_H2O_NONE; g->ch_h2o = 11;
    g->ch_t = EMIS_NONE; g->ch_p = EMIS_NONE; g->t_fixed_c = 0.0f; g->p_fixed_kpa = 101.325f;
    g->ch_flow = EMIS_NONE;
    g->period_s = 1800u; g->min_avail = 2.0f / 3.0f; g->max_discard = 5u;
    g->ci_subtract = 1u; g->clamp_zero = 1u; g->cal_ext_pct = 10.0f;
}

void emis_chs_default(emis_chs_t *c) {
    memset(c, 0, sizeof *c);
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) { c->unit[i] = EMIS_U_OFF; c->add[i] = EMIS_NONE; }
}

static bool is_conc(uint8_t u) { return u == EMIS_U_PPM || u == EMIS_U_MG_ACT || u == EMIS_U_MG_STD; }
static bool is_flow(uint8_t u) { return u == EMIS_U_FLOW_ACT || u == EMIS_U_FLOW_STD; }
static bool ch_ok(uint8_t c)   { return c == EMIS_NONE || c < EMIS_CH_MAX; }
static bool fin(float f)       { return isfinite(f) != 0; }

static int bad(char *why, size_t cap, int ret, const char *what, unsigned ch) {
    if (why && cap) {
        if (ret <= (int)EMIS_CH_MAX) snprintf(why, cap, "channel %u: %s", ch, what);
        else snprintf(why, cap, "%s", what);
    }
    return ret;
}

int emis_check(const emis_cfg_t *g, const emis_chs_t *ch, char *why, size_t cap) {
    const int P = (int)EMIS_CH_MAX + 1;
    if (!(g->t_ref_k > 0.0f) || !(g->p_ref_kpa > 0.0f) || !fin(g->t_ref_k) || !fin(g->p_ref_kpa))
        return bad(why, cap, P, "reference temperature and pressure must be above zero", 0);
    if (g->period_s < 60u || 86400u % g->period_s)
        return bad(why, cap, P, "the averaging period must divide a day (600, 1800, 3600 s)", 0);
    if (!(g->min_avail >= 0.0f && g->min_avail <= 1.0f))
        return bad(why, cap, P, "the minimum valid share must be 0..1", 0);
    if (!ch_ok(g->ch_o2) || !ch_ok(g->ch_h2o) || !ch_ok(g->ch_t) || !ch_ok(g->ch_p) || !ch_ok(g->ch_flow))
        return bad(why, cap, P, "unknown channel number", 0);
    if (g->diluent > EMIS_DIL_CO2 || g->h2o_mode > EMIS_H2O_FIXED)
        return bad(why, cap, P, "unknown diluent or water mode", 0);
    if (g->diluent == EMIS_DIL_O2) {
        if (!(g->ref_pct >= 0.0f && g->ref_pct < 21.0f) || !(g->o2_max > 0.0f && g->o2_max < 21.0f))
            return bad(why, cap, P, "reference O2 and the O2 limit must be below 21 %", 0);
    } else if (g->diluent == EMIS_DIL_CO2 && !(g->ref_pct > 0.0f && g->ref_pct <= 100.0f)) {
        return bad(why, cap, P, "reference CO2 must be above 0 %", 0);
    }
    if (g->diluent != EMIS_DIL_NONE && (g->ch_o2 == EMIS_NONE || ch->unit[g->ch_o2] != EMIS_U_PCT))
        return bad(why, cap, P, "the diluent channel must be a % reading", 0);
    if (g->h2o_mode == EMIS_H2O_MEASURED &&
        (g->ch_h2o == EMIS_NONE || ch->unit[g->ch_h2o] != EMIS_U_PCT || ch->wet[g->ch_h2o] || ch->ref[g->ch_h2o]))
        return bad(why, cap, P, "the water channel must be a % reading, not wet-corrected or O2-corrected", 0);
    if (g->h2o_mode == EMIS_H2O_FIXED && !(g->h2o_fixed >= 0.0f && g->h2o_fixed <= H2O_MAX))
        return bad(why, cap, P, "the fixed water content must be 0..80 %", 0);
    if (g->ch_t != EMIS_NONE && (ch->unit[g->ch_t] != EMIS_U_OTHER || ch->ref[g->ch_t] || ch->wet[g->ch_t]))
        return bad(why, cap, P, "the temperature channel must be a plain reading (degC)", 0);
    if (g->ch_p != EMIS_NONE && (ch->unit[g->ch_p] != EMIS_U_OTHER || ch->ref[g->ch_p] || ch->wet[g->ch_p]))
        return bad(why, cap, P, "the pressure channel must be a plain reading (kPa absolute)", 0);
    if (!(g->t_fixed_c > -273.15f) || !(g->p_fixed_kpa > 0.0f))
        return bad(why, cap, P, "the fixed temperature and pressure must be physical", 0);
    if (g->ch_flow != EMIS_NONE && !is_flow(ch->unit[g->ch_flow]))
        return bad(why, cap, P, "the flow channel must be a flow reading", 0);
    if (!(g->cal_ext_pct >= 0.0f && g->cal_ext_pct <= 100.0f))
        return bad(why, cap, P, "the calibration range extension must be 0..100 % of the ELV", 0);
    if (g->period2_s && (g->period2_s < 60u || 86400u % g->period2_s))
        return bad(why, cap, P, "the second averaging period must be off or divide a day (600 s)", 0);

    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        if (ch->unit[i] >= EMIS_U_COUNT) return bad(why, cap, (int)i + 1, "unknown unit", i);
        if (ch->unit[i] == EMIS_U_OFF) continue;
        if (!fin(ch->qal2_a[i]) || !fin(ch->qal2_b[i]) || !fin(ch->ys_max[i]) || !fin(ch->elv[i]) ||
            !fin(ch->molar_mass[i]) || !fin(ch->elv2[i]))
            return bad(why, cap, (int)i + 1, "a setting is not a number", i);
        if (ch->unit[i] == EMIS_U_PPM && !(ch->molar_mass[i] > 0.0f))
            return bad(why, cap, (int)i + 1, "a ppm reading needs the molar mass", i);
        if (ch->ref[i] && !is_conc(ch->unit[i]))
            return bad(why, cap, (int)i + 1, "only concentrations are corrected to the reference O2", i);
        if (ch->ref[i] && g->diluent == EMIS_DIL_NONE)
            return bad(why, cap, (int)i + 1, "O2 correction asked for, but no reference diluent is set", i);
        if (ch->wet[i] && g->h2o_mode == EMIS_H2O_NONE && (is_conc(ch->unit[i]) || ch->unit[i] == EMIS_U_PCT || is_flow(ch->unit[i])))
            return bad(why, cap, (int)i + 1, "a wet reading needs the water content (measured or fixed)", i);
        if (ch->wet[i] && ch->unit[i] == EMIS_U_OTHER) return bad(why, cap, (int)i + 1, "a plain reading has no wet basis", i);
        if (!(ch->ci_pct[i] >= 0.0f && ch->ci_pct[i] <= 100.0f) || ch->elv[i] < 0.0f || ch->ys_max[i] < 0.0f ||
            ch->elv2[i] < 0.0f)
            return bad(why, cap, (int)i + 1, "ELV, ys,max and the confidence interval must not be negative", i);
        if (ch->add[i] != EMIS_NONE) {
            if (ch->add[i] >= EMIS_CH_MAX || ch->add[i] == i || ch->unit[ch->add[i]] != ch->unit[i])
                return bad(why, cap, (int)i + 1, "the added reading must be another channel in the same unit", i);
        }
    }
    return 0;
}

/* ---------------- the engine ---------------- */

typedef struct {                  /* 32 bytes a channel */
    double   sum;                 /* B_REF x ms over the period's valid time */
    uint32_t valid_ms;
    float    day_sum;             /* validated short-term averages of the day (at most 144) */
    uint16_t day_n, day_disc;
    uint16_t week_n, week_above;
    uint8_t  q_bad;               /* worst non-valid quality seen while operating */
    uint8_t  day_q_bad;
} acc_t;

typedef struct {                  /* the second period's average in progress, 16 bytes a channel */
    double   sum;
    uint32_t valid_ms;
    uint8_t  q_bad;
} acc2_t;

static const emis_cfg_t    *G;       /* the caller's: kept, not copied (RAM) */
static const emis_chs_t *C;
emis_res_t                 emis_res;
#define R emis_res
static acc_t         A[EMIS_CH_MAX];
static acc2_t        A2[EMIS_CH_MAX];
static uint32_t      op_ms;              /* operating time in the period so far (the same for every channel) */
static uint32_t      op2_ms;             /* and in the second period so far */
static bool          day_op;             /* the day so far had operating time */
/* Within a step, B_STD and q_calc first hold the calibrated reading y and its quality (pass 1),
 * then B_STD in place (pass 2): no scratch arrays. */
#define Y  R.v[EMIS_B_STD]
#define QY R.q[EMIS_Q_CALC]
static uint32_t      cur_period = NO_PERIOD, cur_period2 = NO_PERIOD, cur_day, cur_week;
static int32_t       cur_year;

static void out_clear(unsigned i) {
    for (unsigned b = 0; b < EMIS_BANKS; ++b) R.v[b][i] = 0.0f;
    for (unsigned k = 0; k < EMIS_QS; ++k) R.q[k][i] = PLAT_Q_UNAVAILABLE;
    R.status[i] = R.discarded[i] = R.days_invalid[i] = R.weeks_above[i] = 0u;
    R.v2[i] = 0.0f; R.q2[i] = PLAT_Q_UNAVAILABLE;
    R.n2[i] = R.n2_above[i] = R.n2_day[i] = R.n2_above_day[i] = 0u;
}

void emis_init(const emis_cfg_t *g, const emis_chs_t *ch) {
    G = g; C = ch;
    memset(A, 0, sizeof A);
    memset(A2, 0, sizeof A2);
    op_ms = op2_ms = 0u; day_op = false;
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) { out_clear(i); A[i].q_bad = A[i].day_q_bad = A2[i].q_bad = PLAT_Q_VALID; }
    cur_period = cur_period2 = NO_PERIOD;
}

void emis_clear(void) {
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) out_clear(i);
}

void emis_reset_for_test(void) { G = NULL; C = NULL; emis_clear(); }

emis_out_t emis_out(unsigned ch) {
    emis_out_t o;
    memset(&o, 0, sizeof o);
    if (ch >= EMIS_CH_MAX) { o.q_in = o.q_calc = o.q_mass = o.q_avg = o.q_day = o.q_run = o.q_avg2 = PLAT_Q_UNAVAILABLE; return o; }
    for (unsigned b = 0; b < EMIS_BANKS; ++b) o.v[b] = R.v[b][ch];
    o.q_in = R.q[EMIS_Q_IN][ch]; o.q_calc = R.q[EMIS_Q_CALC][ch]; o.q_mass = R.q[EMIS_Q_MASS][ch];
    o.q_avg = R.q[EMIS_Q_AVG][ch]; o.q_day = R.q[EMIS_Q_DAY][ch]; o.q_run = R.q[EMIS_Q_RUN][ch];
    o.status = R.status[ch]; o.discarded_today = R.discarded[ch];
    o.days_invalid = R.days_invalid[ch]; o.weeks_above = R.weeks_above[ch];
    o.avg2 = R.v2[ch]; o.q_avg2 = R.q2[ch];
    o.n2_today = R.n2[ch]; o.n2_above_today = R.n2_above[ch];
    o.n2_day = R.n2_day[ch]; o.n2_above_day = R.n2_above_day[ch];
    return o;
}

/* Gregorian year of a day count since 1970-01-01 (H. Hinnant's civil_from_days). */
static int32_t year_of(uint32_t days) {
    int32_t z = (int32_t)days + 719468;
    int32_t era = z / 146097;
    int32_t doe = z - era * 146097;
    int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int32_t mp = (5 * doy + 2) / 153;
    int32_t m = mp < 10 ? mp + 3 : mp - 9;
    return yoe + era * 400 + (m <= 2);
}

/* the upper end of the valid calibration range at standard conditions and reference O2 (0: none) */
static float cal_upper(unsigned i) {
    if (!(C->ys_max[i] > 0.0f)) return 0.0f;
    return C->ys_max[i] + C->elv[i] * G->cal_ext_pct / 100.0f;
}

static void close_period(unsigned i) {
    acc_t *a = &A[i];
    if (op_ms == 0u) {                                   /* not operating: no average, nothing discarded */
        R.v[EMIS_B_AVG][i] = R.v[EMIS_B_VAL][i] = 0.0f;
        R.q[EMIS_Q_AVG][i] = PLAT_Q_UNAVAILABLE;
        R.status[i] &= (uint16_t)~EMIS_ST_AVG_INCOMPLETE;
    } else if (a->valid_ms > 0u && (double)a->valid_ms + 0.5 >= (double)G->min_avail * (double)op_ms) {
        float avg = (float)(a->sum / (double)a->valid_ms);
        float val = avg;
        if (G->ci_subtract && C->elv[i] > 0.0f) val = avg - C->elv[i] * C->ci_pct[i] / 100.0f;
        if (G->clamp_zero && val < 0.0f) val = 0.0f;
        R.v[EMIS_B_AVG][i] = avg; R.v[EMIS_B_VAL][i] = val;
        R.q[EMIS_Q_AVG][i] = PLAT_Q_VALID;
        R.status[i] &= (uint16_t)~EMIS_ST_AVG_INCOMPLETE;
        a->day_sum += val; a->day_n++;
        a->week_n++;
        float up = cal_upper(i);
        if (up > 0.0f && avg > up) a->week_above++;
    } else {                                                /* discarded: AMS malfunction or maintenance */
        R.v[EMIS_B_AVG][i] = R.v[EMIS_B_VAL][i] = 0.0f;
        R.q[EMIS_Q_AVG][i] = a->q_bad != PLAT_Q_VALID ? a->q_bad : PLAT_Q_UNAVAILABLE;
        R.status[i] |= EMIS_ST_AVG_INCOMPLETE;
        a->day_disc++;
        a->day_q_bad = emis_q_worst(a->day_q_bad, R.q[EMIS_Q_AVG][i]);
        if (R.discarded[i] < 0xFFFFu) R.discarded[i]++;
    }
    a->sum = 0.0; a->valid_ms = 0u; a->q_bad = PLAT_Q_VALID;
}

/* the second period (IED Annex VI Part 8 1.2: from the validated values, within the operating time) */
static void close_period2(unsigned i) {
    acc2_t *a = &A2[i];
    if (op2_ms == 0u) {
        R.v2[i] = 0.0f; R.q2[i] = PLAT_Q_UNAVAILABLE;
    } else if (a->valid_ms > 0u && (double)a->valid_ms + 0.5 >= (double)G->min_avail * (double)op2_ms) {
        float val = (float)(a->sum / (double)a->valid_ms);
        if (G->ci_subtract && C->elv[i] > 0.0f) val -= C->elv[i] * C->ci_pct[i] / 100.0f;
        if (G->clamp_zero && val < 0.0f) val = 0.0f;
        R.v2[i] = val; R.q2[i] = PLAT_Q_VALID;
        if (R.n2[i] < 0xFFFFu) R.n2[i]++;
        if (C->elv2[i] > 0.0f && val > C->elv2[i] && R.n2_above[i] < 0xFFFFu) R.n2_above[i]++;
    } else {
        R.v2[i] = 0.0f; R.q2[i] = a->q_bad != PLAT_Q_VALID ? a->q_bad : PLAT_Q_UNAVAILABLE;
    }
    a->sum = 0.0; a->valid_ms = 0u; a->q_bad = PLAT_Q_VALID;
}

static void close_day(unsigned i) {
    acc_t *a = &A[i];
    if (!day_op) {
        R.v[EMIS_B_DAY][i] = 0.0f; R.q[EMIS_Q_DAY][i] = PLAT_Q_UNAVAILABLE;
        R.status[i] &= (uint16_t)~EMIS_ST_DAY_INVALID;
    } else if (a->day_n > 0u && a->day_disc <= G->max_discard) {
        R.v[EMIS_B_DAY][i] = a->day_sum / (float)a->day_n; R.q[EMIS_Q_DAY][i] = PLAT_Q_VALID;
        R.status[i] &= (uint16_t)~EMIS_ST_DAY_INVALID;
    } else {
        R.v[EMIS_B_DAY][i] = 0.0f;
        R.q[EMIS_Q_DAY][i] = a->day_q_bad != PLAT_Q_VALID ? a->day_q_bad : PLAT_Q_UNAVAILABLE;
        R.status[i] |= EMIS_ST_DAY_INVALID;
        if (R.days_invalid[i] < 0xFFFFu) R.days_invalid[i]++;
    }
    a->day_sum = 0.0f; a->day_n = a->day_disc = 0u; a->day_q_bad = PLAT_Q_VALID;
    R.discarded[i] = 0u;
    R.n2_day[i] = R.n2[i]; R.n2_above_day[i] = R.n2_above[i];
    R.n2[i] = R.n2_above[i] = 0u;
}

static void close_week(unsigned i) {
    acc_t *a = &A[i];
    if (a->week_n > 0u) {
        if ((uint32_t)a->week_above * 100u > (uint32_t)a->week_n * WEEK_ABOVE_PCT) {
            if (R.weeks_above[i] < 0xFFFFu) R.weeks_above[i]++;
        } else {
            R.weeks_above[i] = 0u;
        }
        if (R.weeks_above[i] >= WEEKS_ABOVE_MAX || (uint32_t)a->week_above * 100u > (uint32_t)a->week_n * WEEK_ABOVE_ONE)
            R.status[i] |= EMIS_ST_QAL2_REVIEW;               /* latched until the configuration changes (a new QAL2) */
    }
    a->week_n = a->week_above = 0u;
}

/* the clock: close whatever ended since the last step */
static void tick_clock(uint32_t clock_s) {
    if (clock_s == 0u) {                                    /* no clock: no averages; the period in progress is dropped */
        if (cur_period != NO_PERIOD)
            { for (unsigned i = 0; i < EMIS_CH_MAX; ++i) { A[i].sum = 0.0; A[i].valid_ms = 0u; } op_ms = 0u; }
        for (unsigned i = 0; i < EMIS_CH_MAX; ++i) { A2[i].sum = 0.0; A2[i].valid_ms = 0u; }
        op2_ms = 0u;
        cur_period = cur_period2 = NO_PERIOD;
        return;
    }
    if (G->period2_s) {                                     /* first, so its last period of a day counts in that day */
        uint32_t p2 = clock_s / G->period2_s;
        if (cur_period2 != NO_PERIOD && p2 != cur_period2) {
            for (unsigned i = 0; i < EMIS_CH_MAX; ++i) if (C->unit[i] != EMIS_U_OFF) close_period2(i);
            op2_ms = 0u;
        }
        cur_period2 = p2;
    }
    uint32_t per = clock_s / G->period_s, day = clock_s / 86400u, week = (day + 3u) / 7u;   /* weeks start on Monday */
    if (cur_period == NO_PERIOD) {                          /* first clock: start mid-period */
        cur_period = per; cur_day = day; cur_week = week; cur_year = year_of(day);
        return;
    }
    if (per == cur_period) return;
    /* the period ended (normally, or the clock stepped: the period ends where it is) */
    bool new_day = day != cur_day, new_week = week != cur_week;
    if (op_ms) day_op = true;
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        if (C->unit[i] == EMIS_U_OFF) continue;
        close_period(i);
        if (new_day) close_day(i);
        if (new_week) close_week(i);
    }
    op_ms = 0u;
    if (new_day) day_op = false;
    int32_t yr = year_of(day);
    if (new_day && yr != cur_year)
        for (unsigned i = 0; i < EMIS_CH_MAX; ++i) R.days_invalid[i] = 0u;
    cur_period = per; cur_day = day; cur_week = week; cur_year = yr;
}

/* temperature (K) and pressure (kPa) of the stack, with their quality */
static float stack_t(uint8_t *q) {
    if (G->ch_t == EMIS_NONE) return G->t_fixed_c + 273.15f;
    *q = emis_q_worst(*q, QY[G->ch_t]);
    float t = Y[G->ch_t] + 273.15f;
    if (!(t > 0.0f)) { *q = emis_q_worst(*q, PLAT_Q_RANGE); return G->t_ref_k; }
    return t;
}
static float stack_p(uint8_t *q) {
    if (G->ch_p == EMIS_NONE) return G->p_fixed_kpa;
    *q = emis_q_worst(*q, QY[G->ch_p]);
    float p = Y[G->ch_p];
    if (!(p > 0.0f)) { *q = emis_q_worst(*q, PLAT_Q_RANGE); return G->p_ref_kpa; }
    return p;
}
/* water content, % of the wet gas; st gets FIXED_H2O or INPUT_MISSING */
static float water(uint8_t *q, uint16_t *st) {
    if (G->h2o_mode == EMIS_H2O_FIXED) { *st |= EMIS_ST_FIXED_H2O; return G->h2o_fixed; }
    if (G->h2o_mode != EMIS_H2O_MEASURED || G->ch_h2o == EMIS_NONE) {
        *st |= EMIS_ST_INPUT_MISSING; *q = emis_q_worst(*q, PLAT_Q_UNAVAILABLE); return 0.0f;
    }
    *q = emis_q_worst(*q, QY[G->ch_h2o]);
    float h = Y[G->ch_h2o];
    if (!(h >= 0.0f && h <= H2O_MAX)) { *q = emis_q_worst(*q, PLAT_Q_RANGE); return h > 0.0f ? H2O_MAX : 0.0f; }
    return h;
}

/* B_STD of channel i from its calibrated reading */
static float to_std(unsigned i, uint8_t *q, uint16_t *st) {
    float y = Y[i], v = y;
    if (i == G->ch_h2o && G->h2o_mode == EMIS_H2O_MEASURED) return y;    /* the water content itself */
    switch (C->unit[i]) {
    case EMIS_U_PPM:
        v = emis_ppm_to_mg(y, C->molar_mass[i], G->t_ref_k, G->p_ref_kpa);
        break;
    case EMIS_U_MG_ACT: {
        float t = stack_t(q), p = stack_p(q);
        v = emis_norm_conc(y, t, p, G->t_ref_k, G->p_ref_kpa);
        break; }
    case EMIS_U_FLOW_ACT: {
        float t = stack_t(q), p = stack_p(q);
        v = emis_norm_flow(y, t, p, G->t_ref_k, G->p_ref_kpa);
        break; }
    default:                                                /* % vol, mg/m3 and flow at reference, plain */
        break;
    }
    if (C->wet[i]) {
        float h = water(q, st);
        v = is_flow(C->unit[i]) ? v * (100.0f - h) / 100.0f : emis_to_dry(v, h);
    }
    return v;
}

void emis_step(const emis_in_t in[EMIS_CH_MAX], bool operating, uint32_t clock_s, uint32_t dt_ms) {
    if (!G) return;
    tick_clock(clock_s);

    /* 1. raw reading and the calibration function (EN 14181 6.4: in the measuring conditions) */
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        R.status[i] &= (uint16_t)(EMIS_ST_AVG_INCOMPLETE | EMIS_ST_DAY_INVALID | EMIS_ST_QAL2_REVIEW);
        if (C->unit[i] == EMIS_U_OFF) { Y[i] = 0.0f; QY[i] = PLAT_Q_UNAVAILABLE; continue; }
        float x = in[i].x; uint8_t q = in[i].q < PLAT_Q_COUNT ? in[i].q : PLAT_Q_FAULT;
        if (C->add[i] != EMIS_NONE) {
            x += in[C->add[i]].x;
            q = emis_q_worst(q, in[C->add[i]].q);
        }
        if (!fin(x)) { x = 0.0f; q = emis_q_worst(q, PLAT_Q_RANGE); }
        uint8_t rg = in[i].range;
        if (C->add[i] != EMIS_NONE) rg |= in[C->add[i]].range;
        if (rg & EMIS_IN_OVER)  { R.status[i] |= EMIS_ST_OVER_RANGE;  q = emis_q_worst(q, PLAT_Q_RANGE); }
        if (rg & EMIS_IN_BELOW) { R.status[i] |= EMIS_ST_BELOW_RANGE; q = emis_q_worst(q, PLAT_Q_RANGE); }
        float y = x;
        if (C->qal2_b[i] != 0.0f) y = C->qal2_a[i] + C->qal2_b[i] * x;
        else R.status[i] |= EMIS_ST_NO_QAL2;
        if (!(G->ci_subtract && C->elv[i] > 0.0f)) R.status[i] |= EMIS_ST_CI_OFF;
        R.v[EMIS_B_RAW][i] = x; R.q[EMIS_Q_IN][i] = q;
        Y[i] = y; QY[i] = q;
    }

    /* 2. standard conditions, dry, in place. The temperature, pressure and water channels other
     * channels read are plain readings: their B_STD is their y, so the order does not matter. */
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        if (C->unit[i] == EMIS_U_OFF) continue;
        uint8_t q = QY[i];
        float v = to_std(i, &q, &R.status[i]);
        if (!fin(v)) { v = 0.0f; q = emis_q_worst(q, PLAT_Q_RANGE); }
        Y[i] = v; QY[i] = q;
    }
    if (cur_period != NO_PERIOD && operating) op_ms += dt_ms;   /* IED: within the effective operating time */
    if (cur_period2 != NO_PERIOD && operating) op2_ms += dt_ms;

    /* 3. reference diluent, mass emission, averages */
    float dil = 0.0f; uint8_t qd = PLAT_Q_UNAVAILABLE;
    if (G->diluent != EMIS_DIL_NONE && G->ch_o2 != EMIS_NONE) { dil = R.v[EMIS_B_STD][G->ch_o2]; qd = R.q[EMIS_Q_CALC][G->ch_o2]; }
    float flow = 0.0f; uint8_t qf = PLAT_Q_UNAVAILABLE;
    if (G->ch_flow != EMIS_NONE) { flow = R.v[EMIS_B_STD][G->ch_flow]; qf = R.q[EMIS_Q_CALC][G->ch_flow]; }

    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        acc_t *a = &A[i];
        if (C->unit[i] == EMIS_U_OFF) { out_clear(i); continue; }
        float v = R.v[EMIS_B_STD][i], r = v; uint8_t q_std = R.q[EMIS_Q_CALC][i], q = q_std;
        if (C->ref[i]) {
            q = emis_q_worst(q, qd);
            if (G->diluent == EMIS_DIL_O2) {
                float om = dil;
                if (om > G->o2_max) { om = G->o2_max; R.status[i] |= EMIS_ST_O2_HIGH; q = emis_q_worst(q, PLAT_Q_RANGE); }
                if (!(G->only_above && om <= G->ref_pct)) r = emis_o2_ref(v, om, G->ref_pct);
            } else {
                float cm = dil;
                if (!(cm >= CO2_MIN)) { cm = CO2_MIN; q = emis_q_worst(q, PLAT_Q_RANGE); }
                r = emis_co2_ref(v, cm, G->ref_pct);
            }
            if (!fin(r)) { r = 0.0f; q = emis_q_worst(q, PLAT_Q_RANGE); }
        }
        R.v[EMIS_B_STD][i] = v; R.v[EMIS_B_REF][i] = r; R.q[EMIS_Q_CALC][i] = q;
        float up = cal_upper(i);
        if (up > 0.0f && q == PLAT_Q_VALID && r > up) R.status[i] |= EMIS_ST_ABOVE_CAL;

        if (is_conc(C->unit[i]) && G->ch_flow != EMIS_NONE) {
            R.v[EMIS_B_MASS][i] = v * flow * 1e-6f;           /* mg/Nm3 x Nm3/h -> kg/h */
            R.q[EMIS_Q_MASS][i] = emis_q_worst(q_std, qf);
        } else {
            R.v[EMIS_B_MASS][i] = 0.0f; R.q[EMIS_Q_MASS][i] = PLAT_Q_UNAVAILABLE;
        }

        if (cur_period != NO_PERIOD && operating) {
            if (q == PLAT_Q_VALID) { a->sum += (double)r * dt_ms; a->valid_ms += dt_ms; }
            else a->q_bad = emis_q_worst(a->q_bad, q);
        }
        if (cur_period2 != NO_PERIOD && operating) {
            acc2_t *a2 = &A2[i];
            if (q == PLAT_Q_VALID) { a2->sum += (double)r * dt_ms; a2->valid_ms += dt_ms; }
            else a2->q_bad = emis_q_worst(a2->q_bad, q);
        }
        if (a->valid_ms) { R.v[EMIS_B_RUN][i] = (float)(a->sum / (double)a->valid_ms); R.q[EMIS_Q_RUN][i] = PLAT_Q_VALID; }
        else { R.v[EMIS_B_RUN][i] = 0.0f; R.q[EMIS_Q_RUN][i] = a->q_bad != PLAT_Q_VALID ? a->q_bad : PLAT_Q_UNAVAILABLE; }
    }
}

/* ---------------- retained state ---------------- */

#define SAVE_MAGIC 0x454D3104u         /* "EM1" + layout */
typedef struct {                       /* 32 bytes a channel: what a restart must not lose (the second
                                          period in progress is not kept: it restarts) */
    float    mean;                     /* the period so far: mean of the valid time */
    uint32_t valid_ms;
    float    day_sum;
    uint16_t day_n, day_disc, week_n, week_above, days_invalid, weeks_above, status;
    uint8_t  q_bad, day_q_bad;
    uint16_t n2, n2_above;
} keep_t;
typedef struct {
    uint32_t magic, cfg_crc;
    uint32_t period, day, week, op_ms;
    int32_t  year;
    uint32_t day_op;
    keep_t   k[EMIS_CH_MAX];
} save_t;
_Static_assert(sizeof(keep_t) == 32u, "keep_t layout");
_Static_assert(sizeof(save_t) <= EMIS_SAVE_SIZE, "EMIS_SAVE_SIZE too small");

#define KEPT_ST (EMIS_ST_AVG_INCOMPLETE | EMIS_ST_DAY_INVALID | EMIS_ST_QAL2_REVIEW)

void emis_save(void *buf, uint32_t cfg_crc) {
    save_t *s = buf;                   /* written in place: no copy on the scan task's stack */
    s->magic = SAVE_MAGIC; s->cfg_crc = cfg_crc;
    s->period = cur_period; s->day = cur_day; s->week = cur_week; s->year = cur_year;
    s->op_ms = op_ms; s->day_op = day_op;
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        const acc_t *a = &A[i];
        s->k[i] = (keep_t){ .mean = a->valid_ms ? (float)(a->sum / (double)a->valid_ms) : 0.0f, .valid_ms = a->valid_ms,
                            .day_sum = a->day_sum, .day_n = a->day_n, .day_disc = a->day_disc,
                            .week_n = a->week_n, .week_above = a->week_above, .q_bad = a->q_bad, .day_q_bad = a->day_q_bad,
                            .days_invalid = R.days_invalid[i], .weeks_above = R.weeks_above[i],
                            .status = (uint16_t)(R.status[i] & KEPT_ST), .n2 = R.n2[i], .n2_above = R.n2_above[i] };
    }
}

bool emis_load(const void *buf, uint32_t cfg_crc) {
    const save_t *s = buf;
    if (!C || s->magic != SAVE_MAGIC || s->cfg_crc != cfg_crc) return false;
    cur_period = s->period; cur_day = s->day; cur_week = s->week; cur_year = s->year;
    op_ms = s->op_ms; day_op = s->day_op != 0u;
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        const keep_t *k = &s->k[i]; acc_t *a = &A[i];
        if (C->unit[i] == EMIS_U_OFF) continue;
        a->sum = (double)k->mean * (double)k->valid_ms; a->valid_ms = k->valid_ms;
        a->day_sum = k->day_sum; a->day_n = k->day_n; a->day_disc = k->day_disc;
        a->week_n = k->week_n; a->week_above = k->week_above;
        a->q_bad = k->q_bad < PLAT_Q_COUNT ? k->q_bad : PLAT_Q_FAULT;
        a->day_q_bad = k->day_q_bad < PLAT_Q_COUNT ? k->day_q_bad : PLAT_Q_FAULT;
        R.discarded[i] = k->day_disc;
        R.days_invalid[i] = k->days_invalid; R.weeks_above[i] = k->weeks_above;
        R.status[i] = (uint16_t)(k->status & KEPT_ST);
        R.n2[i] = k->n2; R.n2_above[i] = k->n2_above;
    }
    return true;
}
