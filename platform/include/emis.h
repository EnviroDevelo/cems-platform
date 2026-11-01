/* emis.h - the emissions calculation engine: from analyser readings to the values an emission
 * limit is judged on (design and formula references: docs/EMISSIONS_MATH.md).
 *
 * Standards: EN 14181 (QAL2 calibration function, valid calibration range), EN 17255-1 (data
 * acquisition and handling), Directive 2010/75/EU (IED) Annex V Part 3 (large combustion plants)
 * and Annex VI Parts 3, 6, 7, 8 (waste incineration), NSW Approved Methods for air (Appendix 1).
 *
 * Channels. EMIS_CH_MAX channels; channel n is slot n of the Modbus standard block (O2 dry = 0,
 * O2 wet = 1, CO2 = 2, CO = 3 ...). Each scan the caller gives every channel its raw reading and
 * that reading's quality (PLAT_Q_*, plat_state.h). For each channel the engine works out, in
 * this order (the order EN 14181 applies its calibration function in):
 *
 *   B_RAW    x, the reading as measured, in the analyser's own unit and conditions
 *            (a derived channel: the sum of two readings, e.g. NOx = NO + NO2 in ppm)
 *   y = a + b x   QAL2 calibration function, in the measuring conditions (EN 14181 6.4)
 *   B_STD    y at standard conditions, dry: mg/Nm3 (ppm and mg/m3 inputs), % vol dry (% inputs),
 *            Nm3/h dry (flow), y unchanged (anything else: temperature, pressure, opacity)
 *   B_REF    B_STD corrected to the reference O2 (or CO2): the value an ELV applies to
 *   B_MASS   mass emission kg/h = B_STD x the dry normalised flow / 1e6 (not O2 corrected)
 *   B_RUN    the running short-term average of B_REF (the period so far)
 *   B_AVG    the last complete short-term average of B_REF (10, 30 or 60 min, clock-aligned)
 *   B_VAL    B_AVG validated: the confidence interval (ci_pct of the ELV) subtracted
 *   B_DAY    the last complete daily average of the validated short-term averages
 *
 * A second, shorter period (period2_s, 0 = off; IED Annex VI Part 3 1.5 (c), Part 8 1.1 (d)(i)
 * and 1.2: 10-minute CO averages, at least 95 % of them within the 10-minute ELV) gives
 * every channel a validated (confidence interval subtracted) average of its own, emis_res.v2,
 * with the same valid-time rule, and the day's counts of valid ones and of those above elv2.
 * Its averages do not count towards the day's validity (that rule is for the half-hours).
 *
 * Quality. Every calculated value carries one of the eight PLAT_Q_* codes: the worst of the
 * qualities of everything it was calculated from (emis_q_worst), or the engine's own verdict
 * (RANGE: O2 too high to correct, the water content impossible, the analyser out of its range;
 * UNAVAILABLE: an input missing).
 * Only VALID values go into averages. A short-term average is valid when the valid time is at
 * least min_avail of the plant's operating time in that period; periods with no operating time
 * have no average and are not counted. A daily average is valid when no more than max_discard
 * short-term averages were discarded (IED Annex VI Part 8 1.2: 5 half-hours; Annex V Part 3
 * 10: 3 hours). Status bits (EMIS_ST_*) say why.
 *
 * Time. The caller gives the clock (local standard time, s since 1970, plat_clock.h) and the
 * time since the last step. Periods and days follow the clock. A clock step backwards, or forward
 * by more than a period, ends the period and the day in progress where they are. Clock 0 (never
 * set): no averages.
 *
 * Threading: the scan task only. The results (emis_res) are plain words and floats, written
 * once per step. */
#ifndef EMIS_H
#define EMIS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef EMIS_CH_MAX
#define EMIS_CH_MAX 32u
#endif
#define EMIS_NONE 0xFFu              /* "no channel" in a channel reference */

/* Value banks (the Modbus standard block's banks 0..7 in this order) */
enum { EMIS_B_RAW = 0, EMIS_B_STD, EMIS_B_REF, EMIS_B_MASS, EMIS_B_RUN, EMIS_B_AVG, EMIS_B_VAL, EMIS_B_DAY,
       EMIS_BANKS };

