/* 4F3_faithful.c - rung-by-rung / line-by-line C of the VM sequence and schedule editor.
 * Sources: stage0/project_db/ladder/VM_Code__SchedulerSequencesVM.txt (30 rungs, none disabled),
 *          stage0/project_db/st/VM_Code__Scheduler_SequencesScroll.st (965 lines).
 * Rules E4/E7 and 11.6.1 are the 11_UniLogic_Semantics rules. Called from Function1 rung 15, every scan.
 * Links with 4D_faithful.c (the Copy* routines; it also defines g_oob) and 4A_faithful.c (ConvertIntToString). */
#include <string.h>
#include "4F3.h"

void ConvertIntToString(int16_t It, uint8_t IntLength, char FinalASCIIINT_out[8 + 1]);   /* 4A, 41b */

static bool pcont(bool *m, bool b) { bool e = b && !*m; *m = b; return e; }
static bool ncont(bool *m, bool b) { bool e = !b && *m; *m = b; return e; }
static bool in_range(int v, int lo, int hi) { return v >= lo && v <= hi; }            /* A4.7 */
static void store_string(char *dst, size_t cap, const char *src) { strncpy(dst, src, cap - 1); dst[cap - 1] = 0; }

/* ---------------- file model (as 4E, ASSUMPTION A4.12) ----------------
 * DeleteFile and AppendBuffertoFile report 1 (in progress) on the request scan and 0 (done) from the next scan. */
char g_inlets_csv[24][24], g_outlets_csv[24][24]; int g_inlets_lines, g_outlets_lines;
static bool del_pending, app_pending;
static void append_line(char csv[24][24], int *lines) {
    if (*lines < 24) { int n = TempStringLength - 2; if (n > 23) n = 23; if (n < 0) n = 0;
        memcpy(csv[*lines], TempBuffer, (size_t)n); csv[*lines][n] = 0; (*lines)++; }
}
/* ScheduleCSVAdds (46b): CR LF copied to TempBuffer at TempStringLength, length + 2 always */
static void ScheduleCSVAdds(void) {
    UserDelim[0] = 13; UserDelim[1] = 10;
    int di = TempStringLength;
    if (di >= 0 && di < 24) { int c = 2; if (c > 24 - di) { c = 24 - di; g_oob++; } memcpy(&TempBuffer[di], UserDelim, (size_t)c); }
    else g_oob++;
    TempStringLength = (int16_t)(TempStringLength + 2);
}
/* TagToBuffer(string, TempBuffer, 0, Overwrite): the string's bytes from offset 0 */
static void tag_to_buffer(const char *s, size_t tagsize) { size_t n = tagsize < 24 ? tagsize : 24; memcpy(TempBuffer, s, n); }

/* Schedule window stores used by rungs 16, 18, 21, 22: From := ScheduleHMI.From; To := From + 5 minutes.
 * Hour/Minute/Second index constants: [0] = Seconds, [1] = Minutes, [2] = Hours. Minutes + 5 has no carry (V13). */
static void window_from_hmi(Schedule_t *s) {
    s->From_Hour[2] = ScheduleHMI.From_Hour[2]; s->From_Hour[1] = ScheduleHMI.From_Hour[1]; s->From_Hour[0] = ScheduleHMI.From_Hour[0];
    s->To_Hour[2] = ScheduleHMI.From_Hour[2]; s->To_Hour[1] = (uint8_t)(ScheduleHMI.From_Hour[1] + 5); s->To_Hour[0] = ScheduleHMI.From_Hour[0];
}
/* 20 ClearString, Reset NumberOfPeriods, 4 x RemoveFromArr(0, 20) (ASSUMPTION 11 6.2: leaves zeros) */
static void clear_sequence(Sequences_t *q) {
    for (int k = 1; k <= 20; k++) seq_name(q, k)[0] = 0;
    q->NumberOfPeriods = 0;
    memset(q->NumberArray, 0, sizeof q->NumberArray); memset(q->Timers, 0, sizeof q->Timers);
    memset(q->Inlets, 0, sizeof q->Inlets); memset(q->Outlets, 0, sizeof q->Outlets);
}

