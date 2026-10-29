/* emis_bind.c - the emissions engine (emis.h) in the platform: settings from parameters 400-443,
 * inputs from the declared measurements (plat_state.h), results to parameters 450-469, the
 * periods in progress kept across a power cycle (retained block 910).
 *
 * Scan task, every scan, after plat_state_scan (the measurement qualities are this scan's). A
 * change to any setting is checked with emis_check; a good one restarts the engine (the periods in
 * progress are forgotten, so an average never mixes two configurations; the change itself is in
 * the event log as a parameter change). A bad one stops it: every result UNAVAILABLE, parameter
 * 423 says which channel. */
#include <string.h>
#include "emis.h"
#include "emis_bind.h"
#include "param.h"
#include "param_store.h"
#include "plat_state.h"
#include "plat_clock.h"
#include "plat_power.h"
#include "nvm.h"
#include "plat_config.h"

#define N EMIS_CH_MAX
#define SAVE_MS 60000u                 /* the retained copy is refreshed once a minute (and at power fail) */

/* settings (parameters 400-443) */
bool     emis_p_enable;
float    emis_p_t_ref = 273.15f, emis_p_p_ref = 101.325f, emis_p_ref_pct = 11.0f, emis_p_o2_max = 20.0f;
uint8_t  emis_p_diluent = EMIS_DIL_O2;
bool     emis_p_only_above, emis_p_ci_subtract = true, emis_p_clamp_zero = true, emis_p_operating = true;
uint8_t  emis_p_ch_o2, emis_p_h2o_mode, emis_p_ch_h2o = 11, emis_p_ch_t = EMIS_NONE, emis_p_ch_p = EMIS_NONE,
         emis_p_ch_flow = EMIS_NONE;
float    emis_p_h2o_fixed, emis_p_t_fixed, emis_p_p_fixed = 101.325f, emis_p_cal_ext = 10.0f;
uint16_t emis_p_period_s = 1800u;
uint8_t  emis_p_min_avail = 67u, emis_p_max_discard = 5u;
uint16_t emis_p_cfg_error;
uint8_t  emis_p_src[N], emis_p_unit[N], emis_p_add[N];
bool     emis_p_wet[N], emis_p_ref[N];
float    emis_p_mm[N], emis_p_factor[N], emis_p_a[N], emis_p_b[N], emis_p_ys_max[N], emis_p_elv[N], emis_p_ci[N],
         emis_p_lo[N], emis_p_hi[N];
/* results (450-469) */
float    emis_r_bank[EMIS_BANKS][N];
uint8_t  emis_r_q[6][N];               /* in, calc, mass, avg, day, run */
uint16_t emis_r_status[N], emis_r_disc[N], emis_r_days[N], emis_r_weeks[N];

/* defaults of the arrays */
#define X4(v)  v, v, v, v
#define X32(v) X4(v), X4(v), X4(v), X4(v), X4(v), X4(v), X4(v), X4(v)
_Static_assert(EMIS_CH_MAX == 32u, "the defaults below are written for 32 channels");
const uint8_t emis_dflt_none[N] = { X32(EMIS_NONE) };
const float   emis_dflt_one[N]  = { X32(1.0f) };

/* retained: the engine's periods in progress */
#if PLAT_FRAM_SIZE >= 131072u
static uint8_t keep[EMIS_SAVE_SIZE];
static const retain_def_t blocks[] = { RETAIN(PARAM_ID_EMIS_BLOCK, "emis_state", keep, RF_UNIT, NULL) };
const retain_def_t *plat_emis_blocks(size_t *n) { *n = sizeof blocks / sizeof blocks[0]; return blocks; }
#define KEEP 1
#else
#define KEEP 0                         /* the 32 KB development FRAM: no room; a restart loses the period in progress */
#endif

static emis_cfg_t    cur_g;
static emis_ch_cfg_t cur_c[N];
static bool          have_cfg, running, loaded;
static uint32_t      cfg_crc, last_ms, save_ms;

