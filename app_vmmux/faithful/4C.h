/* 4C.h - Stage 4C I/O layer: globals (types/sizes from 20_Data_Dictionary) and prototypes. */
#ifndef STAGE4C_H
#define STAGE4C_H
#include "vm_types.h"   /* shared types, system tags and vm_config.h switches (app_vmmux) */
#include <stdint.h>
#include <stdbool.h>

/* ---- physical I/O images (UniLogic hardware tags; new names per 10_IO_Map) ---- */
extern bool    TA30_DI[14];          /* On_board_IO_1.Digital_Inputs[0..13]   -> DIG_IN_01..14 */
extern bool    UID_DI[16];           /* UID_W1616R_0.Inputs[0..15] (0..5 used)-> DIG_IN_15..20 */
extern bool    TA30_DO[10];          /* On_board_IO_1.Digital_Outputs[0..9]   */
extern bool    UID_DO[16];           /* UID_W1616R_0.Outputs[0..9] used       */
extern int16_t TA30_AI[2];           /* On_board_IO_1.Analog_Inputs[0..1] (raw 0..16383 assumed, see 10_IO_Map) */
extern int16_t TA30_AI_Status[2];    /* On_board_IO_1.Status_Analog_Inputs[0..1] */
extern int16_t TA30_TEMP[2];         /* On_board_IO_1.Temperature_Inputs[0..1] */

/* ---- DI ---- */
extern bool    AdditionalDiscreteModule, DiscreteInputsArray[22], TempBoolsDI[14], TempBoolsDI1[16];
extern uint8_t Di_FunctionVM[22], DI_FunctionMUX[22], AlarmConditionVM[22], AlarmConditionMUX[22], FeedbackConditionMUX[22];
extern bool    AlarmsVM[22], AlarmEnableVM[22], EmailEnableVM[22], EnableVisibleVM[22];
extern bool    AlarmsMUX[22], AlarmEnableMUX[22], EmailEnableMUX[22], EnableVisibleMUX[22], EmailVisibMUX[22], FeedbackStateAlarm[22];
extern bool    FBAlarmsMUX[20], FBEmailEnableMUX[20];
extern bool    FlushValveBit, FlushValveBitModbus, PurgeInletActivateDI, AnalyzerOkBit, AnalyzerFunctionIsActive, AnalyzerOkBitAlarm, StopSeqBit;
extern bool    AlarmDescriptionEntered[22], AlarmDescriptionEnteredMUX[22];
extern int8_t  AlarmStatus[22], AlarmStatusMUX[22];
/* ---- DO ---- */
extern bool    DOList[20], InitialDO[20], DOFinal[20], DOListMUX[20], InitialDOMUX[20], DOFinalMUX[20];
extern int16_t DOInlet[19], DOOutlet[19], InletSlotByLegacyDO[19], OutletSlotByLegacyDO[19];
extern int16_t FirstInlets, FirstOutlets, SecondInlets, SecondOutlets, NumInterconnections;
extern int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
extern int16_t ActiveInletIdx, DOSingularInlet[19], DOPurgeInlet, SingularInletState[19], PurgeInletState;
extern bool    SingularInletActivate[19], PrevSingularInletActivate[19], PurgeInletActivate, PurgeInletActivateModbus;
extern bool    BackFlushExists, BackFlushOptionVisib, BackFlushValveAvailable;
extern uint8_t FlushValveState;
/* ---- analog / temperature ---- */
extern float   AnalogSensor[4], FirstCalibPoint[4], SecondCalibPoint[4];
extern bool    AnalogEnable[4], AnalogAlarmEnabled[4], AnalogAlarmEmailEnabled[4], AnalogAlarmDescriptionEntered[4], AnalogSuppression[4];
extern int8_t  AnalogAlarmStatus[4], TemperatureAlarmStatus[4];
extern int16_t TemperatureSensor[4], TemperatureShift[4];
extern bool    TemperatureEnable[4], TemperatureAlarmEnabled[4], TemperatureAlarmEmailEnabled[4], TemperatureAlarmDescriptionEntered[4], TemperatureAlarmSuppression[4];
extern bool    ParameterColNumberLoggingArray[20];

/* test aids */
extern int g_oob;                    /* ST array accesses outside bounds (skipped, A4.1 / A4.8) */
extern int g_alarm_desc_calls[64];   /* ModifyAlarmProperty calls per alarm ID */
extern int g_twoorfb22_calls;

void DI_Assigning(void);   void DI_Control(void);
void DI_AssigningMUX(void); void DI_ControlMUX(void);
void DO_Control(void);     void DO_Assign(void);
void DO_AssignMUX(void);   void DO_ControlMUX(void);
void Analog_signals(void);
void TwoORFb_22(void);     /* specified in 4F; stubbed here */
#endif
