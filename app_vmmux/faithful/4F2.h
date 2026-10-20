/* 4F2.h - Stage 4F part 2 (VM schedule run): ScheduleMode, ScheduleModeSTValveAssign.
 * Types and globals as in 20_Data_Dictionary (Rev B) and 21_Types.h. */
#ifndef STAGE4F2_H
#define STAGE4F2_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t Preset, Current; bool Out, On; } ul_timer_t;
typedef struct { uint16_t From_Year, To_Year; bool Month[12], Day_Of_Month[31], Day_Of_Week[7];
                 uint8_t From_Hour[3], To_Hour[3]; uint8_t spare[4]; } Schedule_t;   /* [0]=Seconds [1]=Minutes [2]=Hours */
typedef struct { uint8_t NumberArray[20]; char Name[20][13]; uint32_t Timers[20];
                 uint8_t Inlets[20], Outlets[20]; uint8_t NumberOfPeriods; } Sequences_t;   /* Name1..Name20 as one array */
typedef struct { uint16_t Year; uint8_t Month, Day_In_Month, Day_Of_Week; } rtc_date_t;
typedef struct { uint8_t Hours, Minutes, Seconds; } rtc_time_t;

extern uint32_t g_dt_ms; extern int g_oob;
extern rtc_date_t RTC_Date; extern rtc_time_t RTC_Time;

extern bool StartSchedule, YouHaveIncorrectInletOutletConfig, SequenceAbsent;
extern uint8_t ScheduleButtonState;
extern int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;
extern int16_t InletIndex1, OutletIndex1, InletIndex2, OutletIndex2;
extern Sequences_t Sequence1, Sequence2;
extern Schedule_t Schedule1, Schedule2;
extern bool StartInterconnection1Periods, StartInterconnection2Periods;
extern bool StartInterconnectionSchedule1, StartInterconnectionSchedule2, Temp2, Temp3;
extern int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
extern ul_timer_t T1[20], T2[20];          /* T1[k-1] = Sequence1Timer<k>, T2[k-1] = Sequence2Timer<k> */
extern bool Stop1[19], Stop2[19];          /* Stop1[k-1] = Sequence1Stop<k>, k = 1..19 */
extern ul_timer_t ProcessTimer, ProcessTimer2;

void ScheduleMode(void);
void ScheduleModeSTValveAssign(void);
#endif
