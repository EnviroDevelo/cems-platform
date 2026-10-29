/* emis_bind.h - the emissions engine in the platform (emis_bind.c): parameters 400-469 and the
 * retained block 910. */
#ifndef EMIS_BIND_H
#define EMIS_BIND_H
#include <stdint.h>
#include <stddef.h>

void emis_bind_scan(uint32_t now_ms);    /* scan task, after plat_state_scan (plat_sys.c) */
void emis_bind_reset_for_test(void);

/* the tags behind parameters 400-469 (param.c builds the table entries) */
#include <stdbool.h>
#include "emis.h"
typedef struct {
    float    t_ref, p_ref, ref_pct, o2_max, h2o_fixed, t_fixed, p_fixed, cal_ext;
    uint16_t period_s;
    uint8_t  diluent, ch_o2, h2o_mode, ch_h2o, ch_t, ch_p, ch_flow, min_avail, max_discard;
    bool     enable, only_above, ci_subtract, clamp_zero;
    uint8_t  src[EMIS_CH_MAX];       /* 430 */
    emis_chs_t ch;                   /* 431-443: the engine reads these in place */
} emis_set_t;
extern emis_set_t emis_set;            /* 400-421, 430-443 */
extern bool       emis_p_operating;    /* 422 */
extern uint16_t   emis_p_cfg_error;    /* 423 */
/* 450-469 point into emis_res (emis.h) */
extern const uint8_t emis_dflt_none[EMIS_CH_MAX];

#endif
