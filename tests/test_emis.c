/* test_emis.c - the emissions calculation engine (emis.h, docs/EMISSIONS_MATH.md): the formulas
 * against hand-worked vectors (section 6 of the design), the full chain for a hot-wet NOx
 * analyser with a zirconia O2 probe, quality propagation, the O2 limit and "correct only above",
 * the CO2 reference, the short-term average 2/3 rule, confidence interval subtraction, daily
 * validity (5 discards), periods outside operating time, the clock, the valid calibration range,
 * and the configuration check. */
#include <math.h>
#include <string.h>
#include "tst.h"
#include "emis.h"
#include "plat_state.h"
#include "emis_bind.h"
#include "param.h"
#include "bsp_host.h"

#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

/* 2026-10-29 00:00:00 local, a Thursday */
#define T0 1793232000u

static emis_cfg_t G;
static emis_chs_t C;
static emis_in_t IN[EMIS_CH_MAX];

static void base(void) {
    emis_cfg_default(&G);
    emis_chs_default(&C);
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) { IN[i].x = 0.0f; IN[i].q = PLAT_Q_VALID; IN[i].range = 0u; }
}

/* Vectors 1-8: the formulas on their own */
static void t_formulas(void) {
    NEAR(emis_molar_volume(273.15f, 101.325f), 22.41397, 1e-4);
    NEAR(emis_ppm_to_mg(100.0f, 64.066f, 273.15f, 101.325f), 285.8307, 1e-3);   /* SO2 */
    NEAR(emis_ppm_to_mg(100.0f, 46.0055f, 273.15f, 101.325f), 205.2537, 1e-3);  /* NO as NO2 */
    NEAR(emis_ppm_to_mg(50.0f, 28.010f, 273.15f, 101.325f), 62.4834, 1e-3);     /* CO */
    NEAR(emis_ppm_to_mg(30.0f, 12.011f, 273.15f, 101.325f), 16.0761, 1e-3);     /* TOC: 10 ppm propane x 3, as C */
    NEAR(emis_mg_to_ppm(285.8307f, 64.066f, 273.15f, 101.325f), 100.0, 1e-3);
    NEAR(emis_o2_ref(100.0f, 7.0f, 11.0f), 71.4286, 1e-3);
    NEAR(emis_o2_ref(100.0f, 11.0f, 11.0f), 100.0, 1e-4);
    NEAR(emis_to_dry(100.0f, 15.0f), 117.6471, 1e-3);
    NEAR(emis_norm_conc(50.0f, 423.15f, 100.0f, 273.15f, 101.325f), 78.4838, 1e-3);
    NEAR(emis_co2_ref(100.0f, 8.0f, 12.0f), 150.0, 1e-4);
    /* worst-of: fault beats everything, a missing input beats a mode, a mode beats range */
    CHECK_EQ(emis_q_worst(PLAT_Q_VALID, PLAT_Q_RANGE), PLAT_Q_RANGE);
    CHECK_EQ(emis_q_worst(PLAT_Q_MAINT, PLAT_Q_RANGE), PLAT_Q_MAINT);
    CHECK_EQ(emis_q_worst(PLAT_Q_CAL, PLAT_Q_STALE), PLAT_Q_STALE);
    CHECK_EQ(emis_q_worst(PLAT_Q_UNAVAILABLE, PLAT_Q_FAULT), PLAT_Q_FAULT);
    CHECK_EQ(emis_q_worst(PLAT_Q_VALID, 99), PLAT_Q_FAULT);
}

/* the hot-wet chain: channel 1 O2 wet (zirconia), 4 NO, 5 NO2, 6 NOx = NO + NO2 as NO2,
 * 11 H2O, 18 flow, 20 T, 21 p */
