/* 4F1.h - Stage 4F part 1 (VM small blocks): TwoORFb_22, TwoORFb_10, ScheduleCSVAdds, Loading, Route,
 * FB_ValveSelectionInter. Types and globals as in 20_Data_Dictionary (Rev B). */
#ifndef STAGE4F1_H
#define STAGE4F1_H
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t Preset, Current; bool Out, On; } ul_timer_t;
extern uint32_t g_dt_ms;           /* scan time used by the timer model */
extern int g_oob;                  /* out-of-range index count (A4.1 / A4.8) */

/* TwoORFb_22 / TwoORFb_10 */
extern bool DOList[20], InitialDO[20], DOFinal[20];
extern bool BackFlushExists, BackFlushOptionVisib, FlushValveBit, FlushValveBitModbus;
extern int16_t BackFlushDONumber;
/* ScheduleCSVAdds */
extern uint8_t UserDelim[4], TempBuffer[24];
extern int16_t TempStringLength;
/* Loading */
extern bool LoadingWindow, BitOne, BitTwo, BitThree;
extern ul_timer_t LoadingTimer;    /* Preset 2000 at power-up */
/* Route, FB_ValveSelectionInter */
extern int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;
extern int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
extern uint8_t TubeColor1[57], TubeColor2[55];
extern bool InletValveTouch[19], OutletValveTouch[19], EnableInletValve[19], EnableOutletValve[19];

void TwoORFb_22(void);
void TwoORFb_10(bool DOOut[10]);   /* NEVER CALLED IN ORIGINAL; its 10 outputs DOOut0..9 are returned in DOOut[] */
void ScheduleCSVAdds(void);
void Loading(void);
void Route(void);
void FB_ValveSelectionInter(void);
#endif
