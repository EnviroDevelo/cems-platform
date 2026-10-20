/* vm_alarms.c - the application's alarms (APP_INTEGRATION.md section 6, 70_Alarms_Logging.md
 * section 8, 70_alarm_map.csv). The logic only reports conditions; the platform owns delays,
 * latch, acknowledge, history, email and relays.
 *
 *   100..121  DI_ALARM_01..22   one per input: AlarmsVM[i] OR AlarmsMUX[i] (VM and MUX merged,
 *                               43 section 3). Description: the VM text "Alarm<i> Description"
 *                               (the MUX texts stay parameters; see INTEGRATION.md Q4)
 *   130..149  VALVE_FB_01..20   FBAlarmsMUX[i], delay = valve_fb_settle_s (D19, default 5 s)
 *   160/162   AI1/AI2_RANGE     AnalogSensor outside RangeMin..RangeMax, 10 s on and off (panel
 *                               alarms 24-25), only while AnalogAlarmEnabled (the old suppression)
 *   164/166   RTD1/RTD2_RANGE   TemperatureSensor (0.1 degree) outside Min..MaxTemperatureRange,
 *                               no delay (panel alarms 29-30, finding K5 kept: see Q5)
 *   161..167 odd  *_FAULT       sensor fault of an enabled channel (70 section 8, new)
 *   170       FLOW_VALIDATION   FlowValidationTimer.Out, latched until acknowledged (alarms 81+82, K6)
 *   171       DIGITAL_STOP      StopSeqBit, status alarm (no acknowledge, as alarm 79)
 *   172       ANALYSER_NOT_OK   AnalyzerOkBitAlarm, status alarm (as alarm 80)
 *   173       SAMPLING_PAUSED   PauseMUX, status alarm (alarm 87; never on in the original, V22)
 * Dropped (70_alarm_map.csv): ranges of AI3/AI4 and RTD3/RTD4 (no hardware, K10); the old
 * email, store and log errors are the platform's alarms 2, 3 and 10.
 * Default actions (.route): Modbus common alarm only. The old program emailed only when the
 * per-alarm email tick was set, and those ticks start off; the user turns email or a relay
 * on per alarm (D20). */
#include <math.h>
#include "vm_app.h"
#include "vm_tags.h"
#include "vm_params.h"
#include "vm_alarms.h"

uint16_t vm_fb_settle_s = 5;

#define DI(n, i) { .id = (uint16_t)(VM_AL_DI_FIRST + (i)), .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, \
    .name = "DI_ALARM_" n, .text = "Digital input " n " alarm", .text_param = VMP_ALARM##i##_DESCRIPTION, \
    .route = AR_MODBUS }
#define FB(n, i) { .id = (uint16_t)(VM_AL_FB_FIRST + (i)), .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, \
    .name = "VALVE_FB_" n, .text = "Valve DO" #i " feedback alarm", .delay_param = VM_PARAM_FB_SETTLE_S, \
    .route = AR_MODBUS }

static const alarm_def_t defs[] = {
    DI("01", 0),
    DI("02", 1),
    DI("03", 2),
    DI("04", 3),
    DI("05", 4),
    DI("06", 5),
    DI("07", 6),
    DI("08", 7),
    DI("09", 8),
    DI("10", 9),
    DI("11", 10),
    DI("12", 11),
    DI("13", 12),
    DI("14", 13),
    DI("15", 14),
    DI("16", 15),
    DI("17", 16),
    DI("18", 17),
    DI("19", 18),
    DI("20", 19),
    DI("21", 20),
    DI("22", 21),
    FB("01", 0),
    FB("02", 1),
    FB("03", 2),
    FB("04", 3),
    FB("05", 4),
    FB("06", 5),
    FB("07", 6),
    FB("08", 7),
    FB("09", 8),
    FB("10", 9),
    FB("11", 10),
    FB("12", 11),
    FB("13", 12),
    FB("14", 13),
    FB("15", 14),
    FB("16", 15),
    FB("17", 16),
    FB("18", 17),
    FB("19", 18),
    FB("20", 19),
    { .id = VM_AL_AI1_RANGE, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .name = "AI1_RANGE",
      .text = "Analog input 1 out of range", .on_delay_ms = 10000, .off_delay_ms = 10000, .route = AR_MODBUS },
    { .id = VM_AL_AI1_FAULT, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .name = "AI1_FAULT",
      .text = "Analog input 1 sensor fault", .route = AR_MODBUS },
    { .id = VM_AL_AI2_RANGE, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .name = "AI2_RANGE",
      .text = "Analog input 2 out of range", .on_delay_ms = 10000, .off_delay_ms = 10000, .route = AR_MODBUS },
    { .id = VM_AL_AI2_FAULT, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .name = "AI2_FAULT",
      .text = "Analog input 2 sensor fault", .route = AR_MODBUS },
    { .id = VM_AL_RTD1_RANGE, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .name = "RTD1_RANGE",
      .text = "Temperature 1 out of range", .route = AR_MODBUS },
    { .id = VM_AL_RTD1_FAULT, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .name = "RTD1_FAULT",
      .text = "Temperature 1 sensor fault", .route = AR_MODBUS },
    { .id = VM_AL_RTD2_RANGE, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .name = "RTD2_RANGE",
      .text = "Temperature 2 out of range", .route = AR_MODBUS },
    { .id = VM_AL_RTD2_FAULT, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .name = "RTD2_FAULT",
      .text = "Temperature 2 sensor fault", .route = AR_MODBUS },
    { .id = VM_AL_FLOW_VALIDATION, .severity = AL_SEV_ALARM, .flags = AL_LATCH | AL_NOTIFY, .name = "FLOW_VALIDATION",
      .text = "No flow detected when valve open", .route = AR_MODBUS },
    { .id = VM_AL_DIGITAL_STOP, .severity = AL_SEV_STATUS, .flags = AL_NO_ACK, .name = "DIGITAL_STOP",
      .text = "Digital stop activated", .route = AR_MODBUS },
    { .id = VM_AL_ANALYSER_NOT_OK, .severity = AL_SEV_WARNING, .flags = AL_NO_ACK | AL_NOTIFY, .name = "ANALYSER_NOT_OK",
      .text = "Analyzer not okay", .route = AR_MODBUS },
    { .id = VM_AL_SAMPLING_PAUSED, .severity = AL_SEV_STATUS, .flags = AL_NO_ACK, .name = "SAMPLING_PAUSED",
      .text = "MUX automated sampling paused during calibration", .route = 0 },
};
const alarm_def_t *const vm_alarm_defs = defs;
const size_t vm_n_alarms = sizeof defs / sizeof defs[0];