static void chain_cfg(void) {
    base();
    G.ch_o2 = 1; G.h2o_mode = EMIS_H2O_MEASURED; G.ch_h2o = 11; G.ch_t = 20; G.ch_p = 21; G.ch_flow = 18;
    C.unit[1] = EMIS_U_PCT; C.wet[1] = 1;
    C.unit[4] = EMIS_U_PPM; C.wet[4] = 1; C.molar_mass[4] = 30.006f; C.ref[4] = 1;
    C.unit[5] = EMIS_U_PPM; C.wet[5] = 1; C.molar_mass[5] = 46.0055f; C.ref[5] = 1;
    C.unit[6] = EMIS_U_PPM; C.wet[6] = 1; C.molar_mass[6] = 46.0055f; C.ref[6] = 1; C.add[6] = 5;
    C.qal2_a[6] = 2.0f; C.qal2_b[6] = 1.05f; C.elv[6] = 400.0f; C.ci_pct[6] = 20.0f; C.ys_max[6] = 300.0f;
    C.unit[11] = EMIS_U_PCT;
    C.unit[18] = EMIS_U_FLOW_ACT; C.wet[18] = 1;
    C.unit[20] = EMIS_U_OTHER; C.unit[21] = EMIS_U_OTHER;
    IN[1].x = 8.0f; IN[4].x = 150.0f; IN[5].x = 10.0f; IN[6].x = 150.0f; IN[11].x = 12.0f;
    IN[18].x = 100000.0f; IN[20].x = 150.0f; IN[21].x = 100.0f;
}

/* Vector 9: the whole chain */
static void t_chain(void) {
    char why[96];
    chain_cfg();
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), 0);
    emis_init(&G, &C);
    emis_step(IN, true, T0 + 10u, 1000u);
    NEAR(emis_out(1).v[EMIS_B_STD], 9.090909, 1e-4);             /* 8 % wet at 12 % water */
    NEAR(emis_out(6).v[EMIS_B_RAW], 160.0, 1e-4);               /* NO + NO2 */
    NEAR(emis_out(6).v[EMIS_B_STD], 396.5128, 2e-2);            /* (2 + 1.05 x 160) ppm -> mg/Nm3 -> dry */
    NEAR(emis_out(6).v[EMIS_B_REF], 332.9497, 2e-2);            /* to 11 % O2 */
    NEAR(emis_out(18).v[EMIS_B_STD], 56062.56, 1.0);               /* Nm3/h dry */
    NEAR(emis_out(6).v[EMIS_B_MASS], 22.2295, 2e-3);            /* kg/h, from the uncorrected concentration */
    CHECK_EQ(emis_out(6).q_calc, PLAT_Q_VALID);
    CHECK_EQ(emis_out(6).q_mass, PLAT_Q_VALID);
    CHECK(!(emis_out(6).status & EMIS_ST_NO_QAL2));
    CHECK(emis_out(4).status & EMIS_ST_NO_QAL2);
    CHECK(!(emis_out(6).status & EMIS_ST_ABOVE_CAL));           /* 333 < 300 + 10 % of 400 */
    CHECK_EQ(emis_out(0).q_calc, PLAT_Q_UNAVAILABLE);   /* not used */

    /* quality: the water reading in maintenance makes every wet-corrected value maintenance */
    IN[11].q = PLAT_Q_MAINT;
    emis_step(IN, true, T0 + 11u, 1000u);
    CHECK_EQ(emis_out(6).q_calc, PLAT_Q_MAINT);
    CHECK_EQ(emis_out(6).q_in, PLAT_Q_VALID);
    CHECK_EQ(emis_out(20).q_calc, PLAT_Q_VALID);        /* the temperature does not depend on it */
    IN[11].q = PLAT_Q_VALID;
    /* the NO2 analyser stale: NOx (which adds it) is stale, NO is not */
    IN[5].q = PLAT_Q_STALE;
    emis_step(IN, true, T0 + 12u, 1000u);
    CHECK_EQ(emis_out(6).q_in, PLAT_Q_STALE);
    CHECK_EQ(emis_out(4).q_calc, PLAT_Q_VALID);
    IN[5].q = PLAT_Q_VALID;
    /* flow faulted: mass is FAULT, the concentration is not */
    IN[18].q = PLAT_Q_FAULT;
    emis_step(IN, true, T0 + 13u, 1000u);
    CHECK_EQ(emis_out(6).q_mass, PLAT_Q_FAULT);
    CHECK_EQ(emis_out(6).q_calc, PLAT_Q_VALID);
    IN[18].q = PLAT_Q_VALID;
    /* a water content beyond belief */
    IN[11].x = 95.0f;
    emis_step(IN, true, T0 + 14u, 1000u);
    CHECK_EQ(emis_out(6).q_calc, PLAT_Q_RANGE);
    IN[11].x = 12.0f;
    /* analyser over its range */
    IN[6].x = 495.0f; IN[6].range = EMIS_IN_OVER;
    emis_step(IN, true, T0 + 15u, 1000u);
    CHECK(emis_out(6).status & EMIS_ST_OVER_RANGE);
    CHECK_EQ(emis_out(6).q_in, PLAT_Q_RANGE);
    IN[6].range = 0u; IN[6].x = NAN;
    emis_step(IN, true, T0 + 16u, 1000u);
    CHECK_EQ(emis_out(6).q_in, PLAT_Q_RANGE);
    CHECK(isfinite(emis_out(6).v[EMIS_B_REF]));
    /* fixed water content instead of the analyser */
    chain_cfg(); G.h2o_mode = EMIS_H2O_FIXED; G.h2o_fixed = 12.0f; IN[11].q = PLAT_Q_FAULT;
    emis_init(&G, &C);
    emis_step(IN, true, T0 + 17u, 1000u);
    NEAR(emis_out(6).v[EMIS_B_REF], 332.9497, 2e-2);
    CHECK_EQ(emis_out(6).q_calc, PLAT_Q_VALID);
    CHECK(emis_out(6).status & EMIS_ST_FIXED_H2O);
}