/* One list line (rung 6 for inlets, rung 10 for outlets; checked by script to be the same apart from the names). */
static void list_line(int16_t *Cur, int16_t Target, char names[19][11], bool *AppendBit) {
    if (!(CsvBufferStatus == 0 && *Cur < Target)) return;
    int id = *Cur;
    if (id < 0 || id > 18) return;                                  /* branches exist for 0..18 only */
    store_string(TempString, sizeof TempString, names[id]);         /* w7 */
    TempStringLength = (int16_t)strlen(TempString);
    bool w12 = false;
    if (TempStringLength > 0) { tag_to_buffer(TempString, sizeof TempString); w12 = true; }
    if (TempStringLength <= 0) {
        bool w19 = false;
        if (ScheduleSequencesInterconnection == 0) { TempValveNumber = (int16_t)(*Cur + 1); w19 = true; }
        if (ScheduleSequencesInterconnection == 1) {
            bool w26 = in_range(TempInlets, 1, 9) || in_range(TempOutlets, 1, 9);
            bool w32 = TempInlets > 9 || TempOutlets > 9;
            if (w26) TempValveNumber = (int16_t)(9 - *Cur);                 /* top branch first (11.6.1) */
            if (w32) TempValveNumber = (int16_t)(19 - *Cur);
            if (w26 || w32) { TempValveNumber++; w19 = true; }               /* w28: Inc once */
        }
        if (w19) {
            ConvertIntToString(TempValveNumber, TempLength, TempStringASCII);
            TempStringLength = (int16_t)strlen(TempStringASCII);
            tag_to_buffer(TempStringASCII, sizeof TempStringASCII); w12 = true;
        }
    }
    if (w12) { ScheduleCSVAdds(); *AppendBit = true; }
}

