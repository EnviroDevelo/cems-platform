/* test_4F2.c - tests for 4F2_faithful.c. Build: gcc -std=c11 -Wall -Wextra test_4F2.c 4F2_faithful.c
 * (add -DENABLE_DISABLED_RUNGS for the greyed rungs; the expected values are the same, see V11) */
#include <stdio.h>
#include <string.h>
#include "4F2.h"

uint32_t g_dt_ms = 100; int g_oob; rtc_date_t RTC_Date; rtc_time_t RTC_Time;
bool StartSchedule, YouHaveIncorrectInletOutletConfig, SequenceAbsent; uint8_t ScheduleButtonState;
int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;
int16_t InletIndex1, OutletIndex1, InletIndex2, OutletIndex2;
Sequences_t Sequence1, Sequence2; Schedule_t Schedule1, Schedule2;
bool StartInterconnection1Periods, StartInterconnection2Periods, StartInterconnectionSchedule1, StartInterconnectionSchedule2, Temp2, Temp3;
int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
ul_timer_t T1[20], T2[20], ProcessTimer = {1000, 1000, false, false}, ProcessTimer2 = {1000, 1000, false, false};
bool Stop1[19], Stop2[19];

static int fails, checks;
static void eqi(const char *w, long g, long x) { checks++; if (g != x) { fails++; printf("FAIL %-68s got %ld want %ld\n", w, g, x); } }
static uint32_t ms_acc;
static void tick(void) { ms_acc += g_dt_ms; while (ms_acc >= 1000) { ms_acc -= 1000;
    if (++RTC_Time.Seconds == 60) { RTC_Time.Seconds = 0; if (++RTC_Time.Minutes == 60) { RTC_Time.Minutes = 0; RTC_Time.Hours++; } } } }
static void scan(int n) { for (int i = 0; i < n; i++) { ScheduleMode(); tick(); } }
static void window(Schedule_t *s) { memset(s, 0, sizeof *s); s->From_Hour[2] = 6; s->To_Hour[2] = 6; s->To_Hour[1] = 5; }   /* 06:00-06:05 any day */
static void seq(Sequences_t *S, int n, uint32_t ms) { memset(S, 0, sizeof *S); S->NumberOfPeriods = (uint8_t)n;
    for (int i = 0; i < n; i++) { S->NumberArray[i] = (uint8_t)(i + 1); S->Timers[i] = ms; S->Inlets[i] = (uint8_t)(i % 3 + 1); S->Outlets[i] = (uint8_t)(i % 2 + 1); } }
static void reset(void) {
    StartSchedule = YouHaveIncorrectInletOutletConfig = SequenceAbsent = false;
    StartInterconnection1Periods = StartInterconnection2Periods = false; ScheduleMode();   /* clear edge memories */
    memset(T1, 0, sizeof T1); memset(T2, 0, sizeof T2); SelectedFirstInlet = SelectedFirstOutlet = SelectedSecondInlet = SelectedSecondOutlet = 0;
    RTC_Date = (rtc_date_t){2026, 10, 18, 1}; RTC_Time = (rtc_time_t){6, 0, 30}; ms_acc = 0;
    NumInterconnections = 1; FirstInlets = FirstOutlets = 3; SecondInlets = SecondOutlets = 3;
    window(&Schedule1); window(&Schedule2); }