/* Unit of a channel's raw reading */
enum {
    EMIS_U_OFF = 0,     /* channel not used */
    EMIS_U_PPM,         /* volume fraction, ppm (umol/mol): to mg/Nm3 with the molar mass */
    EMIS_U_PCT,         /* volume fraction, % (O2, CO2, H2O): stays % */
    EMIS_U_MG_ACT,      /* mg/m3 at the stack's actual temperature and pressure (in-situ, dust) */
    EMIS_U_MG_STD,      /* mg/m3 already at the reference temperature and pressure */
    EMIS_U_FLOW_ACT,    /* volume flow, m3/h at actual stack conditions */
    EMIS_U_FLOW_STD,    /* volume flow, m3/h at the reference temperature and pressure */
    EMIS_U_OTHER,       /* passed through (temperature degC, pressure kPa, velocity, opacity) */
    EMIS_U_COUNT
};

/* Reference diluent */
enum { EMIS_DIL_NONE = 0, EMIS_DIL_O2, EMIS_DIL_CO2 };
/* How the water content is known */
enum { EMIS_H2O_NONE = 0,  /* every wet reading is refused (the sample is dried: IED Annex VI Pt 6 2.4) */
       EMIS_H2O_MEASURED,  /* from channel ch_h2o (% vol of the wet gas) */
       EMIS_H2O_FIXED };   /* h2o_fixed, a value the authority accepted */

/* Status bits (one word per channel; positions agreed with the Modbus standard block, bits 0-4
 * belong to the platform: NAMUR NE 107 and forced) */
#define EMIS_ST_NO_QAL2      0x0020u /* no calibration function (b = 0): y = x is used */
#define EMIS_ST_ABOVE_CAL    0x0040u /* B_REF above the valid calibration range (EN 14181 6.5) */
#define EMIS_ST_OVER_RANGE   0x0080u /* raw reading at or above the analyser's range */
#define EMIS_ST_O2_HIGH      0x0100u /* O2 above o2_max: not corrected (B_REF uses o2_max, quality RANGE) */
#define EMIS_ST_INPUT_MISSING 0x0200u /* an input (O2, H2O, T, p, flow, second reading) not configured */
#define EMIS_ST_AVG_INCOMPLETE 0x0400u /* the last short-term average was discarded (too little valid time) */
#define EMIS_ST_DAY_INVALID  0x0800u /* the last day had more than max_discard averages discarded */
#define EMIS_ST_CI_OFF       0x1000u /* B_VAL is B_AVG: no ELV, or confidence interval subtraction off */
#define EMIS_ST_FIXED_H2O    0x2000u /* wet-to-dry used the fixed water content */
#define EMIS_ST_BELOW_RANGE  0x4000u /* raw reading below the analyser's range (lo) */
#define EMIS_ST_QAL2_REVIEW  0x8000u /* valid calibration range exceeded too often: a new QAL2 is due (EN 14181 6.5) */

/* The channels' configuration, one array per setting (parameters 431-443 point straight at them) */
typedef struct {
    uint8_t  unit[EMIS_CH_MAX];   /* EMIS_U_* (OFF: not used) */
    bool     wet[EMIS_CH_MAX];    /* the raw reading is on a wet basis (hot-wet extractive, in-situ, zirconia O2) */
    bool     ref[EMIS_CH_MAX];    /* correct B_REF to the reference diluent (pollutants: 1; O2, flow, T: 0) */
    uint8_t  add[EMIS_CH_MAX];    /* derived channel: also add this channel's raw reading (EMIS_NONE = no) */
    float    molar_mass[EMIS_CH_MAX]; /* g/mol of what it is expressed as, times any factor: NOx as NO2 46.0055;
                                     TOC as C from a propane-calibrated FID 3 x 12.011 = 36.033; NO only, as NOx with
                                     an assumed NO2 share f: 46.0055 / (1 - f) */
    float    qal2_a[EMIS_CH_MAX], qal2_b[EMIS_CH_MAX]; /* EN 14181 calibration function y = a + b x (b = 0: none yet, y = x) */
    float    ys_max[EMIS_CH_MAX]; /* highest calibrated value at standard conditions in the QAL2 (0 = not known) */
    float    elv[EMIS_CH_MAX];    /* daily emission limit, at standard conditions and reference O2 (0 = none) */
    float    ci_pct[EMIS_CH_MAX]; /* 95 % confidence interval as % of the daily ELV (IED: CO 10, SO2 20, NOx 20, dust 30,
                                     TOC 30, HCl 40, HF 40) */
    float    elv2[EMIS_CH_MAX];   /* the second period's ELV (IED waste CO 10 min: 150 mg/Nm3), 0 = none */
} emis_chs_t;