/* O2 near 21 %, "only above", CO2 reference */
static void t_diluent(void) {
    base();
    C.unit[0] = EMIS_U_PCT;
    C.unit[3] = EMIS_U_MG_STD; C.ref[3] = 1;
    IN[3].x = 100.0f;
    emis_init(&G, &C);
    IN[0].x = 20.6f;                                     /* air: start-up, purge */
    emis_step(IN, true, T0, 1000u);
    CHECK(emis_out(3).status & EMIS_ST_O2_HIGH);
    CHECK_EQ(emis_out(3).q_calc, PLAT_Q_RANGE);
    NEAR(emis_out(3).v[EMIS_B_REF], 1000.0, 1e-2);               /* corrected with 20 %: flagged, not used */
    IN[0].x = 7.0f;
    emis_step(IN, true, T0 + 1u, 1000u);
    NEAR(emis_out(3).v[EMIS_B_REF], 71.4286, 1e-3);              /* IED Annex VI Part 7 */
    G.only_above = 1; emis_init(&G, &C);
    emis_step(IN, true, T0 + 2u, 1000u);
    NEAR(emis_out(3).v[EMIS_B_REF], 100.0, 1e-4);                /* Annex VI Pt 6 2.7: 7 % is below 11 %, no correction */
    IN[0].x = 14.0f;
    emis_step(IN, true, T0 + 3u, 1000u);
    NEAR(emis_out(3).v[EMIS_B_REF], 142.857, 1e-2);
    /* NSW Appendix 1 eq 1.6: CO2 reference 12 % */
    base();
    G.diluent = EMIS_DIL_CO2; G.ref_pct = 12.0f; G.ch_o2 = 2;
    C.unit[2] = EMIS_U_PCT; C.unit[3] = EMIS_U_MG_STD; C.ref[3] = 1;
    IN[2].x = 8.0f; IN[3].x = 100.0f;
    CHECK_EQ(emis_check(&G, &C, NULL, 0), 0);
    emis_init(&G, &C);
    emis_step(IN, true, T0, 1000u);
    NEAR(emis_out(3).v[EMIS_B_REF], 150.0, 1e-3);
    IN[2].x = 0.1f;
    emis_step(IN, true, T0 + 1u, 1000u);
    CHECK_EQ(emis_out(3).q_calc, PLAT_Q_RANGE);
}

