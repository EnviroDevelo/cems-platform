/* test_4B.c - hand-worked tests and property checks for the Stage 4B functions.
 * Build: gcc -std=c11 -Wall -Wextra test_4B.c 4B_faithful.c */
#include <stdio.h>
#include <string.h>
#include "4B.h"

int16_t NumberOfMUXValves; bool ConfigWith10, ConfigWith20;
bool SingularInletVisib[19], SingularInletVisib1[19], SingularInletVisib2[19];
int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;
bool TubeVisibility1[39], TubeVisibility2[37];
int16_t DOInlet[19], DOOutlet[19], BackFlushDONumber;
bool ColumnIsntVacantArray[20], ParameterColNumberLoggingArray[20];
int16_t ParameterColNumberArray[20];

static int fails, checks;
static void eqi(const char *w, long g, long x) { checks++; if (g != x) { fails++; printf("FAIL %-50s got %ld want %ld\n", w, g, x); } }
static void eqs(const char *w, const bool *a, int n, const char *want) {
    char got[64]; for (int i = 0; i < n; i++) got[i] = a[i] ? '1' : '0'; got[n] = 0;
    checks++; if (strcmp(got, want)) { fails++; printf("FAIL %-50s\n  got  %s\n  want %s\n", w, got, want); }
}
static void adj(int16_t fi, int16_t fo, int16_t si, int16_t so, int16_t ch, int16_t ni, int16_t mx,
                int16_t efi, int16_t efo, int16_t esi, int16_t eso, const char *w) {
    int16_t a, b, c, d; AdjustNumberOfValves(fi, fo, si, so, ch, ni, mx, &a, &b, &c, &d);
    char t[96]; snprintf(t, sizeof t, "%s: got %d,%d,%d,%d", w, a, b, c, d);
    eqi(t, a == efi && b == efo && c == esi && d == eso, 1);
}