/* ---- producers: once per scan, after Function1 ---- */

static void range(uint16_t id, uint16_t fault_id, bool enabled, bool alarm_en, uint8_t st,
                  float v, float lo, float hi)
{
    alarm_set(fault_id, enabled && st != PLAT_AIN_OK);
    if (!alarm_en || !enabled) { alarm_set(id, false); return; }       /* the old suppression bit */
    alarm_set_range(id, st == PLAT_AIN_OK ? v : NAN, lo, hi, 0.0f);    /* NaN: keep, while the sensor is bad */
}

void vm_alarms_scan(void)
{
    for (unsigned i = 0; i < 22; i++) alarm_set((uint16_t)(VM_AL_DI_FIRST + i), AlarmsVM[i] || AlarmsMUX[i]);
    for (unsigned i = 0; i < 20; i++) alarm_set((uint16_t)(VM_AL_FB_FIRST + i), FBAlarmsMUX[i]);
    range(VM_AL_AI1_RANGE, VM_AL_AI1_FAULT, AnalogEnable[0], AnalogAlarmEnabled[0], vm_chan_ai_status[0],
          AnalogSensor[0], RangeMin[0], RangeMax[0]);
    range(VM_AL_AI2_RANGE, VM_AL_AI2_FAULT, AnalogEnable[1], AnalogAlarmEnabled[1], vm_chan_ai_status[1],
          AnalogSensor[1], RangeMin[1], RangeMax[1]);
    range(VM_AL_RTD1_RANGE, VM_AL_RTD1_FAULT, TemperatureEnable[0], TemperatureAlarmEnabled[0], vm_chan_rtd_status[0],
          (float)TemperatureSensor[0], (float)MinTemperatureRange[0], (float)MaxTemperatureRange[0]);
    range(VM_AL_RTD2_RANGE, VM_AL_RTD2_FAULT, TemperatureEnable[1], TemperatureAlarmEnabled[1], vm_chan_rtd_status[1],
          (float)TemperatureSensor[1], (float)MinTemperatureRange[1], (float)MaxTemperatureRange[1]);
    alarm_set(VM_AL_FLOW_VALIDATION, FlowValidationTimer.Out);
    alarm_set(VM_AL_DIGITAL_STOP, StopSeqBit);
    alarm_set(VM_AL_ANALYSER_NOT_OK, AnalyzerOkBitAlarm);
    alarm_set(VM_AL_SAMPLING_PAUSED, PauseMUX);
#if !VM_OLD_EMAIL
    /* The old Email rung 20 cleared FlowValidationAlarm on "Acknowledge Bit". Now the platform's
     * latched FLOW_VALIDATION alarm carries the acknowledge: once it has gone (acknowledged and
     * the condition gone) the old bit is cleared too, for its Modbus coil and the logic. */
    if (FlowValidationAlarm && !FlowValidationTimer.Out && !alarm_active(VM_AL_FLOW_VALIDATION))
        FlowValidationAlarm = false;
#endif
}
