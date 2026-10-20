/* test_4C.c - hand-worked tests for the Stage 4C I/O layer.
 * Build: gcc -std=c11 -Wall -Wextra test_4C.c 4C_faithful.c   (add -DENABLE_DISABLED_RUNGS for rungs 9-10) */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "4C.h"

bool TA30_DI[14], UID_DI[16], TA30_DO[10], UID_DO[16]; int16_t TA30_AI[2], TA30_AI_Status[2], TA30_TEMP[2];
bool AdditionalDiscreteModule, DiscreteInputsArray[22], TempBoolsDI[14], TempBoolsDI1[16];
uint8_t Di_FunctionVM[22], DI_FunctionMUX[22], AlarmConditionVM[22], AlarmConditionMUX[22], FeedbackConditionMUX[22];
bool AlarmsVM[22], AlarmEnableVM[22], EmailEnableVM[22], EnableVisibleVM[22];
bool AlarmsMUX[22], AlarmEnableMUX[22], EmailEnableMUX[22], EnableVisibleMUX[22], EmailVisibMUX[22], FeedbackStateAlarm[22];
bool FBAlarmsMUX[20], FBEmailEnableMUX[20];
bool FlushValveBit, FlushValveBitModbus, PurgeInletActivateDI, AnalyzerOkBit, AnalyzerFunctionIsActive, AnalyzerOkBitAlarm, StopSeqBit;
bool AlarmDescriptionEntered[22], AlarmDescriptionEnteredMUX[22]; int8_t AlarmStatus[22], AlarmStatusMUX[22];
bool DOList[20], InitialDO[20], DOFinal[20], DOListMUX[20], InitialDOMUX[20], DOFinalMUX[20];
int16_t DOInlet[19], DOOutlet[19], InletSlotByLegacyDO[19], OutletSlotByLegacyDO[19];
int16_t FirstInlets, FirstOutlets, SecondInlets, SecondOutlets, NumInterconnections;
int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
int16_t ActiveInletIdx, DOSingularInlet[19], DOPurgeInlet, SingularInletState[19], PurgeInletState;
bool SingularInletActivate[19], PrevSingularInletActivate[19], PurgeInletActivate, PurgeInletActivateModbus;
bool BackFlushExists, BackFlushOptionVisib, BackFlushValveAvailable; uint8_t FlushValveState;
float AnalogSensor[4], FirstCalibPoint[4], SecondCalibPoint[4];
bool AnalogEnable[4], AnalogAlarmEnabled[4], AnalogAlarmEmailEnabled[4], AnalogAlarmDescriptionEntered[4], AnalogSuppression[4];
int8_t AnalogAlarmStatus[4], TemperatureAlarmStatus[4]; int16_t TemperatureSensor[4], TemperatureShift[4];
bool TemperatureEnable[4], TemperatureAlarmEnabled[4], TemperatureAlarmEmailEnabled[4], TemperatureAlarmDescriptionEntered[4], TemperatureAlarmSuppression[4];
bool ParameterColNumberLoggingArray[20];

static int fails, checks;
static void eqi(const char *w, long g, long x) { checks++; if (g != x) { fails++; printf("FAIL %-58s got %ld want %ld\n", w, g, x); } }
static void eqf(const char *w, double g, double x) { checks++; if (fabs(g - x) > 1e-3) { fails++; printf("FAIL %-58s got %g want %g\n", w, g, x); } }
static int ones(const bool *a, int n) { int k = 0; for (int i = 0; i < n; i++) k += a[i]; return k; }

