/* 4F1_faithful.c - line-by-line / rung-by-rung C of the Stage 4F part 1 VM functions.
 * Sources: stage0/project_db/st/VM_Code__{TwoORFb_22,TwoORFb_10,Route,FB_ValveSelectionInter}.st and
 *          stage0/project_db/ladder/VM_Code__{ScheduleCSVAdds,Loading}.txt.
 * Locals start at 0 on every call (A4.2). Out-of-range array reads give FALSE / 0 and out-of-range writes
 * are skipped; both are counted in g_oob (A4.1, A4.8). Rules E4/E7 are the 11_UniLogic_Semantics rules. */
#include <string.h>
#include "4F1.h"

#define RD(arr, n, i)    (((int)(i) >= 0 && (int)(i) < (n)) ? (arr)[(int)(i)] : (g_oob++, 0))
#define WR(arr, n, i, v) do { int _i = (int)(i); if (_i >= 0 && _i < (n)) (arr)[_i] = (v); else g_oob++; } while (0)

/* ================= TwoORFb_22 (ST, 119 lines; DO_Control rung 2, every scan) ================= */
void TwoORFb_22(void) {
    for (int k = 0; k < 20; k++)      /* lines 1-119: 20 identical IF blocks, k = 0..19 (checked mechanically) */
        DOFinal[k] = DOList[k] || InitialDO[k]
                  || (BackFlushExists && BackFlushOptionVisib && BackFlushDONumber == k && (FlushValveBit || FlushValveBitModbus));
}

/* ================= TwoORFb_10 (ST, 59 lines) NEVER CALLED IN ORIGINAL =================
 * The 10-output forerunner of TwoORFb_22: outputs DOOut0..9 (function outputs, not globals) and no Modbus flush bit. */
void TwoORFb_10(bool DOOut[10]) {
    for (int k = 0; k < 10; k++)      /* lines 1-59 */
        DOOut[k] = DOList[k] || InitialDO[k]
                || (BackFlushExists && BackFlushOptionVisib && BackFlushDONumber == k && FlushValveBit);
}

/* ================= ScheduleCSVAdds (ladder, 5 rungs; rungs 2-5 empty) =================
 * Rung 1, one series chain (E4: each block passes power on):
 *   Fill Buffer(UserDelim, offset 0, Overwrite, 13, x1); Fill Buffer(UserDelim, offset 1, Overwrite, 10, x1);
 *   Copy Buffer Part to Buffer(UserDelim, from 0, 2 bytes, TempBuffer, at TempStringLength);
 *   Add(TempStringLength + 2 -> TempStringLength).
 * Buffer limits as Stage 1 (11c section 10): a destination start outside the buffer copies nothing, a copy
 * running past the end is cut (A4.1). */
void ScheduleCSVAdds(void) {
    UserDelim[0] = 13; UserDelim[1] = 10;                                   /* CR, LF */
    int di = TempStringLength;
    if (di >= 0 && di < 24) { int c = 2; if (c > 24 - di) { c = 24 - di; g_oob++; } memcpy(&TempBuffer[di], UserDelim, (size_t)c); }
    else g_oob++;
    TempStringLength = (int16_t)(TempStringLength + 2);                     /* always, even if nothing was copied */
}