static void build(emis_cfg_t *g, emis_ch_cfg_t *c) {
    memset(g, 0, sizeof *g);
    g->t_ref_k = emis_p_t_ref; g->p_ref_kpa = emis_p_p_ref; g->diluent = emis_p_diluent;
    g->ref_pct = emis_p_ref_pct; g->o2_max = emis_p_o2_max; g->only_above = emis_p_only_above;
    g->ch_o2 = emis_p_ch_o2; g->h2o_mode = emis_p_h2o_mode; g->ch_h2o = emis_p_ch_h2o; g->h2o_fixed = emis_p_h2o_fixed;
    g->ch_t = emis_p_ch_t; g->ch_p = emis_p_ch_p; g->t_fixed_c = emis_p_t_fixed; g->p_fixed_kpa = emis_p_p_fixed;
    g->ch_flow = emis_p_ch_flow; g->period_s = emis_p_period_s; g->min_avail = (float)emis_p_min_avail / 100.0f;
    g->max_discard = emis_p_max_discard; g->ci_subtract = emis_p_ci_subtract; g->clamp_zero = emis_p_clamp_zero;
    g->cal_ext_pct = emis_p_cal_ext;
    for (unsigned i = 0; i < N; ++i) {
        emis_ch_cfg_t *x = &c[i];
        memset(x, 0, sizeof *x);
        x->unit = emis_p_src[i] == EMIS_NONE ? (uint8_t)EMIS_U_OFF : emis_p_unit[i];
        x->wet = emis_p_wet[i]; x->ref = emis_p_ref[i]; x->add = emis_p_add[i];
        x->molar_mass = emis_p_mm[i]; x->factor = emis_p_factor[i]; x->qal2_a = emis_p_a[i]; x->qal2_b = emis_p_b[i];
        x->ys_max = emis_p_ys_max[i]; x->elv = emis_p_elv[i]; x->ci_pct = emis_p_ci[i];
        x->range_lo = emis_p_lo[i]; x->range_hi = emis_p_hi[i];
    }
}

static void publish_off(void) {
    memset(emis_r_bank, 0, sizeof emis_r_bank);
    memset(emis_r_q, PLAT_Q_UNAVAILABLE, sizeof emis_r_q);
    memset(emis_r_status, 0, sizeof emis_r_status);
}

static void publish(void) {
    for (unsigned i = 0; i < N; ++i) {
        const emis_out_t *o = emis_out(i);
        for (unsigned b = 0; b < EMIS_BANKS; ++b) emis_r_bank[b][i] = o->v[b];
        emis_r_q[0][i] = o->q_in; emis_r_q[1][i] = o->q_calc; emis_r_q[2][i] = o->q_mass;
        emis_r_q[3][i] = o->q_avg; emis_r_q[4][i] = o->q_day; emis_r_q[5][i] = o->q_run;
        emis_r_status[i] = o->status; emis_r_disc[i] = o->discarded_today;
        emis_r_days[i] = o->days_invalid; emis_r_weeks[i] = o->weeks_above;
    }
}

void emis_bind_scan(uint32_t now_ms) {
    uint32_t dt = have_cfg ? now_ms - last_ms : 0u;
    last_ms = now_ms;
    if (!emis_p_enable) { running = false; have_cfg = true; emis_p_cfg_error = 0; publish_off(); return; }

    emis_cfg_t g; static emis_ch_cfg_t c[N];
    build(&g, c);
    if (!have_cfg || !running || memcmp(&g, &cur_g, sizeof g) || memcmp(c, cur_c, sizeof c)) {
        bool first = !loaded;
        have_cfg = true;
        cur_g = g; memcpy(cur_c, c, sizeof c);
        int e = emis_check(&g, c, NULL, 0);
        emis_p_cfg_error = (uint16_t)e;
        running = e == 0;
        if (!running) { publish_off(); return; }
        cfg_crc = plat_crc32(0u, &g, sizeof g);
        cfg_crc = plat_crc32(cfg_crc, c, sizeof c);
        emis_init(&g, c);
#if KEEP
        if (first) emis_load(keep, cfg_crc);    /* after a power cycle: the same configuration carries on */
#else
        (void)first;
#endif
        loaded = true;
        save_ms = now_ms;
    }

    emis_in_t in[N];
    size_t nm = plat_meas_count();
    for (unsigned i = 0; i < N; ++i) {
        uint8_t s = emis_p_src[i];
        if (s != EMIS_NONE && s < nm) { in[i].x = plat_meas_value(s); in[i].q = plat_meas_quality(s); }
        else { in[i].x = 0.0f; in[i].q = PLAT_Q_UNAVAILABLE; }
    }
    uint32_t clk = 0u;
    if (!plat_clock_now(&clk)) clk = 0u;          /* never set: no averages */
    emis_step(in, emis_p_operating, clk, dt);
    publish();
#if KEEP
    if (now_ms - save_ms >= SAVE_MS || plat_power_failing()) { emis_save(keep, cfg_crc); save_ms = now_ms; }
#endif
}

void emis_bind_reset_for_test(void) {
    have_cfg = running = loaded = false;
    last_ms = save_ms = 0u;
#if KEEP
    memset(keep, 0, sizeof keep);
#endif
}