/* run minutes [m0, m1) of the day with the CO reading v and quality q; one step a minute */
static void run(unsigned m0, unsigned m1, float v, uint8_t q, bool op) {
    IN[3].x = v; IN[3].q = q;
    for (unsigned m = m0; m < m1; ++m) emis_step(IN, op, T0 + m * 60u, 60000u);
}

static void avg_cfg(void) {
    base();
    G.diluent = EMIS_DIL_NONE;
    C.unit[3] = EMIS_U_MG_STD; C.elv[3] = 50.0f; C.ci_pct[3] = 10.0f;   /* CO, daily ELV 50, CI 10 % = 5 */
    emis_init(&G, &C);
}

/* Vectors 10-13: short-term averages */
static void t_short(void) {
    avg_cfg();
    run(0, 30, 40.0f, PLAT_Q_VALID, true);
    CHECK_EQ(emis_out(3).q_run, PLAT_Q_VALID);
    NEAR(emis_out(3).v[EMIS_B_RUN], 40.0, 1e-4);
    run(30, 31, 40.0f, PLAT_Q_VALID, true);              /* minute 30 closes 00:00-00:30 */
    CHECK_EQ(emis_out(3).q_avg, PLAT_Q_VALID);
    NEAR(emis_out(3).v[EMIS_B_AVG], 40.0, 1e-4);
    NEAR(emis_out(3).v[EMIS_B_VAL], 35.0, 1e-4);                 /* IED Annex VI Part 8 1.2: minus 10 % of 50 */
    /* 00:30-01:00: 20 min valid at 30, 10 min calibration: 2/3 valid, kept */
    run(31, 50, 30.0f, PLAT_Q_VALID, true);
    run(50, 60, 99.0f, PLAT_Q_CAL, true);
    run(60, 61, 30.0f, PLAT_Q_VALID, true);
    CHECK_EQ(emis_out(3).q_avg, PLAT_Q_VALID);
    NEAR(emis_out(3).v[EMIS_B_AVG], (40.0 + 19 * 30.0) / 20.0, 1e-3);  /* minute 30 was still 40 */
    /* 01:00-01:30: 19 min valid, 11 maintenance: discarded */
    run(61, 79, 30.0f, PLAT_Q_VALID, true);
    run(79, 90, 30.0f, PLAT_Q_MAINT, true);
    run(90, 91, 30.0f, PLAT_Q_VALID, true);
    CHECK_EQ(emis_out(3).q_avg, PLAT_Q_MAINT);
    CHECK(emis_out(3).status & EMIS_ST_AVG_INCOMPLETE);
    CHECK_EQ(emis_out(3).discarded_today, 1);
    /* 01:30-02:00: plant off for 20 min, valid for the 10 it ran: kept (2/3 of the operating time) */
    run(91, 110, 0.0f, PLAT_Q_FAULT, false);
    run(110, 120, 20.0f, PLAT_Q_VALID, true);
    run(120, 121, 20.0f, PLAT_Q_VALID, false);
    CHECK_EQ(emis_out(3).q_avg, PLAT_Q_VALID);
    NEAR(emis_out(3).v[EMIS_B_AVG], (30.0 + 10 * 20.0) / 11.0, 1e-3);
    CHECK(!(emis_out(3).status & EMIS_ST_AVG_INCOMPLETE));
    /* 02:00-02:30 plant off throughout: no average, not discarded */
    run(121, 151, 0.0f, PLAT_Q_UNAVAILABLE, false);
    CHECK_EQ(emis_out(3).q_avg, PLAT_Q_UNAVAILABLE);
    CHECK_EQ(emis_out(3).discarded_today, 1);
    /* a validated value below zero is reported as zero; with clamp off it is kept */
    run(151, 181, 2.0f, PLAT_Q_VALID, true);
    NEAR(emis_out(3).v[EMIS_B_VAL], 0.0, 1e-6);
    G.clamp_zero = 0; avg_cfg(); G.clamp_zero = 0; emis_init(&G, &C);
    run(0, 31, 2.0f, PLAT_Q_VALID, true);
    NEAR(emis_out(3).v[EMIS_B_VAL], -3.0, 1e-4);
    /* NSW: no subtraction */
    avg_cfg(); G.ci_subtract = 0; emis_init(&G, &C);
    run(0, 31, 40.0f, PLAT_Q_VALID, true);
    NEAR(emis_out(3).v[EMIS_B_VAL], 40.0, 1e-4);
    CHECK(emis_out(3).status & EMIS_ST_CI_OFF);
}