int main(void) {
    for (int i = 0; i < 19; i++) { InletSlotByLegacyDO[i] = (int16_t)i; OutletSlotByLegacyDO[i] = (int16_t)i; }  /* power-up values */

    /* ---- DI_Control (VM) ---- */
    memset(Di_FunctionVM, 0, sizeof Di_FunctionVM);
    Di_FunctionVM[0] = 1; AlarmEnableVM[0] = true; AlarmConditionVM[0] = 0;   /* alarm when input OFF */
    Di_FunctionVM[1] = 1; AlarmEnableVM[1] = true; AlarmConditionVM[1] = 1;   /* alarm when input ON  */
    Di_FunctionVM[2] = 2;                                                     /* flush trigger */
    Di_FunctionVM[3] = 5;                                                     /* drives DO index 2 */
    Di_FunctionVM[4] = 0; AlarmEnableVM[4] = true; EmailEnableVM[4] = true;   /* unused: settings cleared */
    Di_FunctionVM[15] = 1; AlarmEnableVM[15] = true; AdditionalDiscreteModule = false;
    memset(DiscreteInputsArray, 0, sizeof DiscreteInputsArray);
    DiscreteInputsArray[1] = true; DiscreteInputsArray[2] = true; DiscreteInputsArray[3] = true;
    g_oob = 0; DI_Control();
    eqi("VM alarm cond 0, input OFF -> alarm", AlarmsVM[0], 1);
    eqi("VM alarm cond 1, input ON -> alarm", AlarmsVM[1], 1);
    eqi("VM flush trigger", FlushValveBit, 1);
    eqi("VM function 5 -> DOList[2]", DOList[2], 1);
    eqi("VM DOList: only one ON", ones(DOList, 20), 1);
    eqi("VM unused channel: alarm enable cleared", AlarmEnableVM[4], 0);
    eqi("VM unused channel: email enable cleared", EmailEnableVM[4], 0);
    eqi("VM ch15 hidden without extra module", EnableVisibleVM[15], 0);
    eqi("VM ch15 alarm enable cleared without module", AlarmEnableVM[15], 0);
    eqi("VM DI_Control: out-of-range accesses at index 20 (read+write)", g_oob, 2);
    DiscreteInputsArray[3] = false; DI_Control();
    eqi("VM DO follows input (locals start at 0, A4.2)", DOList[2], 0);

    /* ---- DI_Assigning: R1 edge, R3/R4, R7-8 copy, order ---- */
    AdditionalDiscreteModule = true; DI_Assigning();
    Di_FunctionVM[18] = 7; AdditionalDiscreteModule = false; DI_Assigning();
    eqi("VM falling edge of extra module clears functions 14..21", Di_FunctionVM[18], 0);
    AlarmDescriptionEntered[21] = true; DI_Assigning();
    eqi("VM description entered: alarm ID 22 updated", g_alarm_desc_calls[22], 1);
    eqi("VM description request cleared", AlarmDescriptionEntered[21], 0);
    TA30_DI[0] = true; DI_Assigning();
    eqi("VM as saved: real input does NOT reach DiscreteInputsArray", DiscreteInputsArray[0], 0);

    /* ---- DI_ControlMUX ---- */
    memset(DI_FunctionMUX, 0, sizeof DI_FunctionMUX); memset(DiscreteInputsArray, 0, sizeof DiscreteInputsArray);
    memset(InitialDOMUX, 0, sizeof InitialDOMUX);
    AdditionalDiscreteModule = false;
    DI_FunctionMUX[0] = 3;  FeedbackConditionMUX[0] = 0; InitialDOMUX[0] = true;  DiscreteInputsArray[0] = false; /* commanded ON, no FB */
    DI_FunctionMUX[1] = 4;  FeedbackConditionMUX[1] = 0; InitialDOMUX[1] = true;  DiscreteInputsArray[1] = true;  /* OK */
    DI_FunctionMUX[2] = 5;  FeedbackConditionMUX[2] = 1; InitialDOMUX[2] = false; DiscreteInputsArray[2] = true;  /* inverted, OK */
    DI_FunctionMUX[3] = 6;  FeedbackConditionMUX[3] = 1; InitialDOMUX[3] = true;  DiscreteInputsArray[3] = true;  /* inverted, fault */
    DI_FunctionMUX[4] = 14; DiscreteInputsArray[4] = true;                         /* drives DO 0 */
    DI_FunctionMUX[5] = 13; DiscreteInputsArray[5] = false;                        /* analyzer OK input, not OK */
    DI_FunctionMUX[6] = 24; DiscreteInputsArray[6] = true;                         /* stop sequence */
    DI_FunctionMUX[7] = 2;  DiscreteInputsArray[7] = true;                         /* purge */
    DI_FunctionMUX[8] = 3;  EmailEnableMUX[8] = true;                              /* 2nd FB channel for valve 0 with email */
    DI_FunctionMUX[9] = 30;                                                        /* out of range without module: unused */
    DI_AssigningMUX();
    eqi("MUX FB valve 0 commanded ON, no feedback -> FB alarm", FBAlarmsMUX[0], 1);
    eqi("MUX FB valve 1 OK", FBAlarmsMUX[1], 0);
    eqi("MUX FB valve 2 inverted OK", FBAlarmsMUX[2], 0);
    eqi("MUX FB valve 3 inverted fault", FBAlarmsMUX[3], 1);
    eqi("MUX function 14 -> DOListMUX[0]", DOListMUX[0], 1);
    eqi("MUX analyzer function active", AnalyzerFunctionIsActive, 1);
    eqi("MUX analyzer not OK -> alarm", AnalyzerOkBitAlarm, 1);
    eqi("MUX stop sequence", StopSeqBit, 1);
    eqi("MUX purge from DI", PurgeInletActivateDI, 1);
    eqi("MUX FB email for valve 0", FBEmailEnableMUX[0], 1);
    eqi("MUX channel with function 30, no module: hidden", EmailVisibMUX[9], 0);
    AdditionalDiscreteModule = true; DI_FunctionMUX[4] = 24; DI_FunctionMUX[6] = 44; DI_FunctionMUX[5] = 23; DiscreteInputsArray[5] = true;
    DI_AssigningMUX();
    eqi("MUX with module: function 24 -> DO 0", DOListMUX[0], 1);
    eqi("MUX with module: function 44 stop", StopSeqBit, 1);
    eqi("MUX with module: analyzer OK -> no alarm", AnalyzerOkBitAlarm, 0);

    /* ---- DO_Assign (VM) ---- */
    for (int i = 0; i < 19; i++) { DOInlet[i] = (int16_t)i; DOOutlet[i] = (int16_t)i; }
    NumInterconnections = 1; FirstInlets = 5; FirstOutlets = 4; SecondInlets = 0; SecondOutlets = 0;
    SelectedFirstInlet = 2; SelectedFirstOutlet = 3; g_oob = 0; DO_Assign();
    eqi("VM inlet 2 -> DOInlet[1] = output 1", InitialDO[1], 1);
    eqi("VM outlet 3 -> DOOutlet[5+2=7] = output 7", InitialDO[7], 1);
    eqi("VM exactly two outputs", ones(InitialDO, 20), 2);
    NumInterconnections = 2; FirstInlets = 3; SecondInlets = 2; FirstOutlets = 2; SecondOutlets = 3;
    SelectedFirstInlet = 0; SelectedFirstOutlet = 0; SelectedSecondInlet = 1; SelectedSecondOutlet = 1; DO_Assign();
    eqi("VM tube 2 inlet 1 -> legacy 3+2-1=4", InitialDO[4], 1);
    eqi("VM tube 2 outlet 1 -> legacy 3+2+2+3-1=9", InitialDO[9], 1);
    NumInterconnections = 1; FirstInlets = 10; FirstOutlets = 10; SecondInlets = 0; SecondOutlets = 0;
    SelectedFirstInlet = 0; SelectedSecondInlet = 0; SelectedSecondOutlet = 0; SelectedFirstOutlet = 10; g_oob = 0; DO_Assign();
    eqi("VM 20 valves, outlet 10 -> legacy 19 is past the 19-slot map", g_oob, 1);
    eqi("VM 20 valves, outlet 10 -> no output (A4.8)", ones(InitialDO, 20), 0);

    /* ---- DO_AssignMUX / DO_ControlMUX ---- */
    memset(SingularInletActivate, 0, sizeof SingularInletActivate);
    DOSingularInlet[0] = 1; DOSingularInlet[1] = 0; DOSingularInlet[2] = 20; DOPurgeInlet = 20;
    SingularInletActivate[0] = true; SingularInletActivate[1] = true; SingularInletActivate[2] = true;
    PurgeInletActivate = false; PurgeInletActivateDI = false; PurgeInletActivateModbus = true;
    DO_AssignMUX();
    eqi("MUX inlet 0 on DO 1 -> index 0", InitialDOMUX[0], 1);
    eqi("MUX inlet 1 with DO 0 (none) -> nothing, state still 1", SingularInletState[1], 1);
    eqi("MUX inlet 2 and purge both on DO 20 -> index 19", InitialDOMUX[19], 1);
    eqi("MUX two outputs on", ones(InitialDOMUX, 20), 2);
    eqi("MUX purge state", PurgeInletState, 1);
    eqi("MUX ActiveInletIdx always -1", ActiveInletIdx, -1);
    memset(DOListMUX, 0, sizeof DOListMUX); DOListMUX[5] = true; DO_ControlMUX();
    eqi("MUX final = DI list OR assigned", ones(DOFinalMUX, 20), 3);

    /* ---- DO_Control (VM) ---- */
    memset(DOFinal, 0, sizeof DOFinal); DOFinal[3] = true;
    DO_Control();   /* DO_Assign runs, TwoORFb_22 stub leaves DOFinal */
    eqi("VM output 3 from DOFinal", TA30_DO[3], 1);
    eqi("VM output 0 from DOFinalMUX", TA30_DO[0], 1);
    eqi("VM UID output 9 from DOFinalMUX[19]", UID_DO[9], 1);
    FlushValveBit = false; FlushValveBitModbus = true; DO_Control();
    eqi("Flush state 1 from Modbus", FlushValveState, 1);
    FlushValveBitModbus = false; DO_Control();
    eqi("Flush state 0", FlushValveState, 0);

    /* ---- Analog_signals ---- */
    FirstCalibPoint[0] = 0.0f; SecondCalibPoint[0] = 100.0f;   /* HMI: Low 0, High 100 */
    TA30_AI[0] = 0; TA30_AI_Status[0] = 0; Analog_signals();
    eqf("AI raw 0 gives SecondCalibPoint (back to front, item 9)", AnalogSensor[0], 100.0);
    TA30_AI[0] = 16383; Analog_signals();
    eqf("AI raw 16383 gives FirstCalibPoint", AnalogSensor[0], 0.0);
    TA30_AI[0] = 8192; Analog_signals();
    eqf("AI raw 8192 ~ midpoint", AnalogSensor[0], 49.997);
    TA30_AI_Status[0] = 2; Analog_signals();
    eqf("AI status 2 -> SecondCalibPoint", AnalogSensor[0], 100.0);
    TA30_TEMP[1] = 215; TemperatureShift[1] = -5; Analog_signals();
    eqi("Temperature = raw + shift", TemperatureSensor[1], 210);
    AnalogEnable[0] = true; AnalogAlarmEnabled[0] = true; AnalogAlarmEmailEnabled[0] = true; ParameterColNumberLoggingArray[0] = true; Analog_signals();
    eqi("Suppression = NOT alarm enabled", AnalogSuppression[0], 0);
    AnalogEnable[0] = false; Analog_signals();
    eqi("AI disabled: alarm enable cleared", AnalogAlarmEnabled[0], 0);
    eqi("AI disabled: email cleared", AnalogAlarmEmailEnabled[0], 0);
    eqi("AI disabled: logging cleared", ParameterColNumberLoggingArray[0], 0);
    eqi("AI disabled: suppression on", AnalogSuppression[0], 1);
    TemperatureEnable[0] = true; TemperatureAlarmEnabled[0] = true; TemperatureAlarmEmailEnabled[0] = true; Analog_signals();
    TemperatureAlarmEnabled[0] = false; Analog_signals();
    eqi("Temp alarm disabled -> email cleared, enable kept", TemperatureAlarmEmailEnabled[0] == 0 && TemperatureEnable[0], 1);

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
