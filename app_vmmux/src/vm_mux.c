/* vm_mux.c - master enable of the Multiplexor feature (Gary, 20 Oct: "MUX may not always be
 * enabled inside the system, so we need a master enable/disable for the entire feature").
 *
 * mux_enable (parameter 2906, admin, retained, default on). While it is off, Function1 rung 36
 * does not call MUX and vm_mux_off() holds the MUX at rest every scan: no mode, no inlet or purge
 * open, no MUX outputs (DOFinalMUX feeds DO_Control's OR, 10_IO_Map section 2), no MUX alarms, and
 * the MUX buttons disabled (so their commands and Modbus mode starts are refused). The Valve
 * Manager is not affected. Turning it on starts from rest; nothing restarts by itself. */
#include <string.h>
#include "vm_app.h"
#include "vm_tags.h"
#include "vm_hmi.h"

bool vm_mux_enable = true;

void vm_mux_off(void)
{
    StartScheduleMUX = false; StartModbusMUX = false; ManualModeStartMUX = false;
    StartModbusCoilMUX = false; StartPeriodsMUX = false; StartMUXSchedule1 = false;
    ScheduleButtonStateMUX = 0; ModbusModeStateMUX = 0; ManualModeMUX = 0;
    ResetManualAttemptMUX = false; StartButtonVisibMUX = false; EnableSingulManualSelection = false;
    ModbusModeEnableMUX = false; SchedulerModeEnableMUX = false; ManualModeEnableMUX = false;
    CurrentModeMUX = 0; ModeIsActive = false;
    memset(SingularInletActivate, 0, sizeof SingularInletActivate);
    PurgeInletActivate = false; PurgeInletActivateDI = false; PurgeInletActivateModbus = false;
    memset(DOListMUX, 0, sizeof DOListMUX); memset(InitialDOMUX, 0, sizeof InitialDOMUX);
    memset(DOFinalMUX, 0, sizeof DOFinalMUX);
    memset(AlarmsMUX, 0, sizeof AlarmsMUX); memset(FBAlarmsMUX, 0, sizeof FBAlarmsMUX);
    AnalyzerOkBitAlarm = false; StopSeqBit = false;
    FlowValidationAlarm = false; treset(&FlowValidationTimer);
    SequenceAbsentMUX = false; YouHaveIncorrectInletOutletConfigMUX = false;
}