int main(void) {
    /* AdjustNumberOfValves: hand-worked */
    adj(5, 4, 3, 3, 1, 1, 10,   5, 4, 0, 0, "1 tube within max: second zeroed");
    adj(8, 6, 0, 0, 1, 1, 10,   8, 2, 0, 0, "1 tube, inlets changed: outlets cut first");
    adj(8, 6, 0, 0, 2, 1, 10,   4, 6, 0, 0, "1 tube, outlets changed: inlets cut first");
    adj(15, 9, 0, 0, 1, 1, 10,  9, 1, 0, 0, "1 tube, both cut, outlets stop at 1");
    adj(8, 6, 0, 0, 3, 1, 10,   8, 6, 0, 0, "1 tube, ChangedParameter 3: no change (over max)");
    adj(5, 5, 4, 4, 1, 2, 10,   5, 3, 1, 1, "2 tubes, FI changed: SI, SO, then FO cut");
    adj(5, 5, 4, 4, 3, 2, 10,   1, 1, 4, 4, "2 tubes, SI changed: FI, FO cut first");
    adj(5, 5, 4, 4, 4, 2, 12,   1, 3, 4, 4, "2 tubes, SO changed, max 12");
    adj(5, 5, 4, 4, 0, 2, 10,   5, 5, 4, 4, "2 tubes, ChangedParameter 0: no change");
    adj(1, 1, 1, 1, 1, 2, 3,    1, 1, 1, 1, "2 tubes, max 3 below minimum 4: stays over");
    adj(7, 7, 7, 7, 1, 3, 10,   7, 7, 7, 7, "NumInterconnections 3: copied unchanged");

    /* property: for 1..2 tubes, every result is >= 1 where the input was >= 1, and the
       sum is <= max whenever max >= the number of tubes x 2 and ChangedParameter matches */
    int bad = 0;
    for (int ni = 1; ni <= 2; ni++) for (int mx = 4; mx <= 20; mx++) for (int ch = 1; ch <= 2 * ni; ch++)
    for (int fi = 1; fi <= 19; fi++) for (int fo = 1; fo <= 19; fo++) for (int si = 1; si <= (ni == 2 ? 19 : 1); si++) for (int so = 1; so <= (ni == 2 ? 19 : 1); so++) {
        int16_t a, b, c, d; AdjustNumberOfValves(fi, fo, si, so, ch, ni, mx, &a, &b, &c, &d);
        int sum = a + b + (ni == 2 ? c + d : 0);
        if (sum > mx || a < 1 || b < 1 || (ni == 2 && (c < 1 || d < 1))) bad++;
        if (ni == 1 && (c != 0 || d != 0)) bad++;
    }
    eqi("AdjustNumberOfValves property over all settings", bad, 0);

    /* AdjustNumberOfValvesMUX */
    NumberOfMUXValves = 5; ConfigWith20 = true; ConfigWith10 = false;
    memset(SingularInletVisib1, 0, sizeof SingularInletVisib1);
    AdjustNumberOfValvesMUX();
    eqs("MUX 5 valves -> 4 singular inlets", SingularInletVisib, 19, "1111000000000000000");
    eqs("  ConfigWith20 copy", SingularInletVisib2, 19, "1111000000000000000");
    eqs("  ConfigWith10 copy untouched", SingularInletVisib1, 19, "0000000000000000000");
    NumberOfMUXValves = 1; AdjustNumberOfValvesMUX();
    eqs("MUX 1 valve -> clamped to 1", SingularInletVisib, 19, "1000000000000000000");
    NumberOfMUXValves = 25; ConfigWith20 = false; ConfigWith10 = true; AdjustNumberOfValvesMUX();
    eqs("MUX 25 valves -> clamped to 19", SingularInletVisib1, 19, "1111111111111111111");

    /* TubeVisibility */
    g_out_of_range_writes = 0;
    memset(TubeVisibility1, 1, sizeof TubeVisibility1); memset(TubeVisibility2, 1, sizeof TubeVisibility2);
    NumInterconnections = 1; FirstInlets = 3; FirstOutlets = 2; TubeVisibility();
    eqs("1 tube 3 in 2 out (left, stale above 18 kept)", TubeVisibility1, 39, "111111000000000000011111111111111111111");
    eqs("  right (stale above 16 kept)", TubeVisibility2, 37, "1110000000000000011111111111111111111");
    memset(TubeVisibility1, 0, sizeof TubeVisibility1); memset(TubeVisibility2, 0, sizeof TubeVisibility2);
    NumInterconnections = 2; FirstInlets = 2; SecondInlets = 3; FirstOutlets = 2; SecondOutlets = 1; TubeVisibility();
    eqs("2 tubes 2+3 in (left)", TubeVisibility1, 39, "111100000000011111100000000000000000000");
    eqs("  2+1 out (right)", TubeVisibility2, 37, "1110000000000000100000000000000000000");
    NumInterconnections = 2; FirstInlets = 6; SecondInlets = 5; FirstOutlets = 4; SecondOutlets = 4; TubeVisibility();
    eqs("2 tubes 6+5 in (large layout)", TubeVisibility1, 39, "111111111111000000000000000001111111111");
    eqs("  4+4 out (large layout)", TubeVisibility2, 37, "1111111000000000000000000000001111111");
    /* out-of-range scan over all valid settings */
    for (int ni = 1; ni <= 2; ni++) for (int a = 0; a <= 19; a++) for (int b = 0; b <= 19; b++)
    for (int c = 0; c <= (ni == 2 ? 19 : 0); c++) for (int d = 0; d <= (ni == 2 ? 19 : 0); d++) {
        if (a + b + c + d > 20) continue;
        NumInterconnections = ni; FirstInlets = a; FirstOutlets = b; SecondInlets = c; SecondOutlets = d; TubeVisibility();
    }
    eqi("TubeVisibility out-of-range writes, all settings with total <= 20", g_out_of_range_writes, 0);

    /* ResetDOassigningOver10 */
    for (int i = 0; i < 19; i++) { DOInlet[i] = (int16_t)i; DOOutlet[i] = (int16_t)(18 - i); }
    BackFlushDONumber = 12; ResetDOassigningOver10();
    eqi("DOInlet[9] kept", DOInlet[9], 9); eqi("DOInlet[10] reset", DOInlet[10], 0);
    eqi("DOOutlet[0] (18) reset", DOOutlet[0], 0); eqi("DOOutlet[9] (9) kept", DOOutlet[9], 9);
    eqi("BackFlushDONumber 12 reset", BackFlushDONumber, 0);

    /* ResetCertainCheckbox */
    for (int i = 0; i < 20; i++) { ParameterColNumberArray[i] = (int16_t)(i + 3); ParameterColNumberLoggingArray[i] = true; ColumnIsntVacantArray[i] = false; }
    ParameterColNumberArray[7] = 5;               /* params 2 and 7 both want column 5 */
    ColumnIsntVacantArray[7] = true;              /* user ticked param 7 */
    ResetCertainCheckbox();
    eqi("param 7 logging on", ParameterColNumberLoggingArray[7], 1);
    eqi("param 2 (same column) logging off", ParameterColNumberLoggingArray[2], 0);
    eqi("param 3 untouched", ParameterColNumberLoggingArray[3], 1);
    eqi("flag cleared", ColumnIsntVacantArray[7], 0);
    ColumnIsntVacantArray[2] = true; ColumnIsntVacantArray[7] = true;   /* both flagged: last wins */
    ResetCertainCheckbox();
    eqi("both flagged: param 2 off", ParameterColNumberLoggingArray[2], 0);
    eqi("both flagged: param 7 on", ParameterColNumberLoggingArray[7], 1);

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
