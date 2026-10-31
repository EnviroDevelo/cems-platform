/* emis_bind.h - the emissions engine in the platform (emis_bind.c): parameters 400-480 and the
 * retained blocks 910 (averages in progress) and 911 (QAL3 charts). */
#ifndef EMIS_BIND_H
#define EMIS_BIND_H
#include <stdint.h>
#include <stddef.h>

void emis_bind_scan(uint32_t now_ms);    /* scan task, after plat_state_scan (plat_sys.c) */
void emis_bind_reset_for_test(void);

/* Source of a channel (parameter 430): 0..199 a declared measurement (plat_meas index), 200..254
 * instrument point (src - 200), 255 none. The instrument poller defines emis_inst_point (a weak
 * stub here answers "no such point"); its quality is a PLAT_Q_* code. */
#define EMIS_SRC_INST 200u
bool emis_inst_point(unsigned p, float *value, uint8_t *q);

/* the tags behind parameters 400-469 (param.c builds the table entries) */
#include <stdbool.h>
#include "emis.h"
typedef struct {
    float    t_ref, p_ref, ref_pct, o2_max, h2o_fixed, t_fixed, p_fixed, cal_ext;
    uint16_t period_s, period2_s;
    uint8_t  diluent, ch_o2, h2o_mode, ch_h2o, ch_t, ch_p, ch_flow, min_avail, max_discard;
    bool     enable, only_above, ci_subtract, clamp_zero;
    uint8_t  src[EMIS_CH_MAX];       /* 430 */
    emis_chs_t ch;                   /* 431-442: the engine reads these in place */
} emis_set_t;
extern emis_set_t emis_set;            /* 400-421, 430-443 */
extern bool       emis_p_operating;    /* 422 */
extern uint16_t   emis_p_cfg_error;    /* 423 */
/* 450-475 point into emis_res (emis.h) */

/* QAL3 (emis_qal3.h): settings 425, 426, 444-446 (a change does not restart the engine), the
 * check entry 447, 448 then 427 = channel + 1; 428 = channel + 1 resets a chart after
 * maintenance; 429 the last entry's result; 476-480 point into emis_q3. */
#include "emis_qal3.h"
extern emis_q3_set_t emis_q3set;
extern uint8_t emis_p_q3_record, emis_p_q3_reset, emis_p_q3_result;
extern float   emis_p_q3_zero[EMIS_CH_MAX], emis_p_q3_span[EMIS_CH_MAX];
enum { EMIS_Q3R_OK = 0, EMIS_Q3R_CHANNEL, EMIS_Q3R_OFF, EMIS_Q3R_NAN, EMIS_Q3R_CLOCK };
extern const uint8_t emis_dflt_none[EMIS_CH_MAX];

#endif
