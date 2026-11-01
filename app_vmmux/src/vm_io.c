/* vm_io.c - the platform's I/O image <-> the original program's I/O tags.
 *
 * Channel numbers follow 10_IO_Map.md (APP_INTEGRATION.md section 3):
 *   DI  0..13  TA30_DI[0..13]   (On_board_IO_1.Digital_Inputs)  DIG_IN_01..14
 *   DI 14..29  UID_DI[0..15]    (UID_W1616R_0.Inputs)           DIG_IN_15..20 used (UID I0..I5)
 *   DO  0..9   TA30_DO[0..9]    (On_board_IO_1.Digital_Outputs) VALVE_OUT_01..10
 *   DO 10..19  UID_DO[0..9]     (UID_W1616R_0.Outputs)          VALVE_OUT_11..20
 *   AI  0..1   TA30_AI[0..1] + TA30_AI_Status[0..1]             AIN_1, AIN_2 (4-20 mA)
 *   RTD 0..1   TA30_TEMP[0..1]                                  RTD_1, RTD_2 (PT100)
 * UID_DO[10..15] are never written by the original (10_IO_Map section 6), so DO 20..25
 * are not driven; the platform starts the output image all off every scan.
 *
 * Value conversions, so the faithful logic sees what the TA30 would have given it:
 *   AI   TA30 range 0..16383 (14-bit, 4-20 mA). The platform gives raw counts of a
 *        front end that is not chosen yet (APP_INTEGRATION.md section 3); VM_AI_RAW_FS is
 *        the count at 20 mA. Status: OVER -> 2 (the only code the logic tests, "input
 *        overflow", 10_IO_Map section 4); any other fault -> VM_AI_STATUS_FAULT, which the
 *        logic ignores (as it ignored every other TA30 code) - the fault is reported by the
 *        platform alarm and the BAD log quality instead.
 *   RTD  TA30 gives 0.1 degree units in an INT16 with fault values 32767 (overflow),
 *        -32767 (underflow), -32768 (connection fault) (10_IO_Map section 5). The platform
 *        gives 0.01 degC. NOT_FITTED and FAULT are given the connection-fault value, as
 *        APP_INTEGRATION.md section 3 asks ("treat it like a sensor fault, not 0"). The
 *        logic adds TemperatureShift with no fault check, as the original did (recorded
 *        quirk); the alarm and log quality carry the fault.
 */
#include "vm_app.h"
#include "vm_tags.h"

#ifndef VM_AI_RAW_FS
#define VM_AI_RAW_FS 16383   /* placeholder until the analog front end is chosen (hardware decision) */
#endif

int16_t vm_ai_to_ta30(int32_t raw)
{
    if (raw <= 0) return 0;
    int64_t v = ((int64_t)raw * 16383 + VM_AI_RAW_FS / 2) / VM_AI_RAW_FS;
    return (int16_t)(v > 16383 ? 16383 : v);
}

int16_t vm_ai_status_to_ta30(uint8_t st)
{
    if (st == PLAT_AIN_OK) return 0;
    if (st == PLAT_AIN_OVER) return 2;
    return VM_AI_STATUS_FAULT;
}

int16_t vm_rtd_to_ta30(int32_t cdeg, uint8_t st)
{
    switch (st) {
    case PLAT_AIN_OK: break;
    case PLAT_AIN_OVER:  return 32767;
    case PLAT_AIN_UNDER: return -32767;
    default:             return -32768;      /* FAULT, NOT_FITTED */
    }
    /* 0.01 -> 0.1 degree, rounded half away from zero */
    int32_t d = cdeg >= 0 ? (cdeg + 5) / 10 : -((-cdeg + 5) / 10);
    if (d > 32766) return 32766;             /* stay clear of the fault codes */
    if (d < -32766) return -32766;
    return (int16_t)d;
}

void vm_io_in(const plat_io_in_t *in)
{
    for (unsigned i = 0; i < 14; i++) TA30_DI[i] = plat_di(in, i);
    for (unsigned i = 0; i < 16; i++) UID_DI[i]  = plat_di(in, 14u + i);
    for (unsigned c = 0; c < 2; c++) {
        TA30_AI[c] = vm_ai_to_ta30(in->ai_raw[c]);
        TA30_AI_Status[c] = vm_ai_status_to_ta30(in->ai_status[c]);
        TA30_TEMP[c] = vm_rtd_to_ta30(in->rtd_cdeg[c], in->rtd_status[c]);
        vm_chan_ai_status[c] = in->ai_status[c];
        vm_chan_rtd_status[c] = in->rtd_status[c];
    }
}

void vm_io_out(plat_io_out_t *out)
{
    for (unsigned i = 0; i < 10; i++) plat_do_set(out, i, TA30_DO[i]);
    for (unsigned i = 0; i < 10; i++) plat_do_set(out, 10u + i, UID_DO[i]);
}