/* ================= SchedulerSequencesVM (ladder, 30 rungs) ================= */
void SchedulerSequencesVM(void) {
    static bool m2a, m2b, mday[31], m28, m29;
    if (del_pending) { InletsDeleteStatus = 0; OutletsDeleteStatus = 0; del_pending = false; }   /* file model */
    if (app_pending) { CsvBufferStatus = 0; app_pending = false; }

    /* R1: open the schedule screen for 1 or 2 interconnections */
    if (SchedulerInterconnection1Or2) {
        bool a = NumInterconnections == 1, b = NumInterconnections == 2;
        if (a) LoadScheduleInterCon1 = true;
        if (b) LoadScheduleInterCon2 = true;
        if (a || b) SchedulerInterconnection1Or2 = false;
    }
    /* R2: opening the editor, or a change in its interconnection combo box. The combo branch (w6) joins wire w2,
     * so it also runs ResetNumeric(ScheduleSequencesInterconnection): the choice is set back to 0 (finding V12). */
    {   bool e1 = pcont(&m2a, OpenScheduleSequenceInterCon1Or2);
        bool e2 = pcont(&m2b, ScheduleSequencesInterconnectionSelected);
        if (e2) { LoadSecondInterconnectionSchedule = true; LoadingWindow = true; LoadAlterInterconnectionScreen = true; }
        if (e1 || e2) {
            ScheduleSequencesInterconnection = 0;
            g_inlets_lines = 0; InletsDeleteStatus = 1;
            g_outlets_lines = 0; OutletsDeleteStatus = 1; del_pending = true;
        }
    }
    /* R3: wait for both deletes to be under way (1, 2) or report "not found" (-8); other negatives = error */
    {   bool p = OpenScheduleSequenceInterCon1Or2 || ScheduleSequencesInterconnectionSelected;
        bool inOK = InletsDeleteStatus == 1 || InletsDeleteStatus == 2 || InletsDeleteStatus == -8;
        bool inErr = InletsDeleteStatus != -8 && InletsDeleteStatus < 0;
        bool outOK = OutletsDeleteStatus == 1 || OutletsDeleteStatus == 2 || OutletsDeleteStatus == -8;
        bool outErr = OutletsDeleteStatus != -8 && OutletsDeleteStatus < 0;
        bool w6 = p && inOK && outOK, w15 = p && ((inOK && outErr) || inErr);
        if (w6) OpenScheduleSequenceInterCon1Or2_1 = true;
        if (w15) { ContactManufacturer1 = true; LoadingWindow = false; }
        if (w6 || w15) { OpenScheduleSequenceInterCon1Or2 = false; ScheduleSequencesInterconnectionSelected = false; }
    }
    /* R4 */
    if (Ackbit1) { ContactManufacturer1 = false; Ackbit1 = false; }
    /* R5: deletes done (0) or not found (-8): set up the two lists for the chosen interconnection */
    if (OpenScheduleSequenceInterCon1Or2_1) {
        bool inOK = InletsDeleteStatus == 0 || InletsDeleteStatus == -8;
        bool inErr = InletsDeleteStatus != -8 && InletsDeleteStatus < 0;
        bool outOK = OutletsDeleteStatus == 0 || OutletsDeleteStatus == -8;
        bool outErr = OutletsDeleteStatus != -8 && OutletsDeleteStatus < 0;
        bool w6 = inOK && outOK, w39 = (inOK && outErr) || inErr, w14 = false;
        if (w6) {
            Loadinletlist = true;
            if (ScheduleSequencesInterconnection == 0) {
                CurrentInletlistID = 0; CurrentOutletlistID = 0; TargetInletListID = FirstInlets; TargetOutletListID = FirstOutlets; w14 = true;
            }
            if (ScheduleSequencesInterconnection == 1) {
                TempInlets = (int16_t)(FirstInlets + SecondInlets); TempOutlets = (int16_t)(FirstOutlets + SecondOutlets);
                if (in_range(TempInlets, 1, 9) || in_range(TempOutlets, 1, 9)) {
                    CurrentInletlistID = (int16_t)(9 - SecondInlets); CurrentOutletlistID = (int16_t)(9 - SecondOutlets);
                    TargetInletListID = 9; TargetOutletListID = 9; w14 = true; }
                if (TempInlets > 9 || TempOutlets > 9) {                       /* runs after, so it wins if both */
                    CurrentInletlistID = (int16_t)(19 - SecondInlets); CurrentOutletlistID = (int16_t)(19 - SecondOutlets);
                    TargetInletListID = 19; TargetOutletListID = 19; w14 = true; }
            }
            if (w14) CsvBufferStatus = 0;
        }
        if (w39) { ContactManufacturer1 = true; LoadingWindow = false; }
        if (w14 || w39) OpenScheduleSequenceInterCon1Or2_1 = false;
    }
    /* R6-R8: inlet list, one line per append */
    if (Loadinletlist) list_line(&CurrentInletlistID, TargetInletListID, InletName, &AppendStringToCSVList);
    if (Loadinletlist && ((CsvBufferStatus == 0 && CurrentInletlistID >= TargetInletListID) || CsvBufferStatus < 0)) {
        CsvBufferStatus = 0; CurrentInletlistID = 0; TargetInletListID = 0; Loadinletlist = false; LoadOutlets = true; }
    if (AppendStringToCSVList) {
        append_line(g_inlets_csv, &g_inlets_lines); CsvBufferStatus = 1; app_pending = true;
        AppendStringToCSVList = false; CurrentInletlistID++; }
    /* R9 empty. R10-R12: outlet list (same as R6-R8, checked by script) */
    if (LoadOutlets) list_line(&CurrentOutletlistID, TargetOutletListID, OutletName, &AppendStringToCSVList1);
    if (LoadOutlets && ((CsvBufferStatus == 0 && CurrentOutletlistID >= TargetOutletListID) || CsvBufferStatus < 0)) {
        CsvBufferStatus = 0; CurrentOutletlistID = 0; TargetOutletListID = 0; LoadOutlets = false; LoadScheduleIntercon1_2 = true; }
    if (AppendStringToCSVList1) {
        append_line(g_outlets_csv, &g_outlets_lines); CsvBufferStatus = 1; app_pending = true;
        AppendStringToCSVList1 = false; CurrentOutletlistID++; }
    /* R13 empty. R14: lists done: load the editor screen for 1 or 2 interconnections */
    if (LoadScheduleIntercon1_2) {
        bool a = NumInterconnections == 1, b = NumInterconnections == 2;
        if (a) LoadSequencesInterCon1 = true;
        if (b) LoadSequencesInterCon2 = true;
        if (a || b) {
            LoadScheduleIntercon1_2 = false; LoadingWindow = false;
            if (LoadSecondInterconnectionSchedule) { LoadSecondInterconnectionSchedule = false; ScheduleSequencesInterconnectionSelected1 = true; }
        }
    }
    /* R15 empty. R16: any of the four screen loads: schedule 1 and sequence 1 into the screen copy */
    if (LoadScheduleInterCon1 || LoadScheduleInterCon2 || LoadSequencesInterCon1 || LoadSequencesInterCon2) {
        CopySchedule1_To_HMI();
        ScheduleInterconnection = 0;
        window_from_hmi(&Schedule1);                                          /* w6 */
        clear_sequence(&HMISequences);                                        /* w12-w36 */
        InletMaxHMI = (uint8_t)FirstInlets; OutletsMaxHMI = (uint8_t)FirstOutlets;
        CopySequences1_To_HMI();
        BlockForSelecAll = true;                                              /* w40 */
        if (SelectAllDaysSeq1) SelectAllDays = true; else SelectAllDays = false;
    }
    /* R17: schedule screen combo: show schedule 1 or 2 */
    if (ScheduleInterconnectionSelected) {
        if (ScheduleInterconnection == 0) CopySchedule1_To_HMI();
        if (ScheduleInterconnection == 1) CopySchedule2_To_HMI();
    }
    /* R18: editor reload for the chosen interconnection (set by R14 after a combo change) */
    if (ScheduleSequencesInterconnectionSelected1) {
        window_from_hmi(&Schedule1);                                          /* w3: always Schedule1 */
        clear_sequence(&HMISequences);
        BlockForSelecAll = true;
        if (ScheduleSequencesInterconnection == 0) {
            SelectAllDays = SelectAllDaysSeq1;
            InletMaxHMI = (uint8_t)FirstInlets; OutletsMaxHMI = (uint8_t)FirstOutlets;
            CopySchedule1_To_HMI(); CopySequences1_To_HMI();
        }
        if (ScheduleSequencesInterconnection == 1) {
            SelectAllDays = SelectAllDaysSeq2;
            InletMaxHMI = (uint8_t)SecondInlets; OutletsMaxHMI = (uint8_t)SecondOutlets;
            CopySchedule2_To_HMI(); CopySequences2_To_HMI();
        }
    }
    /* R19: rows 0..6 of the screen copy into the 7 editor rows (no outlets). The scroll ST overwrites them in
     * the same scan (it runs on the same bit), so this rung has no lasting effect. */
    if (ScheduleSequencesInterconnectionSelected1) {
        for (int i = 0; i < 7; i++) {
            store_string(Name1_7[i], sizeof Name1_7[i], seq_name(&HMISequences, i + 1));
            Number1_7[i] = HMISequences.NumberArray[i];
            Timer1_7[i].Preset = HMISequences.Timers[i];
            Inlet[i] = HMISequences.Inlets[i];
        }
    }
    /* R20: save the schedule */
    if (SaveSchedule) {
        bool a = ScheduleInterconnection == 0, b = ScheduleInterconnection == 1;
        if (a) CopyScheduleHMI_To_1();
        if (b) CopyScheduleHMI_To_2();
        if (a || b) SaveSchedule = false;
    }
    /* R21, R22: save the sequence: window := start + 5 min, clear the stored sequence, copy from the screen */
    if (SaveScheduleSequences && ScheduleSequencesInterconnection == 0) {
        window_from_hmi(&Schedule1); clear_sequence(&Sequence1); CopySequenceHMI_To_1(); SaveScheduleSequences = false; }
    if (SaveScheduleSequences && ScheduleSequencesInterconnection == 1) {
        window_from_hmi(&Schedule2); clear_sequence(&Sequence2); CopySequenceHMI_To_2(); SaveScheduleSequences = false; }
    /* R23: reset both sequences (ResetCoil first, then the clears: E4) */
    if (ResetSequences) { ResetSequences = false; clear_sequence(&Sequence1); clear_sequence(&Sequence2); }
    /* R24: rows shown and arrow buttons */
    if (NumberOfPeriodsEntered || ScheduleSequencesInterconnectionSelected1 || LoadSequencesInterCon1 || LoadSequencesInterCon2) {
        uint8_t n = HMISequences.NumberOfPeriods;
        if (n < 7) { memset(ScheduleSequencesVisib, 0, sizeof ScheduleSequencesVisib);
                     for (int i = 0; i < n; i++) { ScheduleSequencesVisib[i] = true; } ArrowButtonsVisib = false; }
        if (n >= 7) { for (int i = 0; i < 7; i++) ScheduleSequencesVisib[i] = true;
                      if (n > 7) ArrowButtonsVisib = true; }                  /* n = 7 leaves the arrows as they were (V17) */
    }
    /* R25 */
    Scheduler_SequencesScroll();
    /* R26 */
    if (ScheduleInterconnectionSelected) ScheduleInterconnectionSelected = false;
    if (ScheduleSequencesInterconnectionSelected1) ScheduleSequencesInterconnectionSelected1 = false;
    /* R27-R29: "select all days" box and the day boxes (as SchedulerSequencesMUX R17-R19, plus the per-interconnection
     * flags). Edge memories update every scan (E7); the 31 contacts are in the order Day1, Day2, Day31..Day6, Day4, Day3, Day5. */
    {   static const int order[31] = {0,1,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,3,2,4};
        bool any = false;
        for (int j = 0; j < 31; j++) any = ncont(&mday[order[j]], ScheduleHMI.Day_Of_Month[order[j]]) || any;
        bool was_block = BlockNegativeDays;
        if (!was_block && any && SelectAllDays) {
            SelectAllDays = false; BlockForSelecAll = true;
            if (ScheduleInterconnection == 0) SelectAllDaysSeq1 = false;
            if (ScheduleInterconnection == 1) SelectAllDaysSeq2 = false;
        }
        if (was_block) BlockNegativeDays = false;                             /* w43 */
        if (pcont(&m28, SelectAllDays)) {                                     /* R28 */
            if (!BlockForSelecAll) {
                for (int i = 0; i < 31; i++) ScheduleHMI.Day_Of_Month[i] = true;
                if (ScheduleInterconnection == 0) SelectAllDaysSeq1 = true;
                if (ScheduleInterconnection == 1) SelectAllDaysSeq2 = true;
            } else BlockForSelecAll = false;
        }
        if (ncont(&m29, SelectAllDays)) {                                     /* R29 */
            if (!BlockForSelecAll) {
                BlockNegativeDays = true;
                for (int i = 0; i < 31; i++) ScheduleHMI.Day_Of_Month[i] = false;
                if (ScheduleInterconnection == 0) SelectAllDaysSeq1 = false;
                if (ScheduleInterconnection == 1) SelectAllDaysSeq2 = false;
            } else BlockForSelecAll = false;
        }
    }
    /* R30 empty */
}

