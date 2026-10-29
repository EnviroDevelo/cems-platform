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
#define S emis_set
#define SAVE_MS 60000u                 /* the retained copy is refreshed once a minute (and at power fail) */

/* settings (parameters 400-443), one structure so a change is found with one checksum */
emis_set_t emis_set = {
    .t_ref = 273.15f, .p_ref = 101.325f, .ref_pct = 11.0f, .o2_max = 20.0f, .diluent = EMIS_DIL_O2,
    .ci_subtract = true, .clamp_zero = true, .ch_h2o = 11, .ch_t = EMIS_NONE, .ch_p = EMIS_NONE,
    .ch_flow = EMIS_NONE, .p_fixed = 101.325f, .cal_ext = 10.0f, .period_s = 1800u, .min_avail = 67u, .max_discard = 5u,
};
bool     emis_p_operating = true;      /* 422: written by the application or a PLC, not a setting */
uint16_t emis_p_cfg_error;             /* 423 */

/* defaults of the arrays */
#define X4(v)  v, v, v, v
#define X32(v) X4(v), X4(v), X4(v), X4(v), X4(v), X4(v), X4(v), X4(v)
_Static_assert(EMIS_CH_MAX == 32u, "the defaults below are written for 32 channels");
const uint8_t emis_dflt_none[N] = { X32(EMIS_NONE) };

/* retained: the engine's periods in progress */
#if PLAT_FRAM_SIZE >= 131072u
static uint64_t keep[(EMIS_SAVE_SIZE + 7u) / 8u];
static const retain_def_t blocks[] = { RETAIN(PARAM_ID_EMIS_BLOCK, "emis_state", keep, RF_UNIT, NULL) };
const retain_def_t *plat_emis_blocks(size_t *n) { *n = sizeof blocks / sizeof blocks[0]; return blocks; }
#define KEEP 1
#else
#define KEEP 0                         /* the 32 KB development FRAM: no room; a restart loses the period in progress */
#endif

/* the plant settings in force (the engine keeps a pointer; the channels it reads from emis_set.ch in
 * place: a change there is seen by the checksum below before the next step, and restarts the engine) */
static emis_cfg_t    cur_g;
static bool          have_cfg, running, loaded;
static uint32_t      set_crc, last_ms, save_ms;

static void build(emis_cfg_t *g) {
    memset(g, 0, sizeof *g);
    g->t_ref_k = S.t_ref; g->p_ref_kpa = S.p_ref; g->diluent = S.diluent;
    g->ref_pct = S.ref_pct; g->o2_max = S.o2_max; g->only_above = S.only_above;
    g->ch_o2 = S.ch_o2; g->h2o_mode = S.h2o_mode; g->ch_h2o = S.ch_h2o; g->h2o_fixed = S.h2o_fixed;
    g->ch_t = S.ch_t; g->ch_p = S.ch_p; g->t_fixed_c = S.t_fixed; g->p_fixed_kpa = S.p_fixed;
    g->ch_flow = S.ch_flow; g->period_s = S.period_s; g->min_avail = (float)S.min_avail / 100.0f;
    g->max_discard = S.max_discard; g->ci_subtract = S.ci_subtract; g->clamp_zero = S.clamp_zero;
    g->cal_ext_pct = S.cal_ext;
}

void emis_bind_scan(uint32_t now_ms) {
    uint32_t dt = have_cfg ? now_ms - last_ms : 0u;
    last_ms = now_ms;
    if (!S.enable) {
        if (running || !have_cfg) emis_clear();
        running = false; have_cfg = true; emis_p_cfg_error = 0; set_crc = 0u;
        return;
    }

    uint32_t crc = plat_crc32(0u, &S, sizeof S);   /* about 1.6 KB: a few microseconds */
    if (!have_cfg || !running || crc != set_crc) {
        have_cfg = true; set_crc = crc;
        build(&cur_g);
        int e = emis_check(&cur_g, &S.ch, NULL, 0);
        emis_p_cfg_error = (uint16_t)e;
        running = e == 0;
        if (!running) { emis_clear(); return; }
        emis_init(&cur_g, &S.ch);
#if KEEP
        if (!loaded) emis_load(keep, set_crc);     /* after a power cycle: the same configuration carries on */
#endif
        loaded = true;
        save_ms = now_ms;
    }

    emis_in_t in[N];
    size_t nm = plat_meas_count();
    for (unsigned i = 0; i < N; ++i) {
        uint8_t src = S.src[i];
        in[i].range = 0u;
        if (src != EMIS_NONE && src < nm) {
            const plat_meas_def_t *m = plat_meas_at(src);
            float x = plat_meas_value(src);
            in[i].x = x; in[i].q = plat_meas_quality(src);
            if (m->lo < m->hi) {                       /* the measurement's own range is the analyser's */
                if (x >= m->hi) in[i].range = EMIS_IN_OVER;
                else if (x < m->lo) in[i].range = EMIS_IN_BELOW;
            }
        } else { in[i].x = 0.0f; in[i].q = PLAT_Q_UNAVAILABLE; }
    }
    uint32_t clk = 0u;
    if (!plat_clock_now(&clk)) clk = 0u;          /* never set: no averages */
    emis_step(in, emis_p_operating, clk, dt);
#if KEEP
    if (now_ms - save_ms >= SAVE_MS || plat_power_failing()) { emis_save(keep, set_crc); save_ms = now_ms; }
#endif
}

void emis_bind_reset_for_test(void) {
    have_cfg = running = loaded = false;
    last_ms = save_ms = 0u;
#if KEEP
    memset(keep, 0, sizeof keep);
#endif
}
