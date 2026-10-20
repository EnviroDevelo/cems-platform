/* 4G.h - Stage 4G Modbus: VM ModbusMode and ModbusValveNames, MUX ModbusModeMUX and ModbusValveSelection
 * (MUX Modbus and ModbusValveNamesMUX are empty functions). Standalone: the globals the functions touch are
 * declared here and defined in 4G_faithful.c. Links with ../4A/4A_faithful.c (ConvertIntToString). */
#ifndef STAGE4G_H
#define STAGE4G_H
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint32_t Preset, Current; bool Out, On; } ul_timer_t;   /* 11 section 4: ms, Current counts down */
extern uint32_t g_dt_ms;

/* ---- VM configuration (written by Interconnection settings, 46h) ---- */
extern int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;

/* ---- VM ModbusMode ---- */
extern bool StartModbus, StartModbusCoil, ModbusModeNotActive, ValveSelectionErrorModbus, ResetMbError, FlushValveBitModbus;
extern int16_t SelectedFirstInletMb, SelectedFirstOutletMb, SelectedSecondInletMb, SelectedSecondOutletMb;   /* master writes */
extern int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
extern uint8_t ModbusModeState;
extern ul_timer_t ModbusTimer, ModbusTimer2, ProcessTimer, ProcessTimer2;   /* ModbusTimer RAM, ModbusTimer2 FRAM (2 h) */

/* ---- VM ModbusValveNames ---- */
extern bool ChangeModbusNamesVM, LoadingWindow;
extern bool ChangeModbusNamesVMFirstInlets, ChangeModbusNamesVMFirstOutlets, ChangeModbusNamesVMSecondInlets, ChangeModbusNamesVMSecondOutlets;
extern char InletName[19][11], OutletName[19][11];                 /* InletName0..18, OutletName0..18 (STRING-ASCII 10) */
extern uint8_t FirstInterconInletName[19][20], FirstInterconOutletName[19][20];    /* ...Name1..19 = [0..18] */
extern uint8_t SecondInterconInletName[18][20], SecondInterconOutletName[18][20];  /* ...Name1..18 = [0..17] */
extern int16_t CurrentInletlistID, CurrentOutletlistID, TargetInletListID, TargetOutletListID;
extern int16_t CurrentInletlistID2Inter, CurrentOutletlistID2Interc, TargetInletListID2Interc, TargetOutletListID2Interc;
extern int16_t TempInletsModbus, TempOutletsModbus, TempStringLength, TempValveNumber, TempValveNumber2Interc, TempValveNumber2IntercTemp;
extern uint8_t TempLength;                                         /* constant 2 (never written) */
extern char TempString[11], TempStringASCII[9];
extern uint8_t TempBuffer1[20];
/* A4.17 (open, 11 6.15): how many bytes Tag To Buffer copies from a string tag. false (default) = the
 * characters only; true = the whole declared tag (11 or 9 bytes), NUL padded. */
extern bool g_tag2buf_whole;

/* ---- MUX ---- */
extern int16_t NumberOfMUXValves;
extern bool StartModbusMUX, StartModbusCoilMUX, ModbusNotActiveMUX, ValveSelectionErrorModbusMUX, ResetMbErrorMUX, PurgeInletActivateModbus;
extern int16_t SelectedValveMbMUX, TempSelectedModbusValveMUX;   /* 1-based from the master, 0-based internally */
extern uint8_t ModbusModeStateMUX;
extern ul_timer_t ModbusTimerMUX, ProcessTimerMUX;                /* ModbusTimerMUX is a Timer Accumulated, RAM */
extern bool EnableLockValvesIfAnalyzerNotOk, AnalyzerOkBitAlarm, StopSeqBit, PauseMUX;
extern bool SingularInletActivate[19];

void ModbusMode(void);
void ModbusValveNames(void);
void ModbusModeMUX(void);
void ModbusValveSelection(void);
void Modbus(void);               /* MUX, 5 empty rungs */
void ModbusValveNamesMUX(void);  /* MUX, 5 empty rungs */
void treset(ul_timer_t *t);
#endif
