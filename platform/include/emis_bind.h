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
extern bool     emis_p_enable, emis_p_only_above, emis_p_ci_subtract, emis_p_clamp_zero, emis_p_operating;
extern float    emis_p_t_ref, emis_p_p_ref, emis_p_ref_pct, emis_p_o2_max, emis_p_h2o_fixed, emis_p_t_fixed,
                emis_p_p_fixed, emis_p_cal_ext;
extern uint8_t  emis_p_diluent, emis_p_ch_o2, emis_p_h2o_mode, emis_p_ch_h2o, emis_p_ch_t, emis_p_ch_p, emis_p_ch_flow,
                emis_p_min_avail, emis_p_max_discard;
extern uint16_t emis_p_period_s, emis_p_cfg_error;
extern uint8_t  emis_p_src[EMIS_CH_MAX], emis_p_unit[EMIS_CH_MAX], emis_p_add[EMIS_CH_MAX];
extern bool     emis_p_wet[EMIS_CH_MAX], emis_p_ref[EMIS_CH_MAX];
extern float    emis_p_mm[EMIS_CH_MAX], emis_p_factor[EMIS_CH_MAX], emis_p_a[EMIS_CH_MAX], emis_p_b[EMIS_CH_MAX],
                emis_p_ys_max[EMIS_CH_MAX], emis_p_elv[EMIS_CH_MAX], emis_p_ci[EMIS_CH_MAX], emis_p_lo[EMIS_CH_MAX],
                emis_p_hi[EMIS_CH_MAX];
extern float    emis_r_bank[EMIS_BANKS][EMIS_CH_MAX];
extern uint8_t  emis_r_q[6][EMIS_CH_MAX];
extern uint16_t emis_r_status[EMIS_CH_MAX], emis_r_disc[EMIS_CH_MAX], emis_r_days[EMIS_CH_MAX], emis_r_weeks[EMIS_CH_MAX];
extern const uint8_t emis_dflt_none[EMIS_CH_MAX];
extern const float   emis_dflt_one[EMIS_CH_MAX];

#endif
