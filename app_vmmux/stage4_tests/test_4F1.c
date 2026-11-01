/* test_4F1.c - tests for 4F1_faithful.c. Build: gcc -std=c11 -Wall -Wextra test_4F1.c 4F1_faithful.c */
#include <stdio.h>
#include <string.h>
#include "4F1.h"

uint32_t g_dt_ms = 100; int g_oob;
bool DOList[20], InitialDO[20], DOFinal[20], BackFlushExists, BackFlushOptionVisib, FlushValveBit, FlushValveBitModbus;
int16_t BackFlushDONumber;
uint8_t UserDelim[4], TempBuffer[24]; int16_t TempStringLength;
bool LoadingWindow, BitOne, BitTwo, BitThree; ul_timer_t LoadingTimer = {2000, 2000, false, false};
int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;
int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
uint8_t TubeColor1[57], TubeColor2[55];
bool InletValveTouch[19], OutletValveTouch[19], EnableInletValve[19], EnableOutletValve[19];

static int fails, checks;
static void eqi(const char *w, long g, long x) { checks++; if (g != x) { fails++; printf("FAIL %-66s got %ld want %ld\n", w, g, x); } }
static int count_true(const bool *a, int n) { int c = 0; for (int i = 0; i < n; i++) c += a[i]; return c; }
static void sel_reset(void) { SelectedFirstInlet = SelectedFirstOutlet = SelectedSecondInlet = SelectedSecondOutlet = 0;
    memset(InletValveTouch, 0, 19); memset(OutletValveTouch, 0, 19); memset(EnableInletValve, 0, 19); memset(EnableOutletValve, 0, 19); }
static void touch_in(int i)  { InletValveTouch[i] = true;  FB_ValveSelectionInter(); }   /* screen SetBit, then Function1 rung 13 */
static void touch_out(int i) { OutletValveTouch[i] = true; FB_ValveSelectionInter(); }