/* ================= Loading (ladder, 5 rungs; rungs 3-5 empty; Function1 rung 2, every scan) ================= */
static void ton(ul_timer_t *t, bool in) {          /* TimerOnDelay, same model as 4E (A4.10) */
    if (!in) { t->Current = t->Preset; t->Out = false; t->On = false; return; }
    if (!t->On) { t->Current = t->Preset; t->On = true; }
    if (!t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
static bool in_range(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }   /* limits inclusive (A4.7) */
void Loading(void) {
    /* R1: LoadingWindow AND NOT LoadingTimer.Out -> TimerOnDelay(LoadingTimer). Restarts itself: the scan after
     * Out the input drops, the timer reloads, and the next scan it starts again. */
    ton(&LoadingTimer, LoadingWindow && !LoadingTimer.Out);
    /* R2: three parallel In Range blocks on the remaining time (it counts DOWN from 2000 ms) */
    bool p = LoadingWindow;
    BitOne   = p && in_range(LoadingTimer.Current, 500, 2000);
    BitTwo   = p && in_range(LoadingTimer.Current, 1000, 2000);
    BitThree = p && in_range(LoadingTimer.Current, 1500, 2000);
}

/* ================= Route (ST, 94 lines; Function1 rung 17, every scan) =================
 * Colours the pipe pictures on the main screens: TubeColor1 = inlet side, TubeColor2 = outlet side. */
void Route(void) {
    int16_t FirstVisibNum = 0, SecondVisibNum = 0, g, j, segLength, segStart;   /* locals, 0 per call (A4.2) */
    if (NumInterconnections == 1) {                                             /* 2-9 */
        if (FirstInlets <= 9 && FirstOutlets <= 9) { FirstVisibNum = 26; SecondVisibNum = 24; }
        else { FirstVisibNum = 56; SecondVisibNum = 54; }
    } else if (NumInterconnections == 2) {                                      /* 10-17 */
        if ((FirstInlets + SecondInlets) <= 9 && (FirstOutlets + SecondOutlets) <= 9) { FirstVisibNum = 26; SecondVisibNum = 24; }
        else { FirstVisibNum = 56; SecondVisibNum = 54; }
    }
    for (int i = 0; i <= FirstVisibNum; i++) WR(TubeColor1, 57, i, 0);          /* 20-22 */
    for (int i = 0; i <= SecondVisibNum; i++) WR(TubeColor2, 55, i, 0);         /* 23-25 */
    /* inlet side, interconnection 1 (31-41) */
    if (SelectedFirstInlet > 0) {
        for (g = 1; g <= SelectedFirstInlet - 1; g++) { j = (int16_t)(3 * (g - 1)); WR(TubeColor1, 57, j, 1); WR(TubeColor1, 57, j + 2, 1); }
        j = (int16_t)(3 * (SelectedFirstInlet - 1)); WR(TubeColor1, 57, j, 1); WR(TubeColor1, 57, j + 1, 1);
    }
    /* inlet side, interconnection 2, drawn from the bottom up (44-62) */
    if (NumInterconnections == 2 && SelectedSecondInlet > 0) {
        segLength = (int16_t)(3 * SecondInlets); segStart = (int16_t)((FirstVisibNum + 1) - segLength);
        for (g = 1; g <= SelectedSecondInlet - 1; g++) { j = (int16_t)(3 * (g - 1));
            WR(TubeColor1, 57, segStart + segLength - 1 - j, 1); WR(TubeColor1, 57, segStart + segLength - 1 - (j + 2), 1); }
        j = (int16_t)(3 * (SelectedSecondInlet - 1));
        WR(TubeColor1, 57, segStart + segLength - 3 - j, 1);
        WR(TubeColor1, 57, segStart + segLength - 1 - j, 1);
        WR(TubeColor1, 57, segStart + segLength - 1 - (j + 1), 1);
        WR(TubeColor1, 57, FirstVisibNum, 1);
    }
    /* outlet side, interconnection 1 (68-77) */
    if (SelectedFirstOutlet > 0) {
        for (g = 1; g <= SelectedFirstOutlet - 1; g++) { j = (int16_t)(3 * (g - 1)); WR(TubeColor2, 55, j + 1, 1); WR(TubeColor2, 55, j + 2, 1); }
        j = (int16_t)(3 * (SelectedFirstOutlet - 1)); WR(TubeColor2, 55, j, 1);
    }
    /* outlet side, interconnection 2 (80-94); note segLength = 3 x outlets - 1 here, 3 x inlets on the inlet side */
    if (NumInterconnections == 2 && SelectedSecondOutlet > 0) {
        segLength = (int16_t)(3 * SecondOutlets - 1); segStart = (int16_t)((SecondVisibNum + 1) - segLength);
        for (g = 1; g <= SelectedSecondOutlet - 1; g++) { j = (int16_t)(3 * (g - 1));
            WR(TubeColor2, 55, segStart + segLength - 1 - (j + 1), 1); WR(TubeColor2, 55, segStart + segLength - 1 - (j + 2), 1); }
        j = (int16_t)(3 * (SelectedSecondOutlet - 1));
        WR(TubeColor2, 55, segStart + segLength - 2 - j, 1);
        WR(TubeColor2, 55, segStart + segLength - 1 - j, 1);
    }
}

/* ================= FB_ValveSelectionInter (ST, 254 lines; Function1 rung 13, on a touch edge) =================
 * The operator picks one inlet and one outlet per interconnection by touching valve pictures. Selections are
 * 1-based (0 = none). Interconnection 2 uses the last valves of the row, numbered from the end. */
void FB_ValveSelectionInter(void) {
    int16_t FirstVisibNum = 0, i, j, idxStartInlet2, idxStartOutlet2;           /* locals, 0 per call (A4.2) */
    if (NumInterconnections == 1) FirstVisibNum = (FirstInlets <= 9 && FirstOutlets <= 9) ? 8 : 18;           /* 20-26 */
    else if (NumInterconnections == 2)                                                                          /* 27-33 */
        FirstVisibNum = ((FirstInlets + SecondInlets) <= 9 && (FirstOutlets + SecondOutlets) <= 9) ? 8 : 18;
    /* Step 1 (36-83): enables from the visible ranges. Any count other than 1 takes the 2-interconnection branch. */
    if (NumInterconnections == 1) {
        for (i = 0; i <= FirstVisibNum; i++) WR(EnableInletValve, 19, i, i < FirstInlets);
        for (i = 0; i <= FirstVisibNum; i++) WR(EnableOutletValve, 19, i, i < FirstOutlets);
    } else {
        for (i = 0; i <= FirstVisibNum; i++) WR(EnableInletValve, 19, i, i < FirstInlets || i >= ((FirstVisibNum + 1) - SecondInlets));
        for (i = 0; i <= FirstVisibNum; i++) WR(EnableOutletValve, 19, i, i < FirstOutlets || i >= ((FirstVisibNum + 1) - SecondOutlets));
    }
    /* Step 2, interconnection 1. Reset on a new press while a pair is complete (91-113). */
    if (SelectedFirstInlet != 0 && SelectedFirstOutlet != 0) {
        for (i = 0; i <= FirstInlets - 1; i++)
            if (RD(InletValveTouch, 19, i) && (i + 1) != SelectedFirstInlet) {
                SelectedFirstInlet = 0; SelectedFirstOutlet = 0;
                for (j = 0; j <= FirstInlets - 1; j++) WR(EnableInletValve, 19, j, true);
                for (j = 0; j <= FirstOutlets - 1; j++) WR(EnableOutletValve, 19, j, true);
                break;                                                          /* EXIT */
            }
        for (i = 0; i <= FirstOutlets - 1; i++)
            if (RD(OutletValveTouch, 19, i) && (i + 1) != SelectedFirstOutlet) {
                SelectedFirstInlet = 0; SelectedFirstOutlet = 0;
                for (j = 0; j <= FirstInlets - 1; j++) WR(EnableInletValve, 19, j, true);
                for (j = 0; j <= FirstOutlets - 1; j++) WR(EnableOutletValve, 19, j, true);
                break;
            }
    }
    if (SelectedFirstInlet == 0)                                                /* 116-122: the last pressed index wins */
        for (i = 0; i <= FirstInlets - 1; i++) if (RD(InletValveTouch, 19, i) && RD(EnableInletValve, 19, i)) SelectedFirstInlet = (int16_t)(i + 1);
    if (SelectedFirstInlet != 0)                                                /* 124-130 */
        for (i = 0; i <= FirstInlets - 1; i++) if ((i + 1) != SelectedFirstInlet) WR(EnableInletValve, 19, i, false);
    if (SelectedFirstOutlet == 0)                                               /* 133-139 */
        for (i = 0; i <= FirstOutlets - 1; i++) if (RD(OutletValveTouch, 19, i) && RD(EnableOutletValve, 19, i)) SelectedFirstOutlet = (int16_t)(i + 1);
    if (SelectedFirstOutlet != 0)                                               /* 140-146 */
        for (i = 0; i <= FirstOutlets - 1; i++) if ((i + 1) != SelectedFirstOutlet) WR(EnableOutletValve, 19, i, false);
    if (SelectedFirstInlet != 0 && SelectedFirstOutlet != 0) {                  /* 153-166: pair complete, selection kept */
        for (i = 0; i <= FirstInlets - 1; i++) { WR(EnableInletValve, 19, i, true); WR(InletValveTouch, 19, i, false); }
        for (i = 0; i <= FirstOutlets - 1; i++) { WR(EnableOutletValve, 19, i, true); WR(OutletValveTouch, 19, i, false); }
    }
    /* Step 3, interconnection 2 (169-254), reversed numbering: index FirstVisibNum is valve 1 */
    if (NumInterconnections == 2) {
        idxStartInlet2 = (int16_t)((FirstVisibNum + 1) - SecondInlets);
        idxStartOutlet2 = (int16_t)((FirstVisibNum + 1) - SecondOutlets);
        if (SelectedSecondInlet != 0 && SelectedSecondOutlet != 0) {            /* 179-201 */
            for (i = idxStartInlet2; i <= FirstVisibNum; i++)
                if (RD(InletValveTouch, 19, i) && ((FirstVisibNum - i) + 1) != SelectedSecondInlet) {
                    SelectedSecondInlet = 0; SelectedSecondOutlet = 0;
                    for (j = idxStartInlet2; j <= FirstVisibNum; j++) WR(EnableInletValve, 19, j, true);
                    for (j = idxStartOutlet2; j <= FirstVisibNum; j++) WR(EnableOutletValve, 19, j, true);
                    break;
                }
            for (i = idxStartOutlet2; i <= FirstVisibNum; i++)
                if (RD(OutletValveTouch, 19, i) && ((FirstVisibNum - i) + 1) != SelectedSecondOutlet) {
                    SelectedSecondInlet = 0; SelectedSecondOutlet = 0;
                    for (j = idxStartInlet2; j <= FirstVisibNum; j++) WR(EnableInletValve, 19, j, true);
                    for (j = idxStartOutlet2; j <= FirstVisibNum; j++) WR(EnableOutletValve, 19, j, true);
                    break;
                }
        }
        if (SelectedSecondInlet == 0)                                           /* 206-212 */
            for (i = idxStartInlet2; i <= FirstVisibNum; i++) if (RD(InletValveTouch, 19, i) && RD(EnableInletValve, 19, i)) SelectedSecondInlet = (int16_t)((FirstVisibNum - i) + 1);
        if (SelectedSecondInlet != 0)                                           /* 213-219 */
            for (i = idxStartInlet2; i <= FirstVisibNum; i++) if (((FirstVisibNum - i) + 1) != SelectedSecondInlet) WR(EnableInletValve, 19, i, false);
        if (SelectedSecondOutlet == 0)                                          /* 224-230 */
            for (i = idxStartOutlet2; i <= FirstVisibNum; i++) if (RD(OutletValveTouch, 19, i) && RD(EnableOutletValve, 19, i)) SelectedSecondOutlet = (int16_t)((FirstVisibNum - i) + 1);
        if (SelectedSecondOutlet != 0)                                          /* 231-237 */
            for (i = idxStartOutlet2; i <= FirstVisibNum; i++) if (((FirstVisibNum - i) + 1) != SelectedSecondOutlet) WR(EnableOutletValve, 19, i, false);
        if (SelectedSecondInlet != 0 && SelectedSecondOutlet != 0) {            /* 240-253 */
            for (i = idxStartInlet2; i <= FirstVisibNum; i++) { WR(EnableInletValve, 19, i, true); WR(InletValveTouch, 19, i, false); }
            for (i = idxStartOutlet2; i <= FirstVisibNum; i++) { WR(EnableOutletValve, 19, i, true); WR(OutletValveTouch, 19, i, false); }
        }
    }
}
