/* 4E_faithful.c - rung-by-rung / line-by-line C of the Stage 4E MUX scheduling functions.
 * Sources: stage0/project_db/ladder/MUX_Code__{ScheduleModeMUX,SchedulerSequencesMUX,MUX}.txt and
 *          stage0/project_db/st/MUX_Code__{ScheduleCalculate1stFastLoopPump,ScheduleFastLoopTime,
 *          ScheduleTimersAndPump,ScheduleModeSTValveAssignMUX,Scheduler_SequencesScrollMUX}.st
 * Rules: E4 (a block passes its input power on), E7 (one edge memory per element, updated every
 * scan, starting FALSE), 11.6.1 (parallel branches top first). Not for the new firmware.
 * DISABLED IN ORIGINAL rungs are under #ifdef ENABLE_DISABLED_RUNGS. */
#include <string.h>
#include "4E.h"

/* ---------------- block models ---------------- */
/* TimerOnDelay. 11 section 4: Current counts down while powered, Out at 0, reloaded when unpowered.
 * ASSUMPTION A4.10: Current is also loaded from Preset on the scan the input turns on, so a Preset
 * written while the timer is idle takes effect on the next start. */
static void ton(ul_timer_t *t, bool in) {
    if (!in) { t->Current = t->Preset; t->Out = false; t->On = false; return; }
    if (!t->On) { t->Current = t->Preset; t->On = true; }
    if (!t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
/* TimerAccumulated: counts while powered, holds while not; Out stays until Timer Reset (11 section 4) */
static void tacc(ul_timer_t *t, bool in) {
    if (in && !t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
/* treset (TimerReset): one definition in src/ul_blocks.c (app_vmmux) */
/* edge elements (E7) */
static bool pcont(bool *m, bool b) { bool e = b && !*m; *m = b; return e; }
static bool ncont(bool *m, bool b) { bool e = !b && *m; *m = b; return e; }
static bool pcoil(bool *m, bool p) { bool e = p && !*m; *m = p; return e; }   /* returns the bit value */
/* Remove from Array over the whole array: ASSUMPTION 11 (array left all zeros) */
static void clear_b(bool *a, int n) { memset(a, 0, (size_t)n * sizeof *a); }
#define WR(arr, n, i, v) do { int _i = (int)(i); if (_i >= 0 && _i < (n)) (arr)[_i] = (v); else g_oob++; } while (0)

/* RtcToUnixTime ("Date Time to UTC"): seconds since 1970 (epoch does not change behaviour, 11 section 4;
 * time zone not applied, ASSUMPTION 6.10) */
static uint32_t rtc_to_unix(void) {
    int y = RTC_Date.Year, m = RTC_Date.Month, d = RTC_Date.Day_In_Month;
    y -= m <= 2; int era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint32_t)(days * 86400L + RTC_Time.Hours * 3600L + RTC_Time.Minutes * 60L + RTC_Time.Seconds);
}
/* TimeFunction (Schedule): 11 section 4. Years 0/0 and all-zero bit arrays mean "any"; From <= now < To.
 * ASSUMPTION A4.11: To is taken as h*3600 + m*60 + s with no range check, so 10:62:00 acts as 11:02:00. */
static bool any_set(const bool *a, int n) { for (int i = 0; i < n; i++) if (a[i]) return true; return false; }
static bool time_function(const Schedule_t *s) {
    if (!(s->From_Year == 0 && s->To_Year == 0) && (RTC_Date.Year < s->From_Year || RTC_Date.Year > s->To_Year)) return false;
    if (any_set(s->Month, 12) && !s->Month[RTC_Date.Month - 1]) return false;
    if (any_set(s->Day_Of_Month, 31) && !s->Day_Of_Month[RTC_Date.Day_In_Month - 1]) return false;
    if (any_set(s->Day_Of_Week, 7) && !s->Day_Of_Week[RTC_Date.Day_Of_Week - 1]) return false;
    long now = RTC_Time.Hours * 3600L + RTC_Time.Minutes * 60L + RTC_Time.Seconds;
    long from = s->From_Hour[2] * 3600L + s->From_Hour[1] * 60L + s->From_Hour[0];
    long to   = s->To_Hour[2] * 3600L + s->To_Hour[1] * 60L + s->To_Hour[0];
    return from <= now && now < to;
}

/* ================= ScheduleCalculate1stFastLoopPump (ST, 101 lines) ================= */
void ScheduleCalculate1stFastLoopPump(void) {
    bool found = false;                 /* initial value 0 (A4.2) */
    uint32_t scanLimit = 3600;          /* initial value 3600 (A4.2) */
    uint16_t yC = RTC_Date.Year;                                    /* 2 */
    uint8_t mC = RTC_Date.Month, dC = RTC_Date.Day_In_Month;        /* 3-4 */
    uint8_t wdC = (uint8_t)(RTC_Date.Day_Of_Week - 1);              /* 5: 1..7 -> 0..6 */
    uint16_t yFrom = ScheduleMUX.From_Year, yTo = ScheduleMUX.To_Year;   /* 11-12 */
    uint8_t fromH = ScheduleMUX.From_Hour[0];   /* 13: index 0 is Seconds in the Schedule type (finding S1) */
    uint8_t fromM = ScheduleMUX.From_Hour[1];   /* 14 */
    uint8_t fromS = ScheduleMUX.From_Hour[2];   /* 15: index 2 is Hours */
    uint32_t CurrentDayUnix = CurrentUnix - (CurrentUnix % 86400u);          /* 18 */
    uint32_t leadSeconds = SequenceMUX.FastLoopPumpTimer[0] / 1000u;         /* 21 */
    uint32_t startUnix = 0; uint8_t dim;
    do {                                                                      /* 24 REPEAT */
        switch (mC) {                                                         /* 26-38 */
        case 1: case 3: case 5: case 7: case 8: case 10: case 12: dim = 31; break;
        case 4: case 6: case 9: case 11: dim = 30; break;
        case 2: { bool isLeap = (yC % 4 == 0) && ((yC % 100 != 0) || (yC % 400 == 0)); dim = isLeap ? 29 : 28; } break;
        default: dim = 31;
        }
        if (yC >= yFrom && yC <= yTo) {                                       /* 41 */
            bool mo = (mC >= 1 && mC <= 12) ? ScheduleMUX.Month[mC - 1] : (g_oob++, false);
            bool dm = (dC >= 1 && dC <= 31) ? ScheduleMUX.Day_Of_Month[dC - 1] : (g_oob++, false);
            bool dw = (wdC <= 6) ? ScheduleMUX.Day_Of_Week[wdC] : (g_oob++, false);
            if (mo && dm && dw) {                                             /* 42-44 */
                startUnix = CurrentDayUnix + (uint32_t)fromH * 3600u + (uint32_t)fromM * 60u + (uint32_t)fromS;  /* 46-49 */
                if (startUnix >= CurrentUnix) found = true;                   /* 51-53 */
            }
        }
        if (!found) {                                                         /* 58 */
            if (dC < dim) dC++; else { dC = 1; if (mC < 12) mC++; else { mC = 1; yC++; } }   /* 60-70 */
            wdC = (uint8_t)((wdC + 1) % 7);                                   /* 73 */
            if (CurrentDayUnix <= 4294967295u - 86400u) CurrentDayUnix += 86400u; else CurrentDayUnix = 4294967295u;  /* 76-80 */
            if (scanLimit > 0) scanLimit--;                                   /* 83-85 */
        }
    } while (!(found || scanLimit == 0));                                     /* 87 */
    if (found) {                                                              /* 91-98 */
        NextSampleStartUnix = startUnix;
        FastLoopStartUnix = (startUnix >= leadSeconds) ? startUnix - leadSeconds : 0;
    } else { NextSampleStartUnix = 0; FastLoopStartUnix = 0; }                /* 100-101 */
}

/* ================= ScheduleFastLoopTime (ST, 9 lines) ================= */
void ScheduleFastLoopTime(void) {
    for (int i = 0; i <= 18; i++)                                             /* 1 */
        if (SequenceMUX.NumberArray[i + 1] != 0) {                            /* 2 */
            uint32_t diffUD = (SequenceMUX.SampleTimer[i] >= SequenceMUX.FastLoopPumpTimer[i + 1])
                            ? SequenceMUX.SampleTimer[i] - SequenceMUX.FastLoopPumpTimer[i + 1] : 0;  /* 3-7 */
            FastLoopStartTime[i] = diffUD;                                    /* 8 */
        }
}

/* ================= ScheduleTimersAndPump (ST, one statement, lines 1-128) =================
 * Grouping exactly as written, with IEC precedence (AND before OR):
 *   PumpCmd := (NOT lock) AND (StartScheduleMUX AND window AND NA[0]=1 AND FLP[0])
 *              OR ( StartPeriodsMUX AND p2  OR p3 OR ... OR p20 )        (finding S6) */
static bool pump_term(int k) {   /* period k (2..20): lines 14-18, 21-25, ... */
    return DwellT[k - 2].Out && SequenceMUX.FastLoopPump[k - 1] && SequenceMUX.NumberArray[k - 1] == k
        && SampleT[k - 2].Current <= FastLoopStartTime[k - 2] && SampleT[k - 2].Current != 0;
}
void ScheduleTimersAndPump(void) {
    bool lock = (EnableLockValvesIfAnalyzerNotOk && AnalyzerOkBitAlarm) || StopSeqBit || PauseMUX;   /* 1-2 */
    bool first = StartScheduleMUX && CurrentUnix >= FastLoopStartUnix && CurrentUnix < NextSampleStartUnix
              && SequenceMUX.NumberArray[0] == 1 && SequenceMUX.FastLoopPump[0];                    /* 4-9 */
    bool rest = StartPeriodsMUX && pump_term(2);                                                    /* 12-18 */
    for (int k = 3; k <= 20; k++) rest = rest || pump_term(k);                                      /* 21-127 */
    PumpCmd = (!lock && first) || rest;                                                             /* 128 */
}

/* ================= ScheduleModeSTValveAssignMUX (ST, 239 lines) ================= */
static void set_inlet(int period_row, bool v) {   /* SingularInletActivate[SequenceMUX.SingulValves[row]] := v */
    WR(SingularInletActivate, 19, SequenceMUX.SingulValves[period_row], v);
}
void ScheduleModeSTValveAssignMUX(void) {
    if (!StartScheduleMUX) return;                                                                   /* 1 */
    set_inlet(0, (!StartPeriodsMUX && PumpCmd) || (StartPeriodsMUX && (!DwellT[0].Out || !SampleT[0].Out)));  /* 3-5 */
    if (!StartPeriodsMUX) return;                                                                    /* 7 */
    for (int k = 2; k <= 20; k++)                                                                    /* 9-101 */
        set_inlet(k - 1, (DwellT[k - 2].Out && !SampleT[k - 2].Out && PumpCmd)
                      || (SampleT[k - 2].Out && (!DwellT[k - 1].Out || !SampleT[k - 1].Out)));
    /* 103-236: IF/ELSIF chain over the 40 steps. DwellSampleStep is NOT written in the arms for
     * Dwell7, Sample7, Dwell18 and Sample18 (finding S9). */
    if (!DwellT[0].Out) { ProcessTimerMUX.Preset = DwellT[0].Current; DwellSampleStep = 0; return; }
    for (int k = 1; k <= 20; k++) {
        bool keep = (k == 7 || k == 18);
        if (k > 1 && SampleT[k - 2].Out && !DwellT[k - 1].Out) {
            ProcessTimerMUX.Preset = DwellT[k - 1].Current; if (!keep) DwellSampleStep = 0; return; }
        if (DwellT[k - 1].Out && !SampleT[k - 1].Out) {
            ProcessTimerMUX.Preset = SampleT[k - 1].Current; if (!keep) DwellSampleStep = 1; return; }
    }
}

/* ================= ScheduleModeMUX (ladder, 23 rungs) ================= */
void ScheduleModeMUX(void) {
    static bool m3a, m3b, m3c, m4, m5a, m5b, m5c, m5d, m6, mStop[19], m2;
    /* R1 */
    if (StartScheduleMUX) {
        SearchInArray_u8(SequenceMUX.SingulValves, NumberOfMUXValves, &InletIndexMUX);
        if (InletIndexMUX != 25) { YouHaveIncorrectInletOutletConfigMUX = true; StartScheduleMUX = false; ScheduleButtonStateMUX = 0; }
    }
    /* R2 */
    {   bool p = StartScheduleMUX;
        if (p && SequenceMUX.NumberOfPeriods == 0) { StartScheduleMUX = false; SequenceAbsentMUX = true; ScheduleButtonStateMUX = 0; }
        bool q = p && !StartPeriodsMUX;
        ton(&Every30minutes, q);
        RecalculateNextScheduleMUX = pcoil(&m2, q);
    }
    /* R3 */
    {   bool p1 = pcont(&m3a, StartScheduleMUX);
        bool p2 = pcont(&m3b, Every30minutes.Out); if (p2) treset(&Every30minutes);
        bool p3 = pcont(&m3c, RecalculateNextScheduleMUX);
        if (p1 || p2 || p3) { CurrentUnix = rtc_to_unix(); ScheduleCalculate1stFastLoopPump(); }
    }
    /* R4 */
    {   bool p = StartScheduleMUX && time_function(&ScheduleMUX) && !StartPeriodsMUX;
        bool w7 = (p && EnableLockValvesIfAnalyzerNotOk && !AnalyzerOkBitAlarm) || (p && !EnableLockValvesIfAnalyzerNotOk);
        bool w10 = w7 && !StopSeqBit && !PauseMUX;
        StartMUXSchedule1 = pcoil(&m4, w10);
        Temp3MUX = w10;                         /* a coil after a transition coil gets the rung power (E4) */
    }
    /* R5 */
    {   bool p1 = ncont(&m5a, StartScheduleMUX);
        bool ps = pcont(&m5b, StopSeqBit), pp = pcont(&m5c, PauseMUX);
        bool pa = pcont(&m5d, AnalyzerOkBitAlarm);
        bool w6 = ps || pp || (EnableLockValvesIfAnalyzerNotOk && pa);
        if (p1 || (w6 && StartScheduleMUX)) { StartPeriodsMUX = false; clear_b(SingularInletActivate, 19); }
    }
    /* R6 */
    if (pcont(&m6, StartMUXSchedule1)) {
        ScheduleFastLoopTime();
        StartPeriodsMUX = true;
        for (int i = 0; i < 20; i++) { DwellT[i].Preset = SequenceMUX.DwellTimer[i]; SampleT[i].Preset = SequenceMUX.SampleTimer[i]; }
    }
#ifdef ENABLE_DISABLED_RUNGS
    /* R7  DISABLED IN ORIGINAL */
    {   static bool m7; if (pcont(&m7, StartMUXSchedule1)) for (int i = 0; i < 20; i++) FastLoopT[i].Preset = SequenceMUX.FastLoopPumpTimer[i]; }
    /* R8-R10  DISABLED IN ORIGINAL: an earlier ladder version of rungs 11-14 and of ScheduleTimersAndPump.
     * Note R10 compares NumberArray[k] with 5 for every k (copy-paste) and drives PumpCmd2. */
    {   static bool m8[10];
        bool w6 = StartScheduleMUX && CurrentUnix >= FastLoopStartUnix && CurrentUnix <= NextSampleStartUnix /* InRange, A4.7 */
               && SequenceMUX.NumberArray[0] == 1 && SequenceMUX.FastLoopPump[0] && SequenceMUX.NumberArray[0] == 1;
        bool sp = StartPeriodsMUX;
        ton(&DwellT[0], sp && SequenceMUX.NumberArray[0] == 1);
        SequenceStopMUX[0] = pcoil(&m8[0], sp && SequenceMUX.NumberArray[0] == 0);
        for (int k = 1; k <= 4; k++) {                    /* R8 branches: Sample k, pump k+1, Dwell k+1 */
            bool s = sp && DwellT[k - 1].Out; ton(&SampleT[k - 1], s);
            w6 = w6 || (s && SequenceMUX.FastLoopPump[k] && SequenceMUX.NumberArray[k] == k + 1
                        && SampleT[k - 1].Current <= FastLoopStartTime[k - 1] && SampleT[k - 1].Current != 0);
            if (k < 4) { bool d = sp && SampleT[k - 1].Out;
                ton(&DwellT[k], d && SequenceMUX.NumberArray[k] == k + 1);
                SequenceStopMUX[k] = pcoil(&m8[k], d && SequenceMUX.NumberArray[k] == 0); }
        }
        PumpCmd = w6;
        for (int k = 4; k <= 9; k++) {                    /* R9: Dwell 5..10 */
            bool d = sp && SampleT[k - 1].Out;
            ton(&DwellT[k], d && SequenceMUX.NumberArray[k] == k + 1);
            SequenceStopMUX[k] = pcoil(&m8[k], d && SequenceMUX.NumberArray[k] == 0);
        }
        bool w8 = false;                                  /* R10: Sample 5..10, PumpCmd2 */
        for (int k = 5; k <= 10; k++) {
            bool s = sp && DwellT[k - 1].Out; ton(&SampleT[k - 1], s);
            w8 = w8 || (s && SequenceMUX.FastLoopPump[k] && SequenceMUX.NumberArray[k] == 5
                        && SampleT[k - 1].Current <= FastLoopStartTime[k - 1] && SampleT[k - 1].Current != 0);
        }
        PumpCmd2 = w8;
    }
#endif
    /* R11-R14: the period chain. Period k: Dwell k runs once Sample k-1 is done (Dwell 1 at the start) if
     * NumberArray[k-1] = k; NumberArray[k-1] = 0 there pulses SequenceStopMUX[k-1]. Sample k runs once Dwell k is done. */
    {   bool sp = StartPeriodsMUX;
        for (int k = 1; k <= 20; k++) {
            bool d = (k == 1) ? sp : (sp && SampleT[k - 2].Out);
            ton(&DwellT[k - 1], d && SequenceMUX.NumberArray[k - 1] == k);
            if (k <= 19) SequenceStopMUX[k - 1] = pcoil(&mStop[k - 1], d && SequenceMUX.NumberArray[k - 1] == 0);  /* none for k = 20 */
            ton(&SampleT[k - 1], sp && DwellT[k - 1].Out);
        }
        /* R14 w28 */
        ScheduleTimersAndPump();
        /* R14 w29-w55: end of sequence */
        bool end = (sp && SampleT[19].Out) || (sp && SampleT[18].Out && SequenceMUX.NumberArray[19] == 0);
        for (int i = 0; i <= 18; i++) end = end || SequenceStopMUX[i];
        if (end) {
            StartPeriodsMUX = false; clear_b(SingularInletActivate, 19);
#if VM_FIX_MUX_RESTART
            /* F5 (app_vmmux): a run that ends inside its start window restarts on the next scan
             * (S3) with every period timer still done, so the second run stood still with no
             * valve open. Resetting the period timers here makes the restart begin at period 1. */
            for (int k = 0; k < 20; k++) { treset(&DwellT[k]); treset(&SampleT[k]); }
#endif
        }
    }
#ifdef ENABLE_DISABLED_RUNGS
    /* R15  DISABLED IN ORIGINAL (unfinished): coil SingularInletActivate[0]. Its other branches, and all of
     * R16-R20, are contact chains (Sequence1Timer1..20.Out) with no output, so they do nothing. */
    {   bool w4 = (!StartPeriodsMUX && StartScheduleMUX && PumpCmd)
               || (StartPeriodsMUX && (!DwellT[0].Out || !SampleT[0].Out));
        SingularInletActivate[0] = w4; }
#endif
    /* R21 */
    ScheduleModeSTValveAssignMUX();
    /* R22, R23 empty */
}

/* ================= Scheduler_SequencesScrollMUX (ST, 452 lines) ================= */
static void load_row(int i, int idx) {
    if (idx < 0 || idx >= 20) { g_oob++; return; }   /* A4.8: only if a count above 20 got in */
    Number1_7MUX[i] = HMISequenceMUX.NumberArray[idx];
    InletMUX[i] = HMISequenceMUX.SingulValves[idx];
    FastLoopPumpCheck[i] = HMISequenceMUX.FastLoopPump[idx];
    Timer1_7[i].Preset = HMISequenceMUX.DwellTimer[idx];
    SampleTimer1_7[i].Preset = HMISequenceMUX.SampleTimer[idx];
    FastLoopTimer1_7[i].Preset = HMISequenceMUX.FastLoopPumpTimer[idx];
}
static void blank_row(int i) {
    Number1_7MUX[i] = 0; InletMUX[i] = 0; FastLoopPumpCheck[i] = 0;
    Timer1_7[i].Preset = 0; SampleTimer1_7[i].Preset = 0; FastLoopTimer1_7[i].Preset = 0;
}
void Scheduler_SequencesScrollMUX(void) {
    int16_t cnt, idx, maxStart;
    if (LoadSequence) {                                                         /* 2-69 */
        cnt = HMISequenceMUX.NumberOfPeriods; scrollIndexMUX = 0;
        for (int i = 0; i <= 6; i++) {
            if (i < cnt) { idx = (int16_t)((scrollIndexMUX + i) % cnt); load_row(i, idx); } else blank_row(i);
        }
    }
    if (NumberOfPeriodsEnteredMUX) {                                            /* 73-157 */
        cnt = HMISequenceMUX.NumberOfPeriods;
        for (int i = cnt; i <= 19; i++) {
            WR(HMISequenceMUX.NumberArray, 20, i, 0); WR(HMISequenceMUX.SingulValves, 20, i, 0);
            WR(HMISequenceMUX.DwellTimer, 20, i, 0); WR(HMISequenceMUX.SampleTimer, 20, i, 0);
            WR(HMISequenceMUX.FastLoopPump, 20, i, 0); WR(HMISequenceMUX.FastLoopPumpTimer, 20, i, 0);
        }
        for (int i = 0; i <= cnt - 1; i++) WR(HMISequenceMUX.NumberArray, 20, i, (uint8_t)(i + 1));
        scrollIndexMUX = 0; NumberOfPeriodsEnteredMUX = false;
        for (int i = 0; i <= 6; i++) {
            if (i < cnt) { idx = (int16_t)((scrollIndexMUX + i) % cnt); load_row(i, idx); } else blank_row(i);
        }
    }
    cnt = HMISequenceMUX.NumberOfPeriods;                                       /* 160-165 */
    maxStart = cnt > 7 ? (int16_t)(cnt - 7) : 0;
    if (ScrollUpMUX && cnt > 0) {                                               /* 169-235 */
        if (scrollIndexMUX > 0) scrollIndexMUX--;
        ScrollUpMUX = false;
        for (int i = 0; i <= 6; i++) { idx = (int16_t)(scrollIndexMUX + i); if (idx < cnt) load_row(i, idx); else blank_row(i); }
    }
    if (ScrollDownMUX && cnt > 0) {                                             /* 239-305 */
        if (scrollIndexMUX < maxStart) scrollIndexMUX++;
        ScrollDownMUX = false;
        for (int i = 0; i <= 6; i++) { idx = (int16_t)(scrollIndexMUX + i); if (idx < cnt) load_row(i, idx); else blank_row(i); }
    }
    cnt = HMISequenceMUX.NumberOfPeriods;                                       /* 309-452 */
    for (int i = 0; i <= 6; i++) {
        if (!EntryRowMUX[i]) continue;
        if (cnt > 0) {
            idx = (int16_t)((scrollIndexMUX + i) % cnt);
            WR(HMISequenceMUX.NumberArray, 20, idx, Number1_7MUX[i]);
            WR(HMISequenceMUX.SingulValves, 20, idx, InletMUX[i]);
            WR(HMISequenceMUX.FastLoopPump, 20, idx, FastLoopPumpCheck[i]);
            if (FastLoopTimer1_7[i].Preset > SampleTimer1_7[i].Preset) {        /* swap */
                uint32_t tmp = SampleTimer1_7[i].Preset; SampleTimer1_7[i].Preset = FastLoopTimer1_7[i].Preset; FastLoopTimer1_7[i].Preset = tmp;
            } else if (FastLoopTimer1_7[i].Preset == SampleTimer1_7[i].Preset) {
                FastLoopTimer1_7[i].Preset = FastLoopTimer1_7[i].Preset >= 10000 ? FastLoopTimer1_7[i].Preset - 10000 : 0;
            }
            WR(HMISequenceMUX.DwellTimer, 20, idx, Timer1_7[i].Preset);
            WR(HMISequenceMUX.SampleTimer, 20, idx, SampleTimer1_7[i].Preset);
            WR(HMISequenceMUX.FastLoopPumpTimer, 20, idx, FastLoopTimer1_7[i].Preset);
        }
        EntryRowMUX[i] = false;
    }
}

/* ================= SchedulerSequencesMUX (ladder, 22 rungs) =================
 * File blocks: DeleteFile / AppendBuffertoFile are modelled on g_csv. Their status codes follow the
 * code's own use: 1 or 2 in progress, 0 done, -8 file not found, other negative = error (ASSUMPTION A4.12).
 * The model reports 1 (in progress) on the scan of the request and 0 (done) from the next scan. */
static int16_t TempStringLength; static char TempString[11], TempStringASCII[9]; static uint8_t TempBuffer[24];
void ScheduleCSVAdds_model(void) {   /* VM ScheduleCSVAdds: CR LF appended to TempBuffer (4F) */
    if (TempStringLength >= 0 && TempStringLength + 2 <= 24) { TempBuffer[TempStringLength] = 13; TempBuffer[TempStringLength + 1] = 10; }
    TempStringLength += 2;
}
void SchedulerSequencesMUX(void) {
    static bool m1, del_pending, app_pending;
    if (del_pending) { InletsDeleteStatus = 0; del_pending = false; }   /* file model: request finished */
    if (app_pending) { CsvBufferStatus = 0; app_pending = false; }
    /* R1 */
    if (pcont(&m1, OpenScheduleSequenceMUX)) { g_csv_lines = 0; InletsDeleteStatus = 1; del_pending = true; }
    /* R2 */
    if (OpenScheduleSequenceMUX) {
        bool ok = InletsDeleteStatus == 1 || InletsDeleteStatus == 2 || InletsDeleteStatus == -8;
        bool err = InletsDeleteStatus != -8 && InletsDeleteStatus < 0;
        if (err) { ContactManufacturer1 = true; LoadingWindow = false; }
        if (ok || err) { OpenScheduleSequenceMUX1 = true; OpenScheduleSequenceMUX = false; }
    }
    /* R3 empty. R4 */
    if (OpenScheduleSequenceMUX1) {
        bool ok = InletsDeleteStatus == 0 || InletsDeleteStatus == -8;
        bool err = InletsDeleteStatus != -8 && InletsDeleteStatus < 0;
        if (ok) { LoadinletlistMUX = true; CurrentInletlistID = 0; TargetInletListID = NumberOfMUXValves; CsvBufferStatus = 0; }
        if (err) { ContactManufacturer1 = true; LoadingWindow = false; }
        if (ok || err) OpenScheduleSequenceMUX1 = false;
    }
    /* R5: build one line of the inlet list (names 0..18; a blank name is written as its number) */
    if (LoadinletlistMUX && CsvBufferStatus == 0 && CurrentInletlistID < TargetInletListID) {
        int id = CurrentInletlistID;
        if (id >= 0 && id <= 18) {                       /* branches exist for 0..18 only (finding S15) */
            strncpy(TempString, SingularInlet[id], 10); TempString[10] = 0;
            TempStringLength = (int16_t)strlen(TempString);
            const char *src = TempString;
            if (TempStringLength <= 0) {
                int16_t TempValveNumber = (int16_t)(CurrentInletlistID + 1);
                /* ConvertIntToString(TempValveNumber, TempLength = 2): last two digits (41b) */
                TempStringASCII[0] = 0; { int v = TempValveNumber % 100; if (v < 0) v = -v;
                    if (v >= 10) { TempStringASCII[0] = (char)('0' + v / 10); TempStringASCII[1] = (char)('0' + v % 10); TempStringASCII[2] = 0; }
                    else { TempStringASCII[0] = (char)('0' + v); TempStringASCII[1] = 0; } }
                TempStringLength = (int16_t)strlen(TempStringASCII); src = TempStringASCII;
            }
            memcpy(TempBuffer, src, (size_t)TempStringLength);
            ScheduleCSVAdds_model();
            AppendStringToCSVListMUX = true;
        }
    }
    /* R6 */
    if (LoadinletlistMUX && ((CsvBufferStatus == 0 && CurrentInletlistID >= TargetInletListID) || CsvBufferStatus < 0)) {
        CsvBufferStatus = 0; CurrentInletlistID = 0; TargetInletListID = 0; LoadinletlistMUX = false; LoadSequence = true; LoadingWindow = false;
    }
    /* R7 */
    if (AppendStringToCSVListMUX) {
        if (g_csv_lines < 24) { int n = TempStringLength - 2; if (n > 23) n = 23; if (n < 0) n = 0;
            memcpy(g_csv[g_csv_lines], TempBuffer, (size_t)n); g_csv[g_csv_lines][n] = 0; g_csv_lines++; }
        CsvBufferStatus = 1; app_pending = true; AppendStringToCSVListMUX = false; CurrentInletlistID++;
    }
    /* R8 empty. R9: while LoadSequence is ON (a project-level action trigger: the OS resets it after the action, A4.15; S12 corrected) */
    if (LoadSequence) { memset(&HMISequenceMUX, 0, sizeof HMISequenceMUX); CopySequenceToHMI(); }
#ifdef ENABLE_DISABLED_RUNGS
    /* R10 DISABLED IN ORIGINAL: copies the VM editor (HMISequences names, numbers, timers, inlets) into
     * the 7-row screen tags. Not modelled: it touches VM tags only (4F). */
#endif
    /* R11 */
    if (EntryTimerBitMUX) {
        ScheduleMUX.To_Hour[2] = ScheduleMUX.From_Hour[2];
        ScheduleMUX.To_Hour[1] = (uint8_t)(ScheduleMUX.From_Hour[1] + 5);    /* no wrap past 59 (finding S4) */
        ScheduleMUX.To_Hour[0] = ScheduleMUX.From_Hour[0];
        EntryTimerBitMUX = false;
    }
    /* R12 */
    if (SaveScheduleSequencesMUX) {
        ScheduleMUX.To_Hour[2] = ScheduleMUX.From_Hour[2];
        ScheduleMUX.To_Hour[1] = (uint8_t)(ScheduleMUX.From_Hour[1] + 5);
        ScheduleMUX.To_Hour[0] = ScheduleMUX.From_Hour[0];
        memset(&SequenceMUX, 0, sizeof SequenceMUX); CopySequenceFromHMI();
        SaveScheduleSequencesMUX = false;
    }
    /* R13 */
    if (ResetSequencesMUX) { ResetSequencesMUX = false; memset(&SequenceMUX, 0, sizeof SequenceMUX); }
    /* R14 */
    if (NumberOfPeriodsEnteredMUX || LoadSequence) {
        uint8_t n = HMISequenceMUX.NumberOfPeriods;
        if (n < 7) { memset(ScheduleSequencesVisib, 0, 7 * sizeof(bool)); for (int i = 0; i < n && i < 7; i++) ScheduleSequencesVisib[i] = true; ArrowButtonsVisib = false; }
        if (n >= 7) { for (int i = 0; i < 7; i++) ScheduleSequencesVisib[i] = true; if (n > 7) ArrowButtonsVisib = true; }
    }
    /* R15 */
    Scheduler_SequencesScrollMUX();
    /* R16 empty. R17-R19: "select all days" box and the day boxes */
    {   static bool mday[31], m18, m19;
        static const int order[31] = {0,1,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,3,2,4}; /* Day1, Day2, Day31..Day6, Day4, Day3, Day5 */
        bool any = false;
        for (int j = 0; j < 31; j++) any = ncont(&mday[order[j]], ScheduleMUX.Day_Of_Month[order[j]]) || any;
        bool was_block = BlockNegativeDaysMUX;
        if (!was_block && any && SelectAllDaysMUX) { SelectAllDaysMUX = false; BlockForSelecAllMUX = true; }
        if (was_block) BlockNegativeDaysMUX = false;
        bool r = pcont(&m18, SelectAllDaysMUX);
        if (r) { if (!BlockForSelecAllMUX) for (int i = 0; i < 31; i++) ScheduleMUX.Day_Of_Month[i] = true; else BlockForSelecAllMUX = false; }
        bool f = ncont(&m19, SelectAllDaysMUX);
        if (f) { if (!BlockForSelecAllMUX) { BlockNegativeDaysMUX = true; for (int i = 0; i < 31; i++) ScheduleMUX.Day_Of_Month[i] = false; } else BlockForSelecAllMUX = false; }
    }
}

/* ================= MUX (ladder, 20 rungs; Function1 rung 36) ================= */
void MUX(void) {
    static bool m12, m12s, m12p, m12a, m18;
    /* R2 */
    if (Ladder_Initial_Cycle && SequenceMUX.NumberOfPeriods == 0) FillNumbersMUX();
    /* R3 */
    {   bool bad = Ladder_Initial_Cycle && (NumberOfMUXValves <= 1 || NumberOfMUXValves > 20);
        if (bad) NumberOfMUXValves = 2;                                         /* both bad cases give 2 */
        if (NumberOfMUXVavlesEntered || bad) {
            memset(SingularInletVisib1, 0, sizeof SingularInletVisib1); memset(SingularInletVisib2, 0, sizeof SingularInletVisib2);
            AdjustNumberOfValvesMUX(); NumberOfMUXVavlesEntered = false; ResetSequencesMUX = true;
        }
    }
    DI_AssigningMUX();                                                          /* R4 */
    SchedulerSequencesMUX();                                                    /* R5 */
    ScheduleModeMUX();                                                          /* R6 */
    ModbusModeMUX(); ModbusValveNamesMUX();                                     /* R7 */
    DO_AssignMUX(); DO_ControlMUX();                                            /* R8 */
#if VM_OLD_LOGGING
    Logging(); LoadTable();                                                     /* R9 */
#endif                                                                          /* R9: platform data logger (vm_config.h) */
    /* R10 manual mode */
    {   bool p = ManualModeStartMUX;
        bool w5 = (p && EnableLockValvesIfAnalyzerNotOk && !AnalyzerOkBitAlarm) || (p && !EnableLockValvesIfAnalyzerNotOk);
        bool w8 = w5 && !PauseMUX && !StopSeqBit;
        tacc(&ManualTimerMUX, w8); if (w8) ProcessTimerMUX.Preset = ManualTimerMUX.Current;
        EnableSingulManualSelection = w8;
    }
    /* R11 */
    if (ManualTimerMUX.Out) { ManualModeStartMUX = false; treset(&ManualTimerMUX); }
    /* R12 */
    {   bool f = ncont(&m12, ManualModeStartMUX);
        bool s = pcont(&m12s, StopSeqBit), pz = pcont(&m12p, PauseMUX), a = pcont(&m12a, AnalyzerOkBitAlarm);
        bool w3 = f || (ManualModeStartMUX && (s || pz || (EnableLockValvesIfAnalyzerNotOk && a)));
        if (f) treset(&ManualTimerMUX);         /* Timer Reset is on the falling-edge branch only, before the join at w3 */
        if (w3) clear_b(SingularInletActivate, 19);
    }
    /* R13-R15: mode number and which mode buttons are enabled */
    {   bool none = !StartScheduleMUX && !StartModbusMUX && !ManualModeStartMUX;
        if (StartModbusMUX) CurrentModeMUX = 1;
        if (none) CurrentModeMUX = 0;
        ModbusModeEnableMUX = StartModbusMUX || none;
        if (StartScheduleMUX) CurrentModeMUX = 2;
        SchedulerModeEnableMUX = StartScheduleMUX || none;
        if (ManualModeStartMUX) CurrentModeMUX = 3;
        ManualModeEnableMUX = ManualModeStartMUX || none;
    }
    /* R16 */
    ModeIsActive = StartScheduleMUX || StartModbusMUX || ManualModeStartMUX;
    /* R17 */
    {   bool any = PurgeInletActivate || PurgeInletActivateDI || PurgeInletActivateModbus; for (int i = 0; i < 19; i++) any = any || SingularInletActivate[i];
        ValidationTime = FlowValidation && ModeIsActive && any; }
    /* R18 */
    ton(&FlowValidationTimer, ValidationTime);
    if (pcont(&m18, FlowValidationTimer.Out)) FlowValidationAlarm = true;
    /* R19: restart the validation timer whenever the chosen flow signal moves by more than Gap */
    if (ValidationTime) {
        bool restore = false;
        if (AnalogTempContainerPGap == AnalogTempContainerMGap) {
            if (FlowAnalogInput == 0 && AnalogSensor[0] != AnalogTempContainer) { AnalogTempContainer = AnalogSensor[0]; restore = true; }
            if (FlowAnalogInput == 1 && AnalogSensor[1] != AnalogTempContainer) { AnalogTempContainer = AnalogSensor[1]; restore = true; }
        }
        if (AnalogTempContainerPGap != AnalogTempContainerMGap) {
            if (FlowAnalogInput == 0 && (AnalogSensor[0] < AnalogTempContainerMGap || AnalogSensor[0] > AnalogTempContainerPGap)) { AnalogTempContainer = AnalogSensor[0]; restore = true; }
            /* w28: compares AnalogSensor[0], not [1] (finding S14) */
            if (FlowAnalogInput == 1 && (AnalogSensor[1] < AnalogTempContainerMGap || AnalogSensor[0] > AnalogTempContainerPGap)) { AnalogTempContainer = AnalogSensor[1]; restore = true; }
        }
        if (restore) { AnalogTempContainerPGap = AnalogTempContainer + Gap; AnalogTempContainerMGap = AnalogTempContainer - Gap; treset(&FlowValidationTimer); }
    }
}