int main(void) {
    /* ---------- TwoORFb_22 / TwoORFb_10 ---------- */
    DOList[3] = true; InitialDO[5] = true; TwoORFb_22();
    eqi("TwoORFb_22: DOList[3] -> DOFinal[3]", DOFinal[3], 1);
    eqi("TwoORFb_22: InitialDO[5] -> DOFinal[5]", DOFinal[5], 1);
    eqi("TwoORFb_22: only those two on", count_true(DOFinal, 20), 2);
    DOList[3] = InitialDO[5] = false;
    BackFlushExists = BackFlushOptionVisib = true; BackFlushDONumber = 7; FlushValveBitModbus = true; TwoORFb_22();
    eqi("TwoORFb_22: Modbus flush drives the back-flush output (number 7 = DOFinal[7])", DOFinal[7], 1);
    eqi("TwoORFb_22: ... and nothing else", count_true(DOFinal, 20), 1);
    BackFlushOptionVisib = false; TwoORFb_22();
    eqi("V1 back-flush needs the screen visibility bit BackFlushOptionVisib", DOFinal[7], 0);
    BackFlushOptionVisib = true; BackFlushDONumber = 20; TwoORFb_22();
    eqi("TwoORFb_22: back-flush number 20 drives no output", count_true(DOFinal, 20), 0);
    BackFlushDONumber = 7; { bool o[10]; TwoORFb_10(o);
        eqi("TwoORFb_10 (never called): ignores the Modbus flush bit", o[7], 0);
        FlushValveBit = true; TwoORFb_10(o); eqi("TwoORFb_10: local flush bit works", o[7], 1); }
    FlushValveBit = FlushValveBitModbus = false; BackFlushExists = BackFlushOptionVisib = false;

    /* ---------- ScheduleCSVAdds ---------- */
    memset(TempBuffer, 'x', 24); TempStringLength = 5; ScheduleCSVAdds();
    eqi("CSVAdds: CR at TempStringLength", TempBuffer[5], 13);
    eqi("CSVAdds: LF after it", TempBuffer[6], 10);
    eqi("CSVAdds: length + 2", TempStringLength, 7);
    eqi("CSVAdds: byte before untouched", TempBuffer[4], 'x');
    g_oob = 0; TempStringLength = 23; ScheduleCSVAdds();
    eqi("V2 length 23: only the CR fits (A4.1 cut)", TempBuffer[23], 13);
    eqi("V2 ... but the length still becomes 25", TempStringLength, 25);
    g_oob = 0; TempStringLength = 24; ScheduleCSVAdds(); eqi("V2 length 24: nothing copied", g_oob, 1);

    /* ---------- Loading ---------- */
    LoadingWindow = true; int first_three_off = -1, first_all_on_again = -1, cyc = 0; bool was_all = true;
    for (int s = 0; s < 60; s++) { Loading();
        bool all = BitOne && BitTwo && BitThree;
        if (!BitThree && first_three_off < 0) first_three_off = s;
        if (all && !was_all && first_all_on_again < 0) first_all_on_again = s;
        if (all && !was_all) cyc++;
        was_all = all; }
    eqi("V3 dots start all on and BitThree goes off first (scan 5)", first_three_off, 5);
    eqi("V3 ... all three back on at scan 20 (out at 19, reload scan 20)", first_all_on_again, 20);
    LoadingWindow = false; Loading();
    eqi("Loading: window closed, all bits off", BitOne + BitTwo + BitThree, 0);
    eqi("Loading: timer reloaded to 2000", LoadingTimer.Current, 2000);

    /* ---------- Route: a hand-worked case ---------- */
    NumInterconnections = 1; FirstInlets = 3; FirstOutlets = 3; SelectedFirstInlet = 2; SelectedFirstOutlet = 3;
    memset(TubeColor1, 9, 57); memset(TubeColor2, 9, 55); Route();
    { const uint8_t want1[8] = {1, 0, 1, 1, 1, 0, 0, 0}; int ok = 1; for (int i = 0; i < 8; i++) ok &= TubeColor1[i] == want1[i];
      eqi("Route: inlet 2 of 3 -> TubeColor1[0..7] = 1 0 1 1 1 0 0 0", ok, 1); }
    { const uint8_t want2[9] = {0, 1, 1, 0, 1, 1, 1, 0, 0}; int ok = 1; for (int i = 0; i < 9; i++) ok &= TubeColor2[i] == want2[i];
      eqi("Route: outlet 3 of 3 -> TubeColor2[0..8] = 0 1 1 0 1 1 1 0 0", ok, 1); }
    eqi("V4 small layout: entries above 26 are not cleared", TubeColor1[27], 9);
    NumInterconnections = 2; SecondInlets = 3; SecondOutlets = 3; SelectedSecondInlet = 2; SelectedSecondOutlet = 1;
    SelectedFirstInlet = SelectedFirstOutlet = 0; memset(TubeColor1, 9, 57); Route();
    { int on[57], n = 0; for (int i = 0; i <= 26; i++) if (TubeColor1[i] == 1) on[n++] = i;
      eqi("Route: 2nd interconnection inlet 2 of 3 -> 5 entries lit", n, 5);
      eqi("Route: ... they are 21 22 23 24 26", n == 5 && on[0] == 21 && on[1] == 22 && on[2] == 23 && on[3] == 24 && on[4] == 26, 1); }
    /* ---------- Route: every valid layout and selection, out-of-range count ---------- */
    long bad_route = 0, worst = 0;
    for (int ni = 1; ni <= 2; ni++) for (int fi = 1; fi <= 19; fi++) for (int fo = 1; fo <= 19; fo++)
      for (int si = (ni == 2); si <= (ni == 2 ? 19 - fi : 0); si++) for (int so = (ni == 2); so <= (ni == 2 ? 19 - fo : 0); so++) {
        NumInterconnections = (int16_t)ni; FirstInlets = (int16_t)fi; FirstOutlets = (int16_t)fo; SecondInlets = (int16_t)si; SecondOutlets = (int16_t)so;
        for (int a = 0; a <= fi; a++) { g_oob = 0; SelectedFirstInlet = (int16_t)a; SelectedFirstOutlet = (int16_t)(a <= fo ? a : fo);
            SelectedSecondInlet = (int16_t)(si ? 1 + a % si : 0); SelectedSecondOutlet = (int16_t)(so ? 1 + a % so : 0);
            Route(); if (g_oob) { bad_route++; if (g_oob > worst) worst = g_oob; } } }
    eqi("Route sweep: no index out of range in any valid layout", bad_route, 0);

    /* ---------- FB_ValveSelectionInter: one interconnection ---------- */
    NumInterconnections = 1; FirstInlets = 3; FirstOutlets = 3; SecondInlets = SecondOutlets = 0; sel_reset();
    touch_in(1);
    eqi("Select: inlet index 1 -> SelectedFirstInlet 2", SelectedFirstInlet, 2);
    eqi("Select: other inlets disabled", EnableInletValve[0] + EnableInletValve[2], 0);
    eqi("Select: invisible inlet 3 disabled", EnableInletValve[3], 0);
    touch_in(2);
    eqi("V5 a second inlet press before the outlet is ignored", SelectedFirstInlet, 2);
    eqi("V5 ... and its touch bit stays set", InletValveTouch[2], 1);
    touch_out(0);
    eqi("Select: outlet 1 completes the pair", SelectedFirstOutlet, 1);
    eqi("Select: pair complete clears all touch bits", count_true(InletValveTouch, 19) + count_true(OutletValveTouch, 19), 0);
    eqi("Select: pair complete re-enables the visible valves", count_true(EnableInletValve, 19) + count_true(EnableOutletValve, 19), 6);
    eqi("Select: the pair is kept after completion", SelectedFirstInlet * 10 + SelectedFirstOutlet, 21);
    touch_in(0);
    eqi("Select: a new inlet press starts a new pair with that inlet", SelectedFirstInlet, 1);
    eqi("Select: ... and clears the outlet", SelectedFirstOutlet, 0);
    /* ---------- two interconnections, numbered from the end ---------- */
    NumInterconnections = 2; FirstInlets = 3; FirstOutlets = 3; SecondInlets = 2; SecondOutlets = 2; sel_reset();
    touch_in(8);
    eqi("Select 2: index 8 is inlet 1 of interconnection 2", SelectedSecondInlet, 1);
    eqi("Select 2: interconnection 1 untouched", SelectedFirstInlet, 0);
    touch_out(7);
    eqi("Select 2: index 7 is outlet 2 of interconnection 2", SelectedSecondOutlet, 2);
    eqi("Select 2: hidden middle valves stay disabled", EnableInletValve[4], 0);
    /* ---------- NumInterconnections other than 1 or 2 (V6) ---------- */
    NumInterconnections = 0; FirstInlets = 3; FirstOutlets = 3; SecondInlets = SecondOutlets = 0; sel_reset();
    touch_in(1);
    eqi("V6 count 0: only enable[0] is refreshed; index 1 keeps its old OFF, press ignored", SelectedFirstInlet, 0);
    /* ---------- sweep: every valid layout, every single press ---------- */
    long bad_sel = 0;
    for (int ni = 1; ni <= 2; ni++) for (int fi = 1; fi <= 19; fi++) for (int fo = 1; fo <= 19; fo++)
      for (int si = (ni == 2); si <= (ni == 2 ? 19 - fi : 0); si++) for (int so = (ni == 2); so <= (ni == 2 ? 19 - fo : 0); so++)
        for (int p = 0; p < 19; p++) {
            NumInterconnections = (int16_t)ni; FirstInlets = (int16_t)fi; FirstOutlets = (int16_t)fo; SecondInlets = (int16_t)si; SecondOutlets = (int16_t)so;
            sel_reset(); g_oob = 0; touch_in(p); touch_out(p); if (g_oob) bad_sel++; }
    eqi("Selection sweep: no index out of range in any valid layout", bad_sel, 0);

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
