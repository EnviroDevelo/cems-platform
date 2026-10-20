/* test_4E.c - scan-by-scan tests for the Stage 4E MUX scheduling functions.
 * Build: gcc -std=c11 -Wall -Wextra test_4E.c 4E_faithful.c   (add -DENABLE_DISABLED_RUNGS to include them) */
#include <stdio.h>
#include <string.h>
#include "4E.h"

/* ---- globals ---- */
rtc_date_t RTC_Date; rtc_time_t RTC_Time; uint32_t g_dt_ms = 100; bool Ladder_Initial_Cycle; int g_oob;
ScheduleMUX_t SequenceMUX, HMISequenceMUX; Schedule_t ScheduleMUX; int16_t NumberOfMUXValves, InletIndexMUX;
bool StartScheduleMUX, StartPeriodsMUX, StartMUXSchedule1, Temp3MUX, SequenceAbsentMUX, YouHaveIncorrectInletOutletConfigMUX;
uint8_t ScheduleButtonStateMUX; bool RecalculateNextScheduleMUX, SequenceStopMUX[19];
bool EnableLockValvesIfAnalyzerNotOk, AnalyzerOkBitAlarm, StopSeqBit, PauseMUX;
ul_timer_t Every30minutes, DwellT[20], SampleT[20], FastLoopT[20], ProcessTimerMUX;
uint32_t CurrentUnix, FastLoopStartUnix, NextSampleStartUnix, FastLoopStartTime[19];
bool PumpCmd, PumpCmd2, SingularInletActivate[19]; int16_t DwellSampleStep; bool Sequence1TimerOut[20];
bool OpenScheduleSequenceMUX, OpenScheduleSequenceMUX1, LoadinletlistMUX, AppendStringToCSVListMUX, LoadSequence;
bool ContactManufacturer1, LoadingWindow, EntryTimerBitMUX, SaveScheduleSequencesMUX, ResetSequencesMUX;
bool NumberOfPeriodsEnteredMUX, ArrowButtonsVisib, ScheduleSequencesVisib[7];
int16_t InletsDeleteStatus, CsvBufferStatus, CurrentInletlistID, TargetInletListID; char SingularInlet[19][21];
bool BlockNegativeDaysMUX, SelectAllDaysMUX, BlockForSelecAllMUX; int16_t scrollIndexMUX;
uint8_t Number1_7MUX[7], InletMUX[7]; bool FastLoopPumpCheck[7]; ul_timer_t Timer1_7[7], SampleTimer1_7[7], FastLoopTimer1_7[7];
bool ScrollUpMUX, ScrollDownMUX, EntryRowMUX[7]; char g_csv[24][24]; int g_csv_lines;
bool SingularInletVisib1[19], SingularInletVisib2[19];
bool NumberOfMUXVavlesEntered, ManualModeStartMUX, StartModbusMUX, EnableSingulManualSelection;
bool ModbusModeEnableMUX, SchedulerModeEnableMUX, ManualModeEnableMUX, ModeIsActive; uint8_t CurrentModeMUX;
ul_timer_t ManualTimerMUX, FlowValidationTimer; bool FlowValidation, ValidationTime, FlowValidationAlarm, PurgeInletActivate, PurgeInletActivateDI, PurgeInletActivateModbus;
float AnalogSensor[4], AnalogTempContainer, AnalogTempContainerPGap, AnalogTempContainerMGap, Gap = 2.0f; uint8_t FlowAnalogInput;

