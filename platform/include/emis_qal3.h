/* emis_qal3.h - EN 14181 QAL3: the zero and span checks of each analyser between QAL2s, on
 * Shewhart control charts (design: docs/EMISSIONS_MATH.md section 9).
 *
 * Each check gives a zero reading and a span reading (the analyser's own unit, as measured, like
 * the engine's B_RAW). The deviation of each from its reference (the zero gas, the span gas's
 * certified value) goes on its own chart, zero and span apart, so drift at either end can be
 * seen and the analyser adjusted (EN 14181 7, Annex C; EU ETS guidance GD7 3.3.3). With
 * s = s_AMS of the channel (from its QAL1 certificate, the analyser's unit):
 *
 *   WARN  the last deviation is beyond the action limit, k s (k = 2: GD7 example, "twice the
 *         sAMS around the zero and span reference values")
 *   OUT   out of control (latched until the channel is reset after maintenance or adjustment):
 *         3 consecutive deviations beyond the same action limit, or 8 consecutive on the same
 *         side of the reference (GD7's example decision rules), or one beyond 1.5 k s (3 s with
 *         k = 2, the Shewhart chart's 3-sigma line)
 *   DUE   no check for interval_d days (AG3 FAQ 5.4: no more than 2 weeks between checks; weekly
 *         for CUSUM), or none yet
 *
 * A channel's chart starts again (counts cleared, DUE) when its s_AMS or references change.
 * Platform alarm QAL3 (warning) while any channel is OUT or DUE.
 *
 * Threading: the scan task only. */
#ifndef EMIS_QAL3_H
#define EMIS_QAL3_H
#include <stdint.h>
#include <stdbool.h>
#include "emis.h"

#define EMIS_Q3_ON        0x01u   /* the channel has QAL3 (s_AMS > 0) */
#define EMIS_Q3_ZERO_WARN 0x02u   /* last zero deviation beyond k s */
#define EMIS_Q3_SPAN_WARN 0x04u
#define EMIS_Q3_ZERO_OUT  0x08u   /* zero chart out of control (latched) */
#define EMIS_Q3_SPAN_OUT  0x10u
#define EMIS_Q3_DUE       0x20u   /* check overdue, or none since the chart started */

#define EMIS_Q3_RUN_OUT   3u      /* consecutive beyond the same action limit */
#define EMIS_Q3_RUN_SIDE  8u      /* consecutive on the same side of the reference */
#define EMIS_Q3_HARD      1.5f    /* one point beyond this x the action limit */

typedef struct {
    float    k;                       /* action limit, multiples of s_AMS (2) */
    uint16_t interval_d;              /* days between checks before DUE (14) */
    float    s_ams[EMIS_CH_MAX];      /* 0 = no QAL3 for the channel */
    float    zero_ref[EMIS_CH_MAX];   /* zero gas value (0) */
    float    span_ref[EMIS_CH_MAX];   /* span gas certified value (80-100 % of the half-hour ELV) */
} emis_q3_set_t;

typedef struct {                      /* results, one array per kind (parameters point at them) */
    float    zero_dev[EMIS_CH_MAX];   /* last zero reading - zero_ref */
    float    span_dev[EMIS_CH_MAX];
    uint32_t last[EMIS_CH_MAX];       /* clock of the last check (0 = none) */
    uint8_t  status[EMIS_CH_MAX];     /* EMIS_Q3_* */
    uint8_t  n[EMIS_CH_MAX];          /* checks on the chart since it started (to 255) */
} emis_q3_res_t;
extern emis_q3_res_t emis_q3;

void emis_qal3_init(void);            /* every chart empty */
/* A check: 0 = recorded; -1 bad channel, -2 no QAL3 on the channel, -3 a reading not a number,
 * -4 no clock. */
int  emis_qal3_record(const emis_q3_set_t *s, unsigned ch, float zero, float span, uint32_t clock_s);
/* After maintenance or an adjustment: the chart starts again (the out of control cleared). */
void emis_qal3_reset(unsigned ch);
/* Every scan: settings changes, DUE. */
void emis_qal3_tick(const emis_q3_set_t *s, uint32_t clock_s);
bool emis_qal3_alarm(void);           /* any channel OUT or DUE */

#define EMIS_Q3_SAVE_SIZE (8u + EMIS_CH_MAX * 32u)
void emis_qal3_save(void *buf);       /* EMIS_Q3_SAVE_SIZE bytes, 4-byte aligned */
bool emis_qal3_load(const void *buf);

#endif