/* Vector 14: daily validity, IED Annex VI Part 8 1.2 */
static void t_day(void) {
    avg_cfg();
    /* day 1: 5 half-hours in maintenance, the rest at 40: valid, mean of 43 validated = 35 */
    run(0, 150, 40.0f, PLAT_Q_MAINT, true);
    run(150, 1440, 40.0f, PLAT_Q_VALID, true);
    CHECK_EQ(emis_out(3).discarded_today, 5);
    run(1440, 1441, 40.0f, PLAT_Q_VALID, true);
    CHECK_EQ(emis_out(3).q_day, PLAT_Q_VALID);
    NEAR(emis_out(3).v[EMIS_B_DAY], 35.0, 1e-3);
    CHECK_EQ(emis_out(3).discarded_today, 0);
    CHECK_EQ(emis_out(3).days_invalid, 0);
    /* day 2: 6 discarded: invalid, counted */
    run(1441, 1620, 40.0f, PLAT_Q_FAULT, true);
    run(1620, 2880, 40.0f, PLAT_Q_VALID, true);
    run(2880, 2881, 40.0f, PLAT_Q_VALID, false);
    CHECK_EQ(emis_out(3).q_day, PLAT_Q_FAULT);
    CHECK(emis_out(3).status & EMIS_ST_DAY_INVALID);
    CHECK_EQ(emis_out(3).days_invalid, 1);
    /* day 3: plant off all day: no daily value, not invalid */
    run(2881, 4320, 0.0f, PLAT_Q_VALID, false);
    run(4320, 4321, 0.0f, PLAT_Q_VALID, false);
    CHECK_EQ(emis_out(3).q_day, PLAT_Q_UNAVAILABLE);
    CHECK(!(emis_out(3).status & EMIS_ST_DAY_INVALID));
    CHECK_EQ(emis_out(3).days_invalid, 1);
    /* LCP (Annex V Part 3 10): hourly, more than 3 invalid hours: invalid */
    avg_cfg(); G.period_s = 3600u; G.max_discard = 3u; emis_init(&G, &C);
    run(0, 240, 40.0f, PLAT_Q_STALE, true);
    run(240, 1441, 40.0f, PLAT_Q_VALID, true);
    CHECK_EQ(emis_out(3).q_day, PLAT_Q_STALE);
    CHECK_EQ(emis_out(3).days_invalid, 1);
}

/* the clock: not set, a step back, the new year */
static void t_clock(void) {
    avg_cfg();
    IN[3].x = 40.0f; IN[3].q = PLAT_Q_VALID;
    for (int i = 0; i < 100; ++i) emis_step(IN, true, 0u, 60000u);
    CHECK_EQ(emis_out(3).q_run, PLAT_Q_UNAVAILABLE);
    NEAR(emis_out(3).v[EMIS_B_STD], 40.0, 1e-4);                 /* live values still work */
    run(0, 10, 40.0f, PLAT_Q_VALID, true);
    emis_step(IN, true, T0 - 3600u, 60000u);             /* clock set back an hour: the period ends where it is */
    CHECK_EQ(emis_out(3).q_avg, PLAT_Q_VALID);
    NEAR(emis_out(3).v[EMIS_B_AVG], 40.0, 1e-4);
    /* days invalid go back to zero in a new year */
    avg_cfg();
    uint32_t nye = 1798675200u;                          /* 2026-12-31 00:00 */
    IN[3].x = 40.0f; IN[3].q = PLAT_Q_FAULT;
    for (uint32_t m = 0; m < 1440u; ++m) emis_step(IN, true, nye + m * 60u, 60000u);
    for (uint32_t m = 1440u; m < 1500u; ++m) emis_step(IN, true, nye + m * 60u, 60000u);
    CHECK_EQ(emis_out(3).days_invalid, 0);
    CHECK_EQ(emis_out(3).q_day, PLAT_Q_FAULT);                   /* the last day of 2026 was invalid */
}

