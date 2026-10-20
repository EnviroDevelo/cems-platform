/* 4E.h - Stage 4E MUX scheduling: types, globals (20_Data_Dictionary) and prototypes.
 * Timer tags SequenceMUXDwellTimer1..20 / SequenceMUXSampleTimer1..20 are held as arrays
 * DwellT[0..19] / SampleT[0..19] (DwellT[k-1] = SequenceMUXDwellTimer<k>). */
#ifndef STAGE4E_H
#define STAGE4E_H
#include <stdint.h>
#include <stdbool.h>

typedef struct { uint32_t Preset, Current; bool Out, On; } ul_timer_t;   /* 11 section 4: ms, Current counts down */
typedef struct {
    uint16_t From_Year, To_Year; bool Month[12]; bool Day_Of_Month[31]; uint8_t Spare1;
    bool Day_Of_Week[7]; uint8_t Spare2; uint8_t From_Hour[3]; uint8_t Spare3; uint8_t To_Hour[3]; uint8_t Spare4;
} Schedule_t;                                   /* From_Hour/To_Hour = [Seconds=0, Minutes=1, Hours=2] (project DB) */
typedef struct {
    uint8_t NumberArray[20]; uint8_t SingulValves[20]; uint32_t DwellTimer[20]; uint32_t SampleTimer[20];
    bool FastLoopPump[20]; uint32_t FastLoopPumpTimer[20]; uint8_t NumberOfPeriods;
} ScheduleMUX_t;
typedef struct { uint8_t Day_Of_Week, Day_In_Month, Month; uint16_t Year; } rtc_date_t;   /* RTC_Date members used */
typedef struct { uint8_t Seconds, Minutes, Hours; } rtc_time_t;                          /* RTC_Time members used */

/* ---- clock and scan (test harness) ---- */
extern rtc_date_t RTC_Date; extern rtc_time_t RTC_Time;
extern uint32_t   g_dt_ms;              /* scan time used by the timer models */
extern bool       Ladder_Initial_Cycle; /* General.Ladder_Initial_Cycle */
extern int        g_oob;                /* out-of-bounds accesses (A4.1 / A4.8) */

/* ---- sequence data ---- */
extern ScheduleMUX_t SequenceMUX, HMISequenceMUX;
extern Schedule_t    ScheduleMUX;                         /* the global tag of type Schedule */
extern int16_t  NumberOfMUXValves, InletIndexMUX;

/* ---- run state ---- */
extern bool StartScheduleMUX, StartPeriodsMUX, StartMUXSchedule1, Temp3MUX, SequenceAbsentMUX, YouHaveIncorrectInletOutletConfigMUX;
extern uint8_t ScheduleButtonStateMUX;
extern bool RecalculateNextScheduleMUX, SequenceStopMUX[19];
extern bool EnableLockValvesIfAnalyzerNotOk, AnalyzerOkBitAlarm, StopSeqBit, PauseMUX;
extern ul_timer_t Every30minutes, DwellT[20], SampleT[20], FastLoopT[20] /* SequenceMUXFastLoopTimer1..20, disabled rung 7 only */;
extern ul_timer_t ProcessTimerMUX;
extern uint32_t CurrentUnix, FastLoopStartUnix, NextSampleStartUnix, FastLoopStartTime[19];
extern bool PumpCmd, PumpCmd2 /* disabled rung 10 only */, SingularInletActivate[19];
extern int16_t DwellSampleStep;
extern bool Sequence1TimerOut[20];                        /* Sequence1Timer1..20.Out, read by disabled rungs 15-20 only */

/* ---- editor ---- */
extern bool OpenScheduleSequenceMUX, OpenScheduleSequenceMUX1, LoadinletlistMUX, AppendStringToCSVListMUX, LoadSequence;
extern bool ContactManufacturer1, LoadingWindow, EntryTimerBitMUX, SaveScheduleSequencesMUX, ResetSequencesMUX;
extern bool NumberOfPeriodsEnteredMUX, ArrowButtonsVisib, ScheduleSequencesVisib[7];
extern int16_t InletsDeleteStatus, CsvBufferStatus, CurrentInletlistID, TargetInletListID;
extern char SingularInlet[19][21];                        /* SingularInlet0..18 (valve names) */
extern bool BlockNegativeDaysMUX, SelectAllDaysMUX, BlockForSelecAllMUX;
extern int16_t scrollIndexMUX;
extern uint8_t Number1_7MUX[7], InletMUX[7]; extern bool FastLoopPumpCheck[7];
extern ul_timer_t Timer1_7[7], SampleTimer1_7[7], FastLoopTimer1_7[7];   /* Timer1..7, SampleTimer1..7, FastLoopTimer1..7 */
extern bool ScrollUpMUX, ScrollDownMUX, EntryRowMUX[7];
/* model of the file the combo box reads: /UserLogs/Csv/scheduleinlets.csv */
extern char g_csv[24][24]; extern int g_csv_lines;

/* ---- top level ---- */
extern bool SingularInletVisib1[19], SingularInletVisib2[19];
extern bool NumberOfMUXVavlesEntered, ManualModeStartMUX, StartModbusMUX, EnableSingulManualSelection;
extern bool ModbusModeEnableMUX, SchedulerModeEnableMUX, ManualModeEnableMUX, ModeIsActive;
extern uint8_t CurrentModeMUX;
extern ul_timer_t ManualTimerMUX, FlowValidationTimer;    /* "Manual TimerMUX" */
extern bool FlowValidation, ValidationTime, FlowValidationAlarm, PurgeInletActivate, PurgeInletActivateDI, PurgeInletActivateModbus;
extern float AnalogSensor[4], AnalogTempContainer, AnalogTempContainerPGap, AnalogTempContainerMGap, Gap;
extern uint8_t FlowAnalogInput;

/* ---- functions ---- */
void ScheduleCalculate1stFastLoopPump(void);
void ScheduleFastLoopTime(void);
void ScheduleTimersAndPump(void);
void ScheduleModeSTValveAssignMUX(void);
void ScheduleModeMUX(void);
void SchedulerSequencesMUX(void);
void Scheduler_SequencesScrollMUX(void);
void MUX(void);
void treset(ul_timer_t *t);                 /* TimerReset block, used by the tests */
/* called functions specified elsewhere; stubs in the test (call counters) */
void SearchInArray_u8(const uint8_t *a, int16_t maxv, int16_t *out);   /* 41f */
void CopySequenceToHMI(void); void CopySequenceFromHMI(void);         /* 44 */
void FillNumbersMUX(void); void AdjustNumberOfValvesMUX(void);         /* 41g, 42b */
void DI_AssigningMUX(void); void ModbusModeMUX(void); void ModbusValveNamesMUX(void);
void DO_AssignMUX(void); void DO_ControlMUX(void); void Logging(void); void LoadTable(void);
void ScheduleCSVAdds_model(void);                                       /* 4F; adds CR LF to TempBuffer */
#endif