/* ================= Scheduler_SequencesScroll (ST, 965 lines) =================
 * The 700 name assignments (CASE i / CASE idx) were checked by script: row i <-> Name(idx+1), no exceptions. */
static void load_row(int i, int idx) {
    if (idx < 0 || idx >= 20) { g_oob++; return; }                   /* only if a count above 20 got in (A4.8) */
    Number1_7[i] = HMISequences.NumberArray[idx]; Inlet[i] = HMISequences.Inlets[idx]; Outlet[i] = HMISequences.Outlets[idx];
    Timer1_7[i].Preset = HMISequences.Timers[idx];
    store_string(Name1_7[i], sizeof Name1_7[i], seq_name(&HMISequences, idx + 1));
}
static void blank_row(int i) { Number1_7[i] = 0; Inlet[i] = 0; Outlet[i] = 0; Timer1_7[i].Preset = 0; Name1_7[i][0] = 0; }
static int16_t mod(int16_t a, int16_t b) { if (b == 0) { g_div0++; return 0; } return (int16_t)(a % b); }   /* ASSUMPTION A4.16 */
int g_div0;

void Scheduler_SequencesScroll(void) {
    int16_t cnt, idx, maxStart;                                       /* locals (A4.2) */
    if (LoadSequencesInterCon1 || LoadSequencesInterCon2 || ScheduleSequencesInterconnectionSelected1) {   /* 1-131 */
        cnt = HMISequences.NumberOfPeriods; scrollIndex = 0;
        for (int i = 0; i <= 6; i++) { if (i < cnt) { idx = mod((int16_t)(scrollIndex + i), cnt); load_row(i, idx); } else blank_row(i); }
    }
    if (NumberOfPeriodsEntered) {                                     /* 134-292 */
        cnt = HMISequences.NumberOfPeriods;
        for (int i = cnt; i <= 19; i++) {
            HMISequences.NumberArray[i] = 0; HMISequences.Timers[i] = 0; HMISequences.Inlets[i] = 0; HMISequences.Outlets[i] = 0;
            seq_name(&HMISequences, i + 1)[0] = 0;
        }
        for (int i = 0; i <= cnt - 1; i++) { if (i < 20) HMISequences.NumberArray[i] = (uint8_t)(i + 1); else g_oob++; }
        scrollIndex = 0; NumberOfPeriodsEntered = false;
        for (int i = 0; i <= 6; i++) { if (i < cnt) { idx = mod((int16_t)(scrollIndex + i), cnt); load_row(i, idx); } else blank_row(i); }
    }
    cnt = HMISequences.NumberOfPeriods;                               /* 295-300 */
    maxStart = cnt > 7 ? (int16_t)(cnt - 7) : 0;
    if (ScrollUp && cnt > 0) {                                        /* 304-520 */
        if (scrollIndex > 0) scrollIndex--;
        ScrollUp = false;
        for (int i = 0; i <= 6; i++) { idx = (int16_t)(scrollIndex + i); if (idx < cnt) load_row(i, idx); else blank_row(i); }
    }
    if (ScrollDown && cnt > 0) {                                      /* 524-727 */
        if (scrollIndex < maxStart) scrollIndex++;
        ScrollDown = false;
        for (int i = 0; i <= 6; i++) { idx = (int16_t)(scrollIndex + i); if (idx < cnt) load_row(i, idx); else blank_row(i); }
    }
    cnt = HMISequences.NumberOfPeriods;                               /* 733-965: write-back, no "cnt > 0" guard (V15) */
    for (int i = 0; i <= 6; i++) {
        if (!EntryRow[i]) continue;
        idx = mod((int16_t)(scrollIndex + i), cnt);
        if (idx < 0 || idx >= 20) { g_oob++; }
        else {
            HMISequences.NumberArray[idx] = Number1_7[i]; HMISequences.Inlets[idx] = Inlet[i]; HMISequences.Outlets[idx] = Outlet[i];
            HMISequences.Timers[idx] = Timer1_7[i].Preset;
            store_string(seq_name(&HMISequences, idx + 1), 13, Name1_7[i]);
        }
        EntryRow[i] = false;
    }
}
