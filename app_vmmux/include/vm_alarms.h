/* vm_alarms.h - application alarm IDs (vm_alarms.c). */
#ifndef VM_ALARMS_H
#define VM_ALARMS_H
#include <stdint.h>
enum {
    VM_AL_DI_FIRST = 100,          /* 100..121 DI_ALARM_01..22 */
    VM_AL_FB_FIRST = 130,          /* 130..149 VALVE_FB_01..20 */
    VM_AL_AI1_RANGE = 160, VM_AL_AI1_FAULT = 161, VM_AL_AI2_RANGE = 162, VM_AL_AI2_FAULT = 163,
    VM_AL_RTD1_RANGE = 164, VM_AL_RTD1_FAULT = 165, VM_AL_RTD2_RANGE = 166, VM_AL_RTD2_FAULT = 167,
    VM_AL_FLOW_VALIDATION = 170, VM_AL_DIGITAL_STOP = 171, VM_AL_ANALYSER_NOT_OK = 172, VM_AL_SAMPLING_PAUSED = 173,
};
enum { VM_PARAM_FB_SETTLE_S = 2900, VM_PARAM_LINK_CLOSE_MIN = 2901,
       VM_PARAM_RTD1_DELAY_S = 2902, VM_PARAM_RTD2_DELAY_S = 2903, VM_PARAM_RTD1_HYST = 2904, VM_PARAM_RTD2_HYST = 2905 };
extern uint16_t vm_fb_settle_s, vm_link_close_min, vm_rtd_delay_s[2];
extern float    vm_rtd_hyst[2];
void vm_alarms_scan(void);
#endif