int main(void) {
    /* ---------- 1. a 3-period run on interconnection 1 ---------- */
    reset(); seq(&Sequence1, 3, 10000); StartSchedule = true;
    scan(1);
    eqi("run: starts inside the window", StartInterconnection1Periods, 1);
    eqi("run: period 1 selects row 0 (inlet 1, outlet 1)", SelectedFirstInlet * 10 + SelectedFirstOutlet, 11);
    scan(100);
    eqi("run: period 2 selects row 1 (inlet 2, outlet 2)", SelectedFirstInlet * 10 + SelectedFirstOutlet, 22);
    eqi("run: remaining time shown for period 2", ProcessTimer.Preset <= 10000 && ProcessTimer.Preset >= 9800, 1);
    long end_scan = -1, restart_scan = -1; bool was = true;
    for (int s = 0; s < 400; s++) { scan(1); bool p = StartInterconnection1Periods;
        if (was && !p && end_scan < 0) end_scan = s;
        if (!was && p && end_scan >= 0 && restart_scan < 0) restart_scan = s;
        was = p; }
    eqi("run: ends after 3 x 10 s (A4.10: 1 scan early per timer)", 101 + end_scan, 300 - 3);   /* 0-based scan index */
    eqi("V9 still inside the window: starts again on the next scan", restart_scan, end_scan + 1);
    eqi("run: no index out of range", g_oob, 0);
    StartSchedule = false; scan(1);
    eqi("run: switching the schedule off clears the selection", SelectedFirstInlet + SelectedFirstOutlet, 0);
    eqi("run: ... and ends the run", StartInterconnection1Periods, 0);

    /* ---------- 2. interconnection 2, period 9 compares with 8 (finding V7) ---------- */
    reset(); NumInterconnections = 2; seq(&Sequence1, 1, 600000); seq(&Sequence2, 10, 1000); StartSchedule = true;
    scan(9 * 10 + 50);                                               /* well past period 9 */
    eqi("V7 interconnection 2 with 10 periods: still running after 14 s", StartInterconnection2Periods, 1);
    eqi("V7 ... stuck on row 8 (period 9), timer 9 never started", T2[8].On, 0);
    eqi("V7 ... and keeps row 8's inlet selected (row 8 inlet = 3)", SelectedSecondInlet, 3);
    reset(); NumInterconnections = 2; seq(&Sequence1, 1, 600000); seq(&Sequence2, 8, 1000); StartSchedule = true;
    { int ends = 0; bool w = false; for (int i = 0; i < 85; i++) { scan(1); if (w && !StartInterconnection2Periods) ends++; w = StartInterconnection2Periods; }
      eqi("V7 with 8 periods interconnection 2 ends normally (once in 85 scans)", ends, 1); }
    reset(); NumInterconnections = 2; seq(&Sequence1, 10, 1000); seq(&Sequence2, 1, 600000); StartSchedule = true;
    { int ends = 0; bool w = false; for (int i = 0; i < 105; i++) { scan(1); if (w && !StartInterconnection1Periods) ends++; w = StartInterconnection1Periods; }
      eqi("interconnection 1 with 10 periods ends normally (once in 105 scans)", ends, 1); }

    /* ---------- 3. empty sequences (finding V8) ---------- */
    reset(); NumInterconnections = 1; memset(&Sequence1, 0, sizeof Sequence1); StartSchedule = true; scan(1);
    eqi("one interconnection, empty: SequenceAbsent", SequenceAbsent, 1);
    eqi("... schedule switched off", StartSchedule, 0);
    reset(); NumInterconnections = 2; memset(&Sequence1, 0, sizeof Sequence1); seq(&Sequence2, 2, 1000); StartSchedule = true;
    int starts = 0; bool pw = false;
    for (int s = 0; s < 50; s++) { scan(1); if (StartInterconnection1Periods && !pw) starts++; pw = StartInterconnection1Periods; }
    eqi("V8 two interconnections, only #1 empty: not flagged", SequenceAbsent, 0);
    eqi("V8 ... interconnection 1 never holds a run (ends the scan it starts)", starts, 0);
    eqi("V8 ... interconnection 2 runs", T2[0].Out, 1);
    reset(); NumInterconnections = 2; memset(&Sequence1, 0, sizeof Sequence1); memset(&Sequence2, 0, sizeof Sequence2); StartSchedule = true; scan(1);
    eqi("V8 both empty: SequenceAbsent", SequenceAbsent, 1);
    reset(); NumInterconnections = 2; seq(&Sequence1, 2, 1000); memset(&Sequence2, 0, sizeof Sequence2); StartSchedule = true; scan(1);
    eqi("V8 only #2 empty: not flagged either", SequenceAbsent, 0);

    /* ---------- 4. inlet number above the configured count (rung 1) ---------- */
    reset(); seq(&Sequence1, 2, 1000); Sequence1.Inlets[1] = 4; StartSchedule = true; scan(1);
    eqi("rung 1: inlet 4 with 3 inlets -> config error", YouHaveIncorrectInletOutletConfig, 1);
    eqi("rung 1: ... schedule off", StartSchedule, 0);
    reset(); seq(&Sequence1, 2, 1000); Sequence1.Inlets[5] = 9; StartSchedule = true; scan(1);
    eqi("rung 1: an unused row above the count is checked too", YouHaveIncorrectInletOutletConfig, 1);

    /* ---------- 5. outside the window ---------- */
    reset(); seq(&Sequence1, 2, 1000); RTC_Time = (rtc_time_t){7, 0, 0}; StartSchedule = true; scan(5);
    eqi("outside the window: no run", StartInterconnection1Periods, 0);

    /* ---------- 6. the remaining-time display is written when idle (finding V10) ---------- */
    reset(); seq(&Sequence1, 2, 1000); T1[0].Preset = 4321; T1[0].Current = 4321; ProcessTimer.Preset = 1234; scan(1);
    eqi("V10 schedule off: ProcessTimer.Preset still overwritten with timer 1", ProcessTimer.Preset, 4321);

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
