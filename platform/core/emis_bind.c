/* emis_bind.c - the emissions engine (emis.h) in the platform: settings from parameters 400-442,
 * inputs from the declared measurements (plat_state.h), results to parameters 450-475, the
 * periods in progress kept across a power cycle (retained block 910); the QAL3 charts
 * (emis_qal3.h: parameters 425-429, 444-448, 476-480, retained block 911).
 *
 * Scan task, every scan, after plat_state_scan (the measurement qualities are this scan's). A
 * change to any setting is checked with emis_check; a good one restarts the engine (the periods in
 * progress are forgotten, so an average never mixes two configurations; the change itself is in
 * the event log as a parameter change). A bad one stops it: every result UNAVAILABLE, parameter
 * 423 says which channel. */
#include <math.h>
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
#include "plat_log.h"

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

/* QAL3 */
emis_q3_set_t emis_q3set = { .k = 2.0f, .interval_d = 14u };
uint8_t emis_p_q3_record, emis_p_q3_reset, emis_p_q3_result;
float   emis_p_q3_zero[EMIS_CH_MAX], emis_p_q3_span[EMIS_CH_MAX];

/* defaults of the arrays */
#define X4(v)  v, v, v, v
#define X32(v) X4(v), X4(v), X4(v), X4(v), X4(v), X4(v), X4(v), X4(v)
_Static_assert(EMIS_CH_MAX == 32u, "the defaults below are written for 32 channels");
const uint8_t emis_dflt_none[N] = { X32(EMIS_NONE) };

/* retained: the engine's periods in progress, the QAL3 charts */
#if PLAT_FRAM_SIZE >= 131072u
static uint64_t keep[(EMIS_SAVE_SIZE + 7u) / 8u];
static uint32_t q3keep[(EMIS_Q3_SAVE_SIZE + 3u) / 4u];
static const retain_def_t blocks[] = { RETAIN(PARAM_ID_EMIS_BLOCK, "emis_state", keep, RF_UNIT, NULL),
                                       RETAIN(PARAM_ID_EMIS_Q3_BLOCK, "emis_qal3", q3keep, RF_UNIT, NULL) };
const retain_def_t *plat_emis_blocks(size_t *n) { *n = sizeof blocks / sizeof blocks[0]; return blocks; }
#define KEEP 1
#else
#define KEEP 0                         /* the 32 KB development FRAM: no room; a restart loses the period in progress */
#endif

/* Instrument points (the instrument poller, inst.c, provides the strong one): false = no such point. */
__attribute__((weak)) bool emis_inst_point(unsigned p, float *value, uint8_t *q) {
    (void)p; (void)value; (void)q;
    return false;
}

/* the plant settings in force (the engine keeps a pointer; the channels it reads from emis_set.ch in
 * place: a change there is seen by the checksum below before the next step, and restarts the engine) */
static emis_cfg_t    cur_g;
static bool          have_cfg, running, loaded, q3_loaded;
static uint32_t      set_crc, last_ms, save_ms;

static void build(emis_cfg_t *g) {
    memset(g, 0, sizeof *g);
    g->t_ref_k = S.t_ref; g->p_ref_kpa = S.p_ref; g->diluent = S.diluent;
    g->ref_pct = S.ref_pct; g->o2_max = S.o2_max; g->only_above = S.only_above;
    g->ch_o2 = S.ch_o2; g->h2o_mode = S.h2o_mode; g->ch_h2o = S.ch_h2o; g->h2o_fixed = S.h2o_fixed;
    g->ch_t = S.ch_t; g->ch_p = S.ch_p; g->t_fixed_c = S.t_fixed; g->p_fixed_kpa = S.p_fixed;
    g->ch_flow = S.ch_flow; g->period_s = S.period_s; g->min_avail = (float)S.min_avail / 100.0f;
    g->max_discard = S.max_discard; g->ci_subtract = S.ci_subtract; g->clamp_zero = S.clamp_zero;
    g->cal_ext_pct = S.cal_ext; g->period2_s = S.period2_s;
}

