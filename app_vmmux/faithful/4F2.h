/* 4F2.h - Stage 4F part 2 (VM schedule run): ScheduleMode, ScheduleModeSTValveAssign.
 * Types and globals as in 20_Data_Dictionary (Rev B) and 21_Types.h. */
#ifndef STAGE4F2_H
#define STAGE4F2_H
#include "vm_types.h"   /* shared types and system tags (app_vmmux) */
#include <stdbool.h>
#include <stdint.h>


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