/* Vector 15: valid calibration range, EN 14181 6.5 */
static void t_cal_range(void) {
    avg_cfg();
    C.ys_max[3] = 30.0f; emis_init(&G, &C);               /* range: 0 .. 30 + 10 % of 50 = 35 */
    run(0, 1, 36.0f, PLAT_Q_VALID, true);
    CHECK(emis_out(3).status & EMIS_ST_ABOVE_CAL);
    run(1, 2, 34.0f, PLAT_Q_VALID, true);
    CHECK(!(emis_out(3).status & EMIS_ST_ABOVE_CAL));
    /* a week with half the averages above: a new QAL2 is due */
    for (unsigned d = 0; d < 7u; ++d)
        for (unsigned m = 0; m < 1440u; ++m) {
            IN[3].x = (m / 30u) % 2u ? 40.0f : 20.0f;
            emis_step(IN, true, T0 + 4u * 86400u + d * 86400u + m * 60u, 60000u);   /* Monday 2 Nov onward */
        }
    emis_step(IN, true, T0 + 11u * 86400u, 60000u);
    CHECK(emis_out(3).status & EMIS_ST_QAL2_REVIEW);
}

static void t_check(void) {
    char why[96] = "";
    base();
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), (int)EMIS_CH_MAX + 1);   /* O2 reference but no O2 channel */
    C.unit[0] = EMIS_U_PCT;
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), 0);
    C.unit[3] = EMIS_U_PPM;
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), 4);     /* no molar mass */
    CHECK(strstr(why, "molar mass") != NULL);
    C.molar_mass[3] = 28.01f; C.wet[3] = 1;
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), 4);     /* wet but no water content */
    C.wet[3] = 0; C.add[3] = 3;
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), 4);
    C.add[3] = EMIS_NONE; C.unit[20] = EMIS_U_OTHER; C.ref[20] = 1;
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), 21);    /* a temperature is not O2-corrected */
    C.ref[20] = 0;
    G.period_s = 700u;
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), (int)EMIS_CH_MAX + 1);
    G.period_s = 600u; G.ref_pct = 21.0f;
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), (int)EMIS_CH_MAX + 1);
    G.ref_pct = 3.0f; C.qal2_b[3] = NAN;
    CHECK_EQ(emis_check(&G, &C, why, sizeof why), 4);
}

/* power cycle: the period in progress carries on under the same configuration, not under another */
static void t_retain(void) {
    static uint8_t buf[EMIS_SAVE_SIZE];
    avg_cfg();
    run(0, 20, 40.0f, PLAT_Q_VALID, true);
    emis_save(buf, 0x1234u);
    emis_init(&G, &C);                                    /* the restart */
    CHECK(!emis_load(buf, 0x9999u));                     /* saved under another configuration */
    CHECK(emis_load(buf, 0x1234u));
    run(25, 31, 40.0f, PLAT_Q_VALID, true);              /* 5 min off while the power was out */
    CHECK_EQ(emis_out(3).q_avg, PLAT_Q_VALID);                   /* 25 of 25 operating minutes valid */
    NEAR(emis_out(3).v[EMIS_B_AVG], 40.0, 1e-4);
}

