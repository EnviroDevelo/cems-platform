/* 4F3.h - Stage 4F part 3 (VM sequence and schedule editor): SchedulerSequencesVM, Scheduler_SequencesScroll.
 * Types from 4D.h (Schedule_t, Sequences_t with Name1..Name20); the copies are 4D_faithful.c, linked as they are.
 * Globals as in 20_Data_Dictionary (Rev B). Arrays stand for numbered tags where noted. */
#ifndef STAGE4F3_H
#define STAGE4F3_H
#include "../4D/4D.h"

typedef struct { uint32_t Preset, Current; bool Out, On; } ul_timer_t;

extern int g_div0;                                  /* MOD by zero in the scroll ST (A4.16) */

/* screen commands and flags */
extern bool SchedulerInterconnection1Or2, OpenScheduleSequenceInterCon1Or2, OpenScheduleSequenceInterCon1Or2_1;
extern bool ScheduleSequencesInterconnectionSelected, ScheduleSequencesInterconnectionSelected1;
extern bool ScheduleInterconnectionSelected, LoadSecondInterconnectionSchedule, LoadingWindow, ContactManufacturer1, Ackbit1;
extern bool Loadinletlist, LoadOutlets, AppendStringToCSVList, AppendStringToCSVList1, LoadScheduleIntercon1_2;
extern bool SaveSchedule, SaveScheduleSequences, ResetSequences, NumberOfPeriodsEntered, ScrollUp, ScrollDown;
extern bool EntryRow[7];                            /* EntryRow1..7 */
extern bool ArrowButtonsVisib, ScheduleSequencesVisib[7];
extern bool SelectAllDays;                          /* tag "select all days" */
extern bool SelectAllDaysSeq1, SelectAllDaysSeq2, BlockForSelecAll, BlockNegativeDays;
/* project-level action triggers (screen loads); the OS resets them after the action (A4.15) */
extern bool LoadScheduleInterCon1, LoadScheduleInterCon2, LoadSequencesInterCon1, LoadSequencesInterCon2, LoadAlterInterconnectionScreen;

extern uint8_t ScheduleSequencesInterconnection, ScheduleInterconnection;   /* combo boxes, 0-based */
extern int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;
extern int16_t InletsDeleteStatus, OutletsDeleteStatus;
extern int8_t  CsvBufferStatus;
extern int16_t CurrentInletlistID, CurrentOutletlistID, TargetInletListID, TargetOutletListID;
extern int16_t TempInlets, TempOutlets, TempValveNumber, TempStringLength;
extern uint8_t TempLength;                          /* power-up 2 */
extern char    TempString[11], TempStringASCII[9];
extern uint8_t TempBuffer[24], UserDelim[4];
extern uint8_t InletMaxHMI, OutletsMaxHMI;
extern char    InletName[19][11], OutletName[19][11];   /* InletName0..18, OutletName0..18 */

/* the 7 editor rows */
extern int16_t scrollIndex;
extern uint8_t Number1_7[7], Inlet[7], Outlet[7];
extern ul_timer_t Timer1_7[7];                      /* Timer1..Timer7 (only .Preset used) */
extern char    Name1_7[7][13];                      /* Name1..Name7 */

/* file model for the two CSV lists (scheduleinlets / scheduleoutlets) */
extern char    g_inlets_csv[24][24], g_outlets_csv[24][24];
extern int     g_inlets_lines, g_outlets_lines;

void SchedulerSequencesVM(void);
void Scheduler_SequencesScroll(void);
#endif
