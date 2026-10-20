/* 4F4.h - Stage 4F part 4: Interconnection settings and Function1 (the VM top level).
 * Uses the 4B globals and functions (AdjustNumberOfValves, TubeVisibility, ResetDOassigningOver10) as they are.
 * Every other function Function1 calls is declared here and supplied by the test as a recording stub, so the
 * test can check what Function1 itself does and in what order. */
#ifndef STAGE4F4_H
#define STAGE4F4_H
#include "vm_types.h"   /* shared types and system tags (app_vmmux) */
#include "4B.h"
#include "4D.h"

extern uint32_t g_dt_ms;
extern bool Ladder_Initial_Cycle, Touched_Bit, VNC_Touched_Bit;     /* General.* system bits */

/* Interconnection settings */
extern bool MaxNumberEntered, NumInterconnectionsEntried, Tube1InletEntered, Tube1OutletEntered, Tube2InletEntered, Tube2OutletEntered;
extern bool OneInterconnectingTubeVisible, TwoInterconnectingTubeVisible, ResetSequences, ChangeModbusNamesVM, ChangeVisibility;
extern bool BackFlushOptionVisib, BackFlushOptionVisib20, BackFlushOptionVisib10;
extern int16_t MaxNumberOfValves, MaxValveDOnumber, ValveSum, ValueChanged, DOSingularInlet[19], DOPurgeInlet;
extern int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
extern bool EnableInletValve[19], EnableOutletValve[19];
extern uint8_t Di_FunctionVM[22], DI_FunctionMUX[22];

/* Function1 */
extern bool LoadSystemConfig, LoadSettings, Load1_5, Load1_9_2, Load1_19_2, LoadVMSettings, LoadMUXSettings;   /* action triggers (A4.15) */
extern bool LoadDOMappingValves1_5, LoadDOMappingValves1_9, LoadDOMappingValves1_19;                           /* action triggers (A4.15) */
extern bool ActivateScreenSaverBit, DeactivateScreenSaverBit, SetBrightness;                                   /* action triggers (A4.15) */
extern uint8_t NumberOfDInputs; extern bool AdditionalDiscreteModule, WithoutModules;
extern bool CheckEmptySettingsBack, AnalogOrDiscrete, LoadDOMappingValvesCheckEmpty;
extern int16_t TempInlets, TempOutlets;
extern bool DOInletVisibility20[18], DOOutletVisibility20[18], DOInletVisibility10[18], DOOutletVisibility10[18];
extern bool OneManualTimer, TwoManualTimers, ManualModeStart, ManualModeEnable, StartButtonVisib;
extern ul_timer_t ManualTimer, ManualTimer2, ProcessTimer, ProcessTimer2;   /* "Manual Timer", "Manual Timer2" */
extern bool InletValveTouch[19], OutletValveTouch[19], Temp1, temp;
extern bool OpenMain, StartModbus, StartSchedule, ResetManualAttempt, YouHaveIncorrectInletOutletConfig;
extern uint8_t DefaultModeOnPowerup, ModbusModeState, ScheduleButtonState;
extern int16_t InletIndex1, OutletIndex1, InletIndex2, OutletIndex2, CurrentMode, VMorMUXState;
extern bool ModbusModeEnable, SchedulerModeEnable, TimerVisible, TimerVisible1;
extern bool LoadVMorMUX, ScreenSaverActive, BrightessEntered, ModeIsActive, PauseMUX;
extern ul_timer_t ActivateScreenSaver, YouCantLeave, SetsettingsBrightness;
extern char Password[17];
/* Sequence1 / Sequence2: Function1 reads NumberOfPeriods, Inlets, Outlets (4D.h; the Stage 4 test used a view) */

/* Callees from other chunks (stubs in the test) */
void Loading(void); void FillNumbers(void); void FB_ValveSelectionInter(void); void SchedulerSequencesVM(void);
void Route(void); void ModbusMode(void); void ScheduleMode(void); void ModbusValveNames(void); void DI_Assigning(void);
void Email(void); void DO_Control(void); void Analog_signals(void); void MUX(void);

extern bool vm_mux_enable;   /* app_vmmux: MUX feature enable (vm_mux.c) */
void vm_mux_off(void);       /* app_vmmux: the MUX's state while the feature is disabled (vm_mux.c) */

void Interconnection_settings(void);
void Function1(void);
#endif