/* the platform binding: parameters in, parameters out */
static float m_co, m_o2;
static const param_def_t bdefs[] = {
    PARAM_REAL(1300, "m_co", m_co, -1000.0f, 1000.0f, 0.0f, "ppm", PLAT_LVL_VIEW, PF_RO),
    PARAM_REAL(1301, "m_o2", m_o2, -1000.0f, 1000.0f, 0.0f, "%", PLAT_LVL_VIEW, PF_RO),
};
static const plat_meas_def_t bmeas[] = {
    { .id = 1300, .index = PLAT_MEAS_NO_INDEX, .max_age_ms = 0, .lo = 0.0f, .hi = 0.0f },
    { .id = 1301, .index = PLAT_MEAS_NO_INDEX, .max_age_ms = 0, .lo = 0.0f, .hi = 0.0f },
};

static void t_bind(void) {
    bsp_host_reset(); bsp_host.ms = 1000;
    plat_state_reset_for_test(); emis_bind_reset_for_test();
    CHECK_EQ(param_init(bdefs, 2), PERR_OK);
    param_set_defaults(PARAM_ALL);
    CHECK_EQ(plat_state_init(bmeas, 2), 0);
    CHECK(param_find(PARAM_ID_EMIS_SRC) != NULL);
    CHECK_EQ(emis_set.src[7], EMIS_NONE);                  /* defaults: no channel fed */
    CHECK(param_find(PARAM_ID_EMIS_QAL2_A)->flags & PF_NO_MODBUS);
    emis_bind_scan(bsp_ms());
    CHECK_EQ(emis_res.q[1][3], PLAT_Q_UNAVAILABLE);        /* off */
    /* CO in ppm from measurement 0, dry O2 from measurement 1 */
    emis_set.enable = true;
    emis_set.src[0] = 1; emis_set.ch.unit[0] = EMIS_U_PCT;
    emis_set.src[3] = 0; emis_set.ch.unit[3] = EMIS_U_PPM; emis_set.ch.molar_mass[3] = 28.010f; emis_set.ch.ref[3] = true;
    m_co = 50.0f; m_o2 = 11.0f;
    plat_state_scan(bsp_ms());
    plat_meas_update(0, PLAT_Q_VALID); plat_meas_update(1, PLAT_Q_VALID);
    plat_app_state(PLAT_ST_MEASURING);
    bsp_host_advance_ms(100); plat_state_scan(bsp_ms());
    emis_bind_scan(bsp_ms());
    CHECK_EQ(emis_p_cfg_error, 0);
    NEAR(emis_res.v[EMIS_B_REF][3], 62.4834, 1e-3);
    CHECK_EQ(emis_res.q[1][3], plat_meas_quality(0));
    /* a bad setting stops it and says where */
    emis_set.ch.molar_mass[3] = 0.0f;
    emis_bind_scan(bsp_ms());
    CHECK_EQ(emis_p_cfg_error, 4);
    CHECK_EQ(emis_res.q[1][3], PLAT_Q_UNAVAILABLE);
    emis_set.ch.molar_mass[3] = 28.010f;
    emis_bind_scan(bsp_ms());
    CHECK_EQ(emis_p_cfg_error, 0);
    /* an instrument point as the source: no poller in this build, so unavailable */
    emis_set.src[3] = (uint8_t)(EMIS_SRC_INST + 2u);
    emis_bind_scan(bsp_ms());
    CHECK_EQ(emis_res.q[EMIS_Q_IN][3], PLAT_Q_UNAVAILABLE);
    emis_set.src[3] = 0;
    emis_set.enable = false;
    emis_bind_scan(bsp_ms());
    plat_state_reset_for_test(); emis_bind_reset_for_test();
}

void test_emis_all(void) {
    RUN(t_formulas); RUN(t_chain); RUN(t_diluent); RUN(t_short); RUN(t_day); RUN(t_clock); RUN(t_cal_range); RUN(t_check); RUN(t_retain); RUN(t_bind);
    emis_reset_for_test();
}