/* The plant's configuration */
typedef struct {
    float    t_ref_k;     /* reference temperature, K (273.15) */
    float    p_ref_kpa;   /* reference pressure, kPa (101.325; IED and NSW write 101.3) */
    uint8_t  diluent;     /* EMIS_DIL_* */
    float    ref_pct;     /* reference O2 % (IED: 11 waste, 3 waste oil, 6 solid fuel, 3 liquid/gas, 15 turbines),
                             or reference CO2 % (EMIS_DIL_CO2, NSW Appendix 1 eq 1.6: 12) */
    float    o2_max;      /* O2 % dry above which no correction is made (20.0): (21 - O2) -> 0 */
    uint8_t  only_above;  /* correct only when the O2 is above the reference (IED Annex VI Pt 6 2.7,
                             hazardous waste with gas cleaning) */
    uint8_t  ch_o2;       /* channel whose B_STD is the dry O2 % (or CO2 %: EMIS_DIL_CO2) */
    uint8_t  h2o_mode;    /* EMIS_H2O_* */
    uint8_t  ch_h2o;      /* channel whose B_STD is the water content, % vol wet gas */
    float    h2o_fixed;   /* %, EMIS_H2O_FIXED */
    uint8_t  ch_t;        /* stack temperature channel, degC (EMIS_NONE: t_fixed_c) */
    uint8_t  ch_p;        /* stack absolute pressure channel, kPa (EMIS_NONE: p_fixed_kpa) */
    float    t_fixed_c, p_fixed_kpa;
    uint8_t  ch_flow;     /* volume flow channel (EMIS_NONE: no mass emissions) */
    uint32_t period_s;    /* short-term averaging period: 600, 1800 (IED Annex VI) or 3600 (Annex V) */
    float    min_avail;   /* share of the operating time that must be valid for a valid average (0.667) */
    uint8_t  max_discard; /* short-term averages a day may lose and stay valid (5 half-hours; 3 hours) */
    uint8_t  ci_subtract; /* subtract the confidence interval (IED: 1; NSW licences: 0) */
    uint8_t  clamp_zero;  /* a validated average below zero is reported as zero */
    float    cal_ext_pct; /* valid calibration range: 0 .. ys_max + this % of the ELV (10) */
    uint32_t period2_s;   /* second averaging period, 0 = off (600: IED waste CO); must divide a day */
} emis_cfg_t;

/* One channel's input for a step */
#define EMIS_IN_BELOW 0x01u           /* the reading is below the analyser's range */
#define EMIS_IN_OVER  0x02u           /* at or above it (saturated) */
typedef struct {
    float   x;            /* raw reading */
    uint8_t q;            /* its quality, PLAT_Q_* */
    uint8_t range;        /* EMIS_IN_* (the binding: from the measurement's lo..hi, plat_state.h) */
} emis_in_t;

/* One channel's results */
typedef struct {
    float    v[EMIS_BANKS];
    uint8_t  q_in;        /* the raw reading's quality (with the second reading's) */
    uint8_t  q_calc;      /* B_STD and B_REF */
    uint8_t  q_mass;      /* B_MASS */
    uint8_t  q_avg;       /* B_AVG and B_VAL */
    uint8_t  q_day;       /* B_DAY */
    uint8_t  q_run;       /* B_RUN */
    uint16_t status;      /* EMIS_ST_* */
    uint16_t discarded_today;  /* short-term averages discarded so far today */
    uint16_t days_invalid;     /* days invalid this calendar year (IED: report when more than 10) */
    uint16_t weeks_above;      /* consecutive weeks with more than 5 % of averages above the valid range */
    float    avg2;             /* the last validated second-period average */
    uint8_t  q_avg2;
    uint16_t n2_today, n2_above_today; /* valid second-period averages today, and those above elv2 */
    uint16_t n2_day, n2_above_day;     /* the same for the last complete day */
} emis_out_t;