/* QAL3: the check entries and resets (parameters 427, 428), every scan whether or not the engine runs */
static void qal3_scan(uint32_t clk) {
    bool changed = false;
    if (!q3_loaded) {
        q3_loaded = true;
        emis_qal3_init();
#if KEEP
        emis_qal3_load(q3keep);
#endif
    }
    if (emis_p_q3_record) {
        unsigned ch = emis_p_q3_record - 1u;
        emis_p_q3_record = 0u;
        int r = emis_qal3_record(&emis_q3set, ch, ch < EMIS_CH_MAX ? emis_p_q3_zero[ch] : NAN,
                                 ch < EMIS_CH_MAX ? emis_p_q3_span[ch] : NAN, clk);
        emis_p_q3_result = (uint8_t)(r < 0 ? -r : 0);
        if (r == 0) {
            uint8_t st = emis_q3.status[ch];
            const char *t = (st & (EMIS_Q3_ZERO_OUT | EMIS_Q3_SPAN_OUT)) ? "out of control" :
                            (st & (EMIS_Q3_ZERO_WARN | EMIS_Q3_SPAN_WARN)) ? "beyond action limit" : "in control";
            plat_log_event(LOG_EV_QAL3, PLAT_SRC_INTERNAL, 0, ch, emis_q3.zero_dev[ch], emis_q3.span_dev[ch], t);
            changed = true;
        }
    }
    if (emis_p_q3_reset) {
        unsigned ch = emis_p_q3_reset - 1u;
        emis_p_q3_reset = 0u;
        if (ch < EMIS_CH_MAX) {
            emis_qal3_reset(ch);
            plat_log_event(LOG_EV_QAL3, PLAT_SRC_INTERNAL, 0, ch, NAN, NAN, "chart reset");
            changed = true;
        }
    }
    uint8_t before[EMIS_CH_MAX];
    memcpy(before, emis_q3.status, sizeof before);
    emis_qal3_tick(&emis_q3set, clk);              /* a settings change starts a chart again; DUE */
    if (memcmp(before, emis_q3.status, sizeof before)) changed = true;
#if KEEP
    if (changed) emis_qal3_save(q3keep);
#else
    (void)changed;
#endif
}

void emis_bind_scan(uint32_t now_ms) {
    uint32_t dt = have_cfg ? now_ms - last_ms : 0u;
    last_ms = now_ms;
    uint32_t clk = 0u;
    if (!plat_clock_now(&clk)) clk = 0u;          /* never set: no averages */
    qal3_scan(clk);
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
        float ix; uint8_t iq;
        if (src >= EMIS_SRC_INST && src != EMIS_NONE) {   /* an instrument point read over Modbus (inst.h) */
            if (emis_inst_point((unsigned)(src - EMIS_SRC_INST), &ix, &iq)) { in[i].x = ix; in[i].q = iq; }
            else { in[i].x = 0.0f; in[i].q = PLAT_Q_UNAVAILABLE; }
        } else if (src < nm) {
            const plat_meas_def_t *m = plat_meas_at(src);
            float x = plat_meas_value(src);
            in[i].x = x; in[i].q = plat_meas_quality(src);
            if (m->lo < m->hi) {                       /* the measurement's own range is the analyser's */
                if (x >= m->hi) in[i].range = EMIS_IN_OVER;
                else if (x < m->lo) in[i].range = EMIS_IN_BELOW;
            }
        } else { in[i].x = 0.0f; in[i].q = PLAT_Q_UNAVAILABLE; }
    }
    emis_step(in, emis_p_operating, clk, dt);
#if KEEP
    if (now_ms - save_ms >= SAVE_MS || plat_power_failing()) { emis_save(keep, set_crc); save_ms = now_ms; }
#endif
}

void emis_bind_reset_for_test(void) {
    have_cfg = running = loaded = q3_loaded = false;
    last_ms = save_ms = 0u;
    emis_p_q3_record = emis_p_q3_reset = emis_p_q3_result = 0u;
#if KEEP
    memset(keep, 0, sizeof keep);
    memset(q3keep, 0, sizeof q3keep);
#endif
}