/* ---- called functions: 41f and 44 as specified there, the rest counted ---- */
void SearchInArray_u8(const uint8_t *a, int16_t maxv, int16_t *out) { *out = 25; for (int i = 0; i <= 19; i++) if ((int16_t)a[i] > maxv) { *out = (int16_t)i; break; } }
static void cpmux(ScheduleMUX_t *d, const ScheduleMUX_t *s) { int16_t n = s->NumberOfPeriods; d->NumberOfPeriods = s->NumberOfPeriods;
    for (int i = 0; i <= n - 1 && i < 20; i++) { d->NumberArray[i] = s->NumberArray[i]; d->SingulValves[i] = s->SingulValves[i]; d->DwellTimer[i] = s->DwellTimer[i];
        d->SampleTimer[i] = s->SampleTimer[i]; d->FastLoopPump[i] = s->FastLoopPump[i]; d->FastLoopPumpTimer[i] = s->FastLoopPumpTimer[i]; } }
void CopySequenceToHMI(void) { cpmux(&HMISequenceMUX, &SequenceMUX); }
void CopySequenceFromHMI(void) { cpmux(&SequenceMUX, &HMISequenceMUX); }
int n_fill, n_adjust; void FillNumbersMUX(void) { n_fill++; } void AdjustNumberOfValvesMUX(void) { n_adjust++; }
void DI_AssigningMUX(void) {} void ModbusModeMUX(void) {} void ModbusValveNamesMUX(void) {}
void DO_AssignMUX(void) {} void DO_ControlMUX(void) {} void Logging(void) {} void LoadTable(void) {}

static int fails, checks;
static void eqi(const char *w, long g, long x) { checks++; if (g != x) { fails++; printf("FAIL %-62s got %ld want %ld\n", w, g, x); } }
static void eqs(const char *w, const char *g, const char *x) { checks++; if (strcmp(g, x)) { fails++; printf("FAIL %-62s got '%s' want '%s'\n", w, g, x); } }

static void set_clock(int y, int mo, int d, int dow, int h, int mi, int s) {
    RTC_Date.Year = (uint16_t)y; RTC_Date.Month = (uint8_t)mo; RTC_Date.Day_In_Month = (uint8_t)d; RTC_Date.Day_Of_Week = (uint8_t)dow;
    RTC_Time.Hours = (uint8_t)h; RTC_Time.Minutes = (uint8_t)mi; RTC_Time.Seconds = (uint8_t)s; }
static uint32_t ms_acc;
static void tick(void) { ms_acc += g_dt_ms; while (ms_acc >= 1000) { ms_acc -= 1000;
    if (++RTC_Time.Seconds == 60) { RTC_Time.Seconds = 0; if (++RTC_Time.Minutes == 60) { RTC_Time.Minutes = 0; RTC_Time.Hours++; } } } }
static uint32_t unix_of(int y, int mo, int d, int h, int mi, int s) {   /* reference, independent of the model */
    long days = 0; for (int yy = 1970; yy < y; yy++) days += ((yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0) ? 366 : 365;
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (int m = 1; m < mo; m++) days += dm[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0));
    days += d - 1; return (uint32_t)(days * 86400L + h * 3600L + mi * 60L + s); }

static void schedule_all_days(uint8_t h, uint8_t m, uint8_t s) {
    memset(&ScheduleMUX, 0, sizeof ScheduleMUX); ScheduleMUX.From_Year = 2026; ScheduleMUX.To_Year = 2030;
    for (int i = 0; i < 12; i++) ScheduleMUX.Month[i] = 1;
    for (int i = 0; i < 31; i++) ScheduleMUX.Day_Of_Month[i] = 1;
    for (int i = 0; i < 7; i++) ScheduleMUX.Day_Of_Week[i] = 1;
    ScheduleMUX.From_Hour[2] = h; ScheduleMUX.From_Hour[1] = m; ScheduleMUX.From_Hour[0] = s;   /* [Hours]=2, [Minutes]=1, [Seconds]=0 */
    ScheduleMUX.To_Hour[2] = h; ScheduleMUX.To_Hour[1] = (uint8_t)(m + 5); ScheduleMUX.To_Hour[0] = s; }