/* Defaults: IED waste incineration (11 % O2, half-hours, 5 discards a day, CI subtracted). */
void emis_cfg_default(emis_cfg_t *g);
void emis_chs_default(emis_chs_t *c);

/* Check a configuration. 0 = good; otherwise 1 + the channel at fault, or EMIS_CH_MAX + 1 for the
 * plant settings; why says what (for the web page). */
int  emis_check(const emis_cfg_t *g, const emis_chs_t *ch, char *why, size_t cap);

/* Start (or restart after a configuration change): forgets the periods in progress. The engine keeps
 * the pointers, not copies: g and ch must stay valid and unchanged until the next emis_init. */
void emis_init(const emis_cfg_t *g, const emis_chs_t *ch);

/* One step: in[EMIS_CH_MAX] readings, operating = the plant is in its effective operating time
 * (fuel or waste being fired), clock_s = local s since 1970 (0 = not set), dt_ms = since the last
 * step. */
void emis_step(const emis_in_t in[EMIS_CH_MAX], bool operating, uint32_t clock_s, uint32_t dt_ms);

/* The results, one array per kind so the parameter table can point straight at them (no copy). */
enum { EMIS_Q_IN = 0, EMIS_Q_CALC, EMIS_Q_MASS, EMIS_Q_AVG, EMIS_Q_DAY, EMIS_Q_RUN, EMIS_QS };
typedef struct {
    float    v[EMIS_BANKS][EMIS_CH_MAX];
    uint8_t  q[EMIS_QS][EMIS_CH_MAX];      /* PLAT_Q_* */
    uint16_t status[EMIS_CH_MAX];          /* EMIS_ST_* */
    uint16_t discarded[EMIS_CH_MAX];
    uint16_t days_invalid[EMIS_CH_MAX];
    uint16_t weeks_above[EMIS_CH_MAX];
    float    v2[EMIS_CH_MAX];              /* the last validated second-period average */
    uint8_t  q2[EMIS_CH_MAX];
    uint16_t n2[EMIS_CH_MAX], n2_above[EMIS_CH_MAX]; /* today: valid ones, and those above elv2 */
    uint16_t n2_day[EMIS_CH_MAX], n2_above_day[EMIS_CH_MAX]; /* the last complete day */
} emis_res_t;
extern emis_res_t emis_res;                /* written by emis_step, the scan task */

emis_out_t emis_out(unsigned ch);          /* one channel's results gathered (all UNAVAILABLE for ch >= EMIS_CH_MAX) */
void emis_clear(void);                     /* every result 0 and UNAVAILABLE (the engine stopped) */

/* ---- the formulas, on their own (docs/EMISSIONS_MATH.md section 3) ---- */
uint8_t emis_q_worst(uint8_t a, uint8_t b);
float   emis_molar_volume(float t_k, float p_kpa);                      /* L/mol: R T / p (22.414 at 273.15 K, 101.325 kPa) */
float   emis_ppm_to_mg(float ppm, float molar_mass, float t_k, float p_kpa); /* mg/m3 at t_k, p_kpa */
float   emis_mg_to_ppm(float mg, float molar_mass, float t_k, float p_kpa);
float   emis_to_dry(float wet, float h2o_pct);                          /* wet x 100 / (100 - h2o) */
float   emis_norm_conc(float c_act, float t_k, float p_kpa, float t_ref_k, float p_ref_kpa); /* to reference T, p */
float   emis_norm_flow(float q_act, float t_k, float p_kpa, float t_ref_k, float p_ref_kpa);
float   emis_o2_ref(float c, float o2_meas, float o2_ref);               /* c (21 - o2_ref) / (21 - o2_meas) */
float   emis_co2_ref(float c, float co2_meas, float co2_ref);            /* c co2_ref / co2_meas */

/* Power cycles: the periods and the day in progress, the last averages and the counters, as one
 * block (EMIS_SAVE_SIZE bytes) for the retained store. emis_load refuses a block saved under
 * another configuration (cfg_crc) or layout; the engine then starts fresh. */
#define EMIS_SAVE_SIZE (32u + EMIS_CH_MAX * 32u)
void emis_save(void *buf, uint32_t cfg_crc);          /* buf: EMIS_SAVE_SIZE bytes, 8-byte aligned */
bool emis_load(const void *buf, uint32_t cfg_crc);

void emis_reset_for_test(void);

#endif
