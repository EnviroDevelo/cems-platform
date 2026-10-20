/* 4F2_faithful.c - rung-by-rung / line-by-line C of the VM schedule run.
 * Sources: stage0/project_db/ladder/VM_Code__ScheduleMode.txt (28 rungs; 14-19 and 21-26 DISABLED IN ORIGINAL),
 *          stage0/project_db/st/VM_Code__ScheduleModeSTValveAssign.st (237 lines).
 * Rules E4/E7 and 11.6.1 are the 11_UniLogic_Semantics rules. DISABLED rungs are under #ifdef ENABLE_DISABLED_RUNGS. */
#include <string.h>
#include "4F2.h"

/* ---------------- block models (as 4E) ---------------- */
static void ton(ul_timer_t *t, bool in) {          /* TimerOnDelay, A4.10 */
    if (!in) { t->Current = t->Preset; t->Out = false; t->On = false; return; }
    if (!t->On) { t->Current = t->Preset; t->On = true; }
    if (!t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
static bool pcont(bool *m, bool b) { bool e = b && !*m; *m = b; return e; }
static bool ncont(bool *m, bool b) { bool e = !b && *m; *m = b; return e; }
static bool pcoil(bool *m, bool p) { bool e = p && !*m; *m = p; return e; }
static bool any_set(const bool *a, int n) { for (int i = 0; i < n; i++) if (a[i]) return true; return false; }
static bool time_function(const Schedule_t *s) {   /* TimeFunction (Schedule), as 4E (A4.11) */
    if (!(s->From_Year == 0 && s->To_Year == 0) && (RTC_Date.Year < s->From_Year || RTC_Date.Year > s->To_Year)) return false;
    if (any_set(s->Month, 12) && !s->Month[RTC_Date.Month - 1]) return false;
    if (any_set(s->Day_Of_Month, 31) && !s->Day_Of_Month[RTC_Date.Day_In_Month - 1]) return false;
    if (any_set(s->Day_Of_Week, 7) && !s->Day_Of_Week[RTC_Date.Day_Of_Week - 1]) return false;
    long now = RTC_Time.Hours * 3600L + RTC_Time.Minutes * 60L + RTC_Time.Seconds;
    long from = s->From_Hour[2] * 3600L + s->From_Hour[1] * 60L + s->From_Hour[0];
    long to   = s->To_Hour[2] * 3600L + s->To_Hour[1] * 60L + s->To_Hour[0];
    return from <= now && now < to;
}
static int16_t SearchInArray(const uint8_t a[20], int16_t maxv) {   /* 41f: first element above maxv, else 25 */
    for (int i = 0; i <= 19; i++) if ((int16_t)a[i] > maxv) return (int16_t)i;
    return 25;
}

/* The period chain of one interconnection (rungs 8-10 for 1, rungs 11-13 for 2).
 * Period k (1..20): timer k runs while Periods AND (k = 1 or timer k-1 done) AND NumberArray[k-1] = cmp[k-1];
 * NumberArray[k-1] = 0 pulses Stop k (k <= 19). Rung 10/13: NumberArray[19] = 0 after timer 19, timer 20 done,
 * or any Stop -> Periods off and the selection cleared. */
static void chain(bool *Periods, Sequences_t *S, ul_timer_t T[20], bool Stop[19], bool mStop[19], const uint8_t cmp[20],
                  int16_t *SelIn, int16_t *SelOut) {
    bool p = *Periods;
    for (int k = 1; k <= 19; k++) {                                   /* rungs 8-9 / 11-12: one branch per period */
        bool d = (k == 1) ? p : (p && T[k - 2].Out);
        ton(&T[k - 1], d && S->NumberArray[k - 1] == cmp[k - 1]);
        Stop[k - 1] = pcoil(&mStop[k - 1], d && S->NumberArray[k - 1] == 0);
    }
    bool d20 = p && T[18].Out;                                        /* rung 10 / 13 */
    ton(&T[19], d20 && S->NumberArray[19] == cmp[19]);
    bool w7 = (d20 && S->NumberArray[19] == 0) || (p && T[19].Out);
    for (int i = 0; i < 19; i++) w7 = w7 || Stop[i];
    if (w7) { *Periods = false; *SelIn = 0; *SelOut = 0; }
}
/* compare values: period k compares NumberArray[k-1] with k, except interconnection 2 period 9, which compares
 * NumberArray[8] with 8 (finding V7; rung 11, wire w59). */
static const uint8_t CMP1[20] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
static const uint8_t CMP2[20] = {1,2,3,4,5,6,7,8,8,10,11,12,13,14,15,16,17,18,19,20};

/* ================= ScheduleMode (ladder, 28 rungs; Function1 rung 22, every scan) ================= */
void ScheduleMode(void) {
    static bool m3a, m3b, m5, m6, m7, mStop1[19], mStop2[19];
    /* R1: four SearchInArray calls in series (E4), then the error check per interconnection count */
    if (StartSchedule) {
        InletIndex2  = SearchInArray(Sequence2.Inlets,  SecondInlets);
        OutletIndex2 = SearchInArray(Sequence2.Outlets, SecondOutlets);
        InletIndex1  = SearchInArray(Sequence1.Inlets,  FirstInlets);
        OutletIndex1 = SearchInArray(Sequence1.Outlets, FirstOutlets);
        bool bad = (NumInterconnections == 1 && (InletIndex1 != 25 || OutletIndex1 != 25))
                || (NumInterconnections == 2 && (InletIndex1 != 25 || OutletIndex1 != 25 || InletIndex2 != 25 || OutletIndex2 != 25));
        if (bad) { YouHaveIncorrectInletOutletConfig = true; StartSchedule = false; ScheduleButtonState = 0; }
    }
    /* R2: the interconnection-2 branch joins BEFORE the Sequence1 count compare (wire w4), so with two
     * interconnections both counts must be 0 (finding V8) */
    if (StartSchedule && (NumInterconnections == 1 || (NumInterconnections == 2 && Sequence2.NumberOfPeriods == 0))
        && Sequence1.NumberOfPeriods == 0) { StartSchedule = false; SequenceAbsent = true; ScheduleButtonState = 0; }
    /* R3: start pulses. Interconnection 2 only with 2 interconnections; interconnection 1 with 1 or 2. */
    {   bool p = StartSchedule;
        bool w7 = p && NumInterconnections == 2 && time_function(&Schedule2) && !StartInterconnection2Periods;
        StartInterconnectionSchedule2 = pcoil(&m3b, w7); Temp2 = w7;                  /* coil after a transition coil: E4 */
        bool w10 = (p && NumInterconnections == 2 && time_function(&Schedule1)) || (p && NumInterconnections == 1 && time_function(&Schedule1));
        bool w12 = w10 && !StartInterconnection1Periods;
        StartInterconnectionSchedule1 = pcoil(&m3a, w12); Temp3 = w12;
    }
    /* R4 empty. R5: falling edge of StartSchedule ends both runs and clears the selections */
    if (ncont(&m5, StartSchedule)) {
        StartInterconnection2Periods = false; StartInterconnection1Periods = false;
        SelectedFirstInlet = 0; SelectedFirstOutlet = 0; SelectedSecondInlet = 0; SelectedSecondOutlet = 0;
    }
    /* R6, R7: on the start pulse, set Periods and load the 20 presets (4 parallel chains of 5 Stores) */
    if (pcont(&m6, StartInterconnectionSchedule1) && !StartInterconnection1Periods) {
        StartInterconnection1Periods = true; for (int i = 0; i < 20; i++) T1[i].Preset = Sequence1.Timers[i]; }
    if (pcont(&m7, StartInterconnectionSchedule2) && !StartInterconnection2Periods) {
        StartInterconnection2Periods = true; for (int i = 0; i < 20; i++) T2[i].Preset = Sequence2.Timers[i]; }
    /* R8-R10, R11-R13 */
    chain(&StartInterconnection1Periods, &Sequence1, T1, Stop1, mStop1, CMP1, &SelectedFirstInlet, &SelectedFirstOutlet);
    chain(&StartInterconnection2Periods, &Sequence2, T2, Stop2, mStop2, CMP2, &SelectedSecondInlet, &SelectedSecondOutlet);
#ifdef ENABLE_DISABLED_RUNGS
    /* R14-R19 DISABLED IN ORIGINAL: ladder version of the ST selection below, steps 0..19 (no step for "all done").
     * Step k: Periods AND timers 1..k done AND timer k+1 not done -> Selected = Inlets[k], Outlets[k]. */
    if (StartInterconnection1Periods)
        for (int k = 0; k <= 19; k++) { bool ok = !T1[k].Out; for (int i = 0; i < k; i++) ok = ok && T1[i].Out;
            if (ok) { SelectedFirstInlet = Sequence1.Inlets[k]; SelectedFirstOutlet = Sequence1.Outlets[k]; } }
    /* R20 empty (enabled). R21-R26 DISABLED IN ORIGINAL: the same for interconnection 2. */
    if (StartInterconnection2Periods)
        for (int k = 0; k <= 19; k++) { bool ok = !T2[k].Out; for (int i = 0; i < k; i++) ok = ok && T2[i].Out;
            if (ok) { SelectedSecondInlet = Sequence2.Inlets[k]; SelectedSecondOutlet = Sequence2.Outlets[k]; } }
#endif
    /* R27 */
    ScheduleModeSTValveAssign();
    /* R28 empty */
}

/* ================= ScheduleModeSTValveAssign (ST, 237 lines) =================
 * Lines 1-107 (interconnection 1) and 110-216 (interconnection 2): 21 IF blocks each, checked mechanically.
 * Step k (0..19): timers 1..k done and timer k+1 not -> idx := k, ProcessTimer.Preset := timer k+1 remaining.
 * Step 20: all 20 done -> idx := 20, Preset := timer 20 remaining. Runs every scan, running or not (V10). */
static int16_t step_of(const ul_timer_t T[20], ul_timer_t *pt) {
    int16_t idx = 0;                                                  /* local, 0 per call (A4.2) */
    for (int k = 0; k <= 20; k++) {
        bool ok = true; for (int i = 0; i < k; i++) ok = ok && T[i].Out;
        if (k < 20) ok = ok && !T[k].Out;
        if (ok) { idx = (int16_t)k; pt->Preset = T[k < 20 ? k : 19].Current; }
    }
    return idx;
}
void ScheduleModeSTValveAssign(void) {
    int16_t idx1 = step_of(T1, &ProcessTimer);                       /* 1-107 */
    int16_t idx2 = step_of(T2, &ProcessTimer2);                      /* 110-216 */
    if (StartInterconnection1Periods) {                              /* 218-228 */
        if (idx1 >= 0 && idx1 <= 19) { SelectedFirstInlet = Sequence1.Inlets[idx1]; SelectedFirstOutlet = Sequence1.Outlets[idx1]; }
        else { SelectedFirstInlet = 0; SelectedFirstOutlet = 0; StartInterconnection1Periods = false; }
    }
    if (StartInterconnection2Periods) {                              /* 229-237 */
        if (idx2 >= 0 && idx2 <= 19) { SelectedSecondInlet = Sequence2.Inlets[idx2]; SelectedSecondOutlet = Sequence2.Outlets[idx2]; }
        else { SelectedSecondInlet = 0; SelectedSecondOutlet = 0; StartInterconnection2Periods = false; }
    }
}