static void reset_run(void) {
    StartScheduleMUX = StartPeriodsMUX = StartMUXSchedule1 = PumpCmd = false; memset(SingularInletActivate, 0, sizeof SingularInletActivate);
    memset(DwellT, 0, sizeof DwellT); memset(SampleT, 0, sizeof SampleT); EnableLockValvesIfAnalyzerNotOk = AnalyzerOkBitAlarm = StopSeqBit = PauseMUX = false;
    YouHaveIncorrectInletOutletConfigMUX = false; Every30minutes.Preset = 1800000; ms_acc = 0; }
static void seq(int n, const uint8_t *valves, uint32_t dwell_ms, uint32_t sample_ms) {
    memset(&SequenceMUX, 0, sizeof SequenceMUX); SequenceMUX.NumberOfPeriods = (uint8_t)n;
    for (int i = 0; i < n; i++) { SequenceMUX.NumberArray[i] = (uint8_t)(i + 1); SequenceMUX.SingulValves[i] = valves[i];
        SequenceMUX.DwellTimer[i] = dwell_ms; SequenceMUX.SampleTimer[i] = sample_ms; } }

int main(void) {
    /* ---------- 1. first-start calculation (finding S1: hours and seconds swapped) ---------- */
    set_clock(2026, 10, 18, 1, 6, 0, 0);                        /* Sunday 18 Oct 2026 06:00:00 */
    schedule_all_days(7, 30, 0);                                /* From 07:30:00 */
    memset(&SequenceMUX, 0, sizeof SequenceMUX); SequenceMUX.FastLoopPumpTimer[0] = 120000;   /* 2 min lead */
    CurrentUnix = unix_of(2026, 10, 18, 6, 0, 0);
    ScheduleCalculate1stFastLoopPump();
    eqi("S1 next start = tomorrow 00:30:07 (reads [0] as hours, [2] as seconds)", (long)NextSampleStartUnix, (long)unix_of(2026, 10, 19, 0, 30, 7));
    eqi("S1 pre-pump start = next start - 120 s", (long)FastLoopStartUnix, (long)unix_of(2026, 10, 19, 0, 28, 7));
    schedule_all_days(0, 30, 7);                                /* the stored value that the code would read as 07:30:00 */
    ScheduleCalculate1stFastLoopPump();
    eqi("S1 a From of 00:30:07 is read as 07:30:00 today", (long)NextSampleStartUnix, (long)unix_of(2026, 10, 18, 7, 30, 0));
    /* S2: nothing ticked, or years 0/0 ("any" for the Schedule block) never matches here */
    schedule_all_days(7, 30, 0); memset(ScheduleMUX.Day_Of_Week, 0, 7); ScheduleCalculate1stFastLoopPump();
    eqi("S2 no weekday ticked: not found, next start 0", (long)NextSampleStartUnix, 0);
    schedule_all_days(7, 30, 0); ScheduleMUX.From_Year = ScheduleMUX.To_Year = 0; ScheduleCalculate1stFastLoopPump();
    eqi("S2 years 0/0: not found", (long)FastLoopStartUnix, 0);
    /* only Mondays: next Monday */
    schedule_all_days(0, 30, 7); memset(ScheduleMUX.Day_Of_Week, 0, 7); ScheduleMUX.Day_Of_Week[1] = 1; ScheduleCalculate1stFastLoopPump();
    eqi("Monday only (Day_Of_Week[1]) from Sunday: next day", (long)NextSampleStartUnix, (long)unix_of(2026, 10, 19, 7, 30, 0));
    /* 29 Feb: only day 29 ticked, starting 1 Feb 2027 -> 29 Feb 2028 */
    schedule_all_days(0, 0, 8); memset(ScheduleMUX.Day_Of_Month, 0, 31); ScheduleMUX.Day_Of_Month[28] = 1;
    memset(ScheduleMUX.Month, 0, 12); ScheduleMUX.Month[1] = 1;
    set_clock(2027, 2, 1, 2, 9, 0, 0); CurrentUnix = unix_of(2027, 2, 1, 9, 0, 0); ScheduleCalculate1stFastLoopPump();
    eqi("leap year: next 29 Feb is 2028", (long)NextSampleStartUnix, (long)unix_of(2028, 2, 29, 8, 0, 0));

    /* ---------- 2. pre-pump lead times ---------- */
    memset(&SequenceMUX, 0, sizeof SequenceMUX); SequenceMUX.NumberArray[1] = 2; SequenceMUX.SampleTimer[0] = 60000; SequenceMUX.FastLoopPumpTimer[1] = 20000;
    SequenceMUX.NumberArray[2] = 3; SequenceMUX.SampleTimer[1] = 10000; SequenceMUX.FastLoopPumpTimer[2] = 30000;
    ScheduleFastLoopTime();
    eqi("FastLoopStartTime[0] = Sample[0] - Lead[1]", (long)FastLoopStartTime[0], 40000);
    eqi("FastLoopStartTime[1] = 0 when the lead is longer", (long)FastLoopStartTime[1], 0);

    /* ---------- 3. a 3-period run, scan by scan ---------- */
    reset_run(); NumberOfMUXValves = 5;
    { const uint8_t v[] = {1, 2, 3}; seq(3, v, 10000, 60000); }
    SequenceMUX.FastLoopPump[1] = 1; SequenceMUX.FastLoopPumpTimer[1] = 20000;  /* 20 s pre-pump before period 2 */
    set_clock(2026, 10, 18, 1, 6, 0, 30); schedule_all_days(6, 0, 0);            /* window 06:00:00-06:05:00 */
    StartScheduleMUX = true;
    long pump_ms = 0, v1_ms = 0, v2_ms = 0, v2_during_p1 = 0, first_end = -1, restart = -1; bool was = false;
    for (long t = 0; t < 240000; t += g_dt_ms) {
        ScheduleModeMUX(); tick();
        if (PumpCmd) pump_ms += g_dt_ms;
        if (SingularInletActivate[1]) v1_ms += g_dt_ms;
        if (SingularInletActivate[2]) { v2_ms += g_dt_ms; if (DwellT[0].Out && !SampleT[0].Out) v2_during_p1 += g_dt_ms; }
        if (was && !StartPeriodsMUX && first_end < 0) first_end = t;
        if (!was && StartPeriodsMUX && t > 0 && first_end >= 0 && restart < 0) restart = t;
#ifdef TRACE_RUN
        { static bool pv[8]; bool cv[8] = {StartPeriodsMUX, SingularInletActivate[1], DwellT[0].Out, SampleT[0].Out, DwellT[1].Out, SampleT[1].Out, SampleT[2].Out, PumpCmd};
          static const char *nm[8] = {"StartPeriods", "Valve1", "Dwell1.Out", "Sample1.Out", "Dwell2.Out", "Sample2.Out", "Sample3.Out", "Pump"};
          for (int k = 0; k < 8; k++) { if (cv[k] != pv[k]) printf("t=%6ld after scan: %-12s -> %d\n", t, nm[k], cv[k]); pv[k] = cv[k]; } }
#endif
        was = StartPeriodsMUX;
    }
    eqi("run: valve 1 open for dwell + sample of period 1 (70 s)", v1_ms, 70000 - 2 * 100);   /* A4.10: each timer counts its start scan, so ends 1 scan early: 2 timers */
    eqi("S5 pre-pump runs for Sample - Lead = 40 s, not the 20 s lead", pump_ms, 40000);
    eqi("S5 valve 2 opened early by the pre-pump for the same 40 s", v2_during_p1, 40000);
    eqi("run: sequence ends after 3 x 70 s", first_end, 210000 - 6 * 100);   /* 6 timers, 1 scan each (A4.10) */
    eqi("S3 still inside the 5-minute window: the sequence starts again", restart, first_end + 100);
    eqi("run: nothing out of bounds", g_oob, 0);

    /* ---------- 3b. a period number that is neither its position nor 0 (finding S10) ---------- */
    reset_run(); { const uint8_t v[] = {1, 2, 3}; seq(3, v, 1000, 1000); } SequenceMUX.NumberArray[1] = 5;
    set_clock(2026, 10, 18, 1, 6, 0, 30); schedule_all_days(6, 0, 0); StartScheduleMUX = true;
    for (int s = 0; s < 600; s++) { ScheduleModeMUX(); tick(); }               /* 60 s: far past 3 x 2 s */
    eqi("S10 period 2 numbered 5: the run never ends", StartPeriodsMUX, 1);
    eqi("S10 ... and period 2 never starts", DwellT[1].Out || DwellT[1].On, 0);

    /* ---------- 3c. the period-1 pre-pump uses a clock read only on start / every 30 min (finding S11) ---------- */
    reset_run(); { const uint8_t v[] = {1, 2}; seq(2, v, 10000, 60000); }
    SequenceMUX.FastLoopPump[0] = 1; SequenceMUX.FastLoopPumpTimer[0] = 120000;    /* 2 min lead before 06:00:06 */
    set_clock(2026, 10, 18, 1, 5, 0, 0); schedule_all_days(6, 0, 6);             /* 06:00:06 reads the same both ways (S1) */
    StartScheduleMUX = true; long pump_0559 = -1, pump_on_at = -1;
    for (long t = 0; t < 3606000; t += g_dt_ms) { ScheduleModeMUX(); tick();
        if (t == 3540000) pump_0559 = PumpCmd;                                   /* 05:59:00, inside the 2 min lead */
        if (PumpCmd && pump_on_at < 0) pump_on_at = t; }
    eqi("S11 05:59:00, inside the lead: pump still OFF (CurrentUnix is 05:30)", pump_0559, 0);
    eqi("S11 pump only comes on at the 06:00 half-hourly re-read (6 s, not 120 s)", pump_on_at / 1000, 3600 - 1);

    /* ---------- 4. the same valve in two periods (finding S7) ---------- */
    reset_run(); { const uint8_t v[] = {1, 2, 1}; seq(3, v, 10000, 60000); }
    set_clock(2026, 10, 18, 1, 6, 0, 30); StartScheduleMUX = true;
    for (int s = 0; s < 50; s++) { ScheduleModeMUX(); tick(); }               /* 5 s into period 1 dwell */
    eqi("S7 valve 1 in periods 1 and 3: OFF during period 1", SingularInletActivate[1], 0);
    for (int s = 0; s < 1400; s++) { ScheduleModeMUX(); tick(); }             /* into period 3 */
    eqi("S7 ... and ON in period 3 (the last line written wins)", SingularInletActivate[1], 1);

    /* ---------- 5. valve number 0 (finding S8) ---------- */
    reset_run(); { const uint8_t v[] = {0, 2}; seq(2, v, 10000, 60000); }
    set_clock(2026, 10, 18, 1, 6, 0, 30); StartScheduleMUX = true;
    for (int s = 0; s < 50; s++) { ScheduleModeMUX(); tick(); }
    eqi("S8 inlet index 0 in period 1 is forced OFF by the unused rows", SingularInletActivate[0], 0);

    /* ---------- 6. display step not updated in periods 7 and 18 (finding S9) ---------- */
    reset_run(); { uint8_t v[8] = {1,2,3,4,1,2,3,4}; seq(8, v, 1000, 1000); }
    set_clock(2026, 10, 18, 1, 6, 0, 30); StartScheduleMUX = true;
    int16_t step_in_dwell7 = -1;
    for (int s = 0; s < 200; s++) { ScheduleModeMUX(); tick();
        if (SampleT[5].Out && !DwellT[6].Out && step_in_dwell7 < 0 && DwellT[6].Current < 900) step_in_dwell7 = DwellSampleStep; }
    eqi("S9 during Dwell 7 the step still shows 1 (sample)", step_in_dwell7, 1);

    /* ---------- 7. stop, pause and the analyser lock ---------- */
    reset_run(); { const uint8_t v[] = {1, 2, 3}; seq(3, v, 10000, 60000); }
    SequenceMUX.FastLoopPump[1] = 1; SequenceMUX.FastLoopPumpTimer[1] = 20000;
    set_clock(2026, 10, 18, 1, 6, 0, 30); StartScheduleMUX = true;
    for (int s = 0; s < 500; s++) { ScheduleModeMUX(); tick(); }              /* 50 s: in period 1 sampling */
    PauseMUX = true; ScheduleModeMUX();
    eqi("pause ends the run (rung 5), it does not hold it", StartPeriodsMUX, 0);
    eqi("pause: all valves cleared", SingularInletActivate[1], 0);
    for (int s = 0; s < 10; s++) { ScheduleModeMUX(); tick(); }
    eqi("pause held: no restart", StartPeriodsMUX, 0);
    /* S6 precedence: on its own, ScheduleTimersAndPump ignores pause for periods 2-20 */
    StartPeriodsMUX = true; DwellT[0].Out = true; SampleT[0].Current = 30000; FastLoopStartTime[0] = 40000;
    SequenceMUX.FastLoopPump[1] = 1; SequenceMUX.NumberArray[1] = 2; PauseMUX = true; ScheduleTimersAndPump();
    eqi("S6 PumpCmd period-2 term is not blocked by PauseMUX", PumpCmd, 1);
    StartScheduleMUX = true; StartPeriodsMUX = false; PauseMUX = false; DwellT[0].Out = false;
    CurrentUnix = 100; FastLoopStartUnix = 50; NextSampleStartUnix = 200; SequenceMUX.NumberArray[0] = 1; SequenceMUX.FastLoopPump[0] = 1;
    PauseMUX = true; ScheduleTimersAndPump(); eqi("S6 period-1 term is blocked by PauseMUX", PumpCmd, 0);
    PauseMUX = false; ScheduleTimersAndPump(); eqi("period-1 term inside the pre-pump window", PumpCmd, 1);

    /* ---------- 8. start checks (rungs 1-2) ---------- */
    reset_run(); NumberOfMUXValves = 3; { const uint8_t v[] = {1, 4}; seq(2, v, 1000, 1000); }
    StartScheduleMUX = true; ScheduleButtonStateMUX = 1; ScheduleModeMUX();
    eqi("valve number above the count: start refused", StartScheduleMUX, 0);
    eqi("... and flagged", YouHaveIncorrectInletOutletConfigMUX, 1);
    eqi("... and the button state reset", ScheduleButtonStateMUX, 0);
    reset_run(); NumberOfMUXValves = 3; { const uint8_t v[] = {3, 2}; seq(2, v, 1000, 1000); }
    StartScheduleMUX = true; ScheduleModeMUX(); eqi("valve number equal to the count is accepted", StartScheduleMUX, 1);
    reset_run(); memset(&SequenceMUX, 0, sizeof SequenceMUX); StartScheduleMUX = true; ScheduleModeMUX();
    eqi("no periods: start refused", StartScheduleMUX, 0); eqi("... SequenceAbsentMUX set", SequenceAbsentMUX, 1);

    /* ---------- 9. the end time is From + 5 minutes with no wrap (finding S4) ---------- */
    memset(&ScheduleMUX, 0, sizeof ScheduleMUX); ScheduleMUX.From_Hour[2] = 10; ScheduleMUX.From_Hour[1] = 57;
    EntryTimerBitMUX = true; SchedulerSequencesMUX();
    eqi("S4 From 10:57 gives To minutes = 62", ScheduleMUX.To_Hour[1], 62);
    eqi("S4 To hours unchanged", ScheduleMUX.To_Hour[2], 10);

    /* ---------- 10. the sequence editor ---------- */
    NumberOfMUXValves = 3; memset(SingularInlet, 0, sizeof SingularInlet); strcpy(SingularInlet[0], "Stack A"); strcpy(SingularInlet[2], "Stack C");
    { const uint8_t v[] = {1, 2, 3}; seq(3, v, 5000, 6000); }
    /* LoadSequence triggers the project-level action LoadSequenceMUX (load screen); the OS resets the trigger bit after
     * the action (UniLogic help, Project-level Actions). Modelled as: cleared between scans (ASSUMPTION A4.15). */
    LoadSequence = false; OpenScheduleSequenceMUX = true; int load_scans = 0;
    for (int s = 0; s < 20; s++) { SchedulerSequencesMUX(); if (LoadSequence) { load_scans++; LoadSequence = false; } }
    eqi("inlet list: 3 lines", g_csv_lines, 3);
    eqs("inlet list line 1 is the name", g_csv[0], "Stack A");
    eqs("inlet list line 2: blank name written as its number", g_csv[1], "2");
    eqi("list done: LoadSequence set once (then reset by the OS)", load_scans, 1);
    eqi("screen copy loaded", HMISequenceMUX.SampleTimer[2], 6000);
    eqi("7-row window row 3 dwell", (long)Timer1_7[2].Preset, 5000);
    HMISequenceMUX.DwellTimer[0] = 9999; SchedulerSequencesMUX();
    eqi("S12 (corrected) with the trigger reset by the OS, an edit is kept", (long)HMISequenceMUX.DwellTimer[0], 9999);
    SequenceMUX.NumberOfPeriods = 9; for (int i = 3; i < 9; i++) SequenceMUX.NumberArray[i] = (uint8_t)(i + 1);
    LoadSequence = true; SchedulerSequencesMUX(); LoadSequence = false;          /* load the 9-period sequence */
    ScrollDownMUX = true; SchedulerSequencesMUX(); SchedulerSequencesMUX();
    eqi("S12 (corrected) ... and scrolling works", scrollIndexMUX, 1);
    LoadSequence = true; SchedulerSequencesMUX(); HMISequenceMUX.DwellTimer[0] = 7777; SchedulerSequencesMUX(); LoadSequence = false;
    eqi("S12 if the OS takes more than one scan, an edit in that time is lost", (long)HMISequenceMUX.DwellTimer[0], 5000);
    /* timer check on write-back */
    LoadSequence = false; HMISequenceMUX.NumberOfPeriods = 3; scrollIndexMUX = 0;
    SampleTimer1_7[1].Preset = 30000; FastLoopTimer1_7[1].Preset = 45000; EntryRowMUX[1] = true; Scheduler_SequencesScrollMUX();
    eqi("lead longer than sample: the two are swapped (sample)", (long)HMISequenceMUX.SampleTimer[1], 45000);
    eqi("... (lead)", (long)HMISequenceMUX.FastLoopPumpTimer[1], 30000);
    SampleTimer1_7[0].Preset = 30000; FastLoopTimer1_7[0].Preset = 30000; EntryRowMUX[0] = true; Scheduler_SequencesScrollMUX();
    eqi("lead equal to sample: lead cut by 10 s", (long)HMISequenceMUX.FastLoopPumpTimer[0], 20000);
    /* number of periods entered: rows from the count up are cleared, numbering 1..n */
    HMISequenceMUX.NumberOfPeriods = 2; HMISequenceMUX.SingulValves[2] = 7; NumberOfPeriodsEnteredMUX = true; Scheduler_SequencesScrollMUX();
    eqi("count entered: row 3 inlet cleared", HMISequenceMUX.SingulValves[2], 0);
    eqi("count entered: row 2 numbered 2", HMISequenceMUX.NumberArray[1], 2);
    /* save */
    HMISequenceMUX.DwellTimer[0] = 1234; SequenceMUX.DwellTimer[5] = 77; SaveScheduleSequencesMUX = true; SchedulerSequencesMUX();
    eqi("save: stored copy updated", (long)SequenceMUX.DwellTimer[0], 1234);
    eqi("save: rows beyond the count cleared first", (long)SequenceMUX.DwellTimer[5], 0);
    /* S15: 20 valves -> the inlet list never finishes */
    NumberOfMUXValves = 20; LoadSequence = false; LoadinletlistMUX = false; OpenScheduleSequenceMUX = true;
    for (int s = 0; s < 100; s++) SchedulerSequencesMUX();
    eqi("S15 20 valves: list stuck at 19 lines", g_csv_lines, 19);
    eqi("S15 ... still loading", LoadinletlistMUX, 1);
    eqi("S15 ... LoadSequence never set", LoadSequence, 0);
    LoadinletlistMUX = false;

    /* ---------- 11. top level ---------- */
    reset_run(); Ladder_Initial_Cycle = true; NumberOfMUXValves = 25; n_adjust = 0; memset(&SequenceMUX, 0, sizeof SequenceMUX);
    SequenceMUX.NumberOfPeriods = 2; MUX(); Ladder_Initial_Cycle = false;
    eqi("first scan, 25 valves: set to 2 (not 20)", NumberOfMUXValves, 2);
    eqi("... sizing re-run", n_adjust, 1);
    eqi("... and the stored sequence cleared (rung 13 runs next scan)", ResetSequencesMUX, 0);
    eqi("... cleared", SequenceMUX.NumberOfPeriods, 0);
    StartScheduleMUX = false; StartModbusMUX = false; ManualModeStartMUX = false; MUX();
    eqi("no mode: CurrentModeMUX 0", CurrentModeMUX, 0); eqi("no mode: all buttons enabled", ModbusModeEnableMUX && SchedulerModeEnableMUX && ManualModeEnableMUX, 1);
    ManualModeStartMUX = true; ManualTimerMUX.Preset = 1000; ManualTimerMUX.Current = 1000; MUX();
    eqi("manual: CurrentModeMUX 3", CurrentModeMUX, 3); eqi("manual: schedule button disabled", SchedulerModeEnableMUX, 0);
    for (int s = 0; s < 12; s++) MUX();
    eqi("manual time used up: manual mode ends", ManualModeStartMUX, 0);
    /* flow validation */
    FlowValidation = true; StartModbusMUX = true; SingularInletActivate[4] = true; FlowAnalogInput = 0;
    FlowValidationTimer.Preset = 2000; AnalogSensor[0] = 10; AnalogTempContainer = 0; AnalogTempContainerPGap = AnalogTempContainerMGap = 0; FlowValidationAlarm = false;
    for (int s = 0; s < 15; s++) MUX();
    eqi("flow flat for 1.5 s: no alarm yet", FlowValidationAlarm, 0);
    for (int s = 0; s < 10; s++) MUX();
    eqi("flow flat past 2 s: alarm", FlowValidationAlarm, 1);
    FlowValidationAlarm = false; FlowAnalogInput = 1; AnalogSensor[1] = 10; AnalogSensor[0] = 50; AnalogTempContainer = 10;
    AnalogTempContainerPGap = 12; AnalogTempContainerMGap = 8; treset(&FlowValidationTimer);
    for (int s = 0; s < 25; s++) MUX();
    eqi("S14 input 1 selected, input 1 flat, input 0 high: timer keeps restarting", FlowValidationAlarm, 0);
    /* rung 17: the DI and Modbus purge bits also arm the validation timer */
    memset(SingularInletActivate, 0, sizeof SingularInletActivate); PurgeInletActivate = false; PurgeInletActivateModbus = true; MUX();
    eqi("R17 PurgeInletActivateModbus alone arms flow validation", ValidationTime, 1); PurgeInletActivateModbus = false;

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
