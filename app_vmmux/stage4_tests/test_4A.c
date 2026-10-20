/* test_4A.c - tests for the Stage 4A utilities.
 * Part 1: faithful C against hand-worked results (worked rung by rung in 41_Utilities.md).
 * Part 2: new-firmware C against faithful C over every input range the project uses.
 * Build: gcc -std=c11 -Wall -Wextra -O1 test_4A.c 4A_faithful.c 4A_new.c -lm
 */
#include <stdio.h>
#include <string.h>
#include "4A.h"
#include "4D.h"

static int fails, checks;
static void eq(const char *what, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want)) { fails++; printf("FAIL %-44s got \"%s\" want \"%s\"\n", what, got, want); }
}
static void eqi(const char *what, long got, long want) {
    checks++;
    if (got != want) { fails++; printf("FAIL %-44s got %ld want %ld\n", what, got, want); }
}

Sequences_t Sequence1, Sequence2; ScheduleMUX_t SequenceMUX;   /* app_vmmux: globals FillNumbers writes */
int main(void) {
    char s4[5], s8[9], s12[13], s19[19 + 1], n[32]; int16_t st;

    /* ---- Part 1: hand-worked ---- */
    Num_to_ASCII(9, 3, 0, 2, 0, s4, &st);     eq("Num to ASCII 9, RL3 pad2 (hour)", s4, " 09"); eqi("  status", st, 0);
    Num_to_ASCII(23, 3, 0, 2, 0, s4, &st);    eq("Num to ASCII 23, RL3 pad2", s4, " 23");
    Num_to_ASCII(0, 3, 0, 2, 0, s4, &st);     eq("Num to ASCII 0, RL3 pad2 (minute 0)", s4, " 00");
    Num_to_ASCII(2026, 4, 0, 0, 0, s4, &st);  eq("Num to ASCII 2026, RL4 pad0 (year)", s4, "2026");
    Num_to_ASCII(5, 12, 0, 0, 0, s4, &st);    eq("Num to ASCII RL12 -> status -1, empty", s4, ""); eqi("  status", st, -1);
    Num_to_ASCII(5, 3, 3, 0, 0, s4, &st);     eqi("Num to ASCII format 3 -> status -2", st, -2);
    Num_to_ASCII(5, 3, 0, 3, 0, s4, &st);     eqi("Num to ASCII padding 3 -> status -3", st, -3);
    Num_to_ASCII(5, 3, 0, 0, 3, s4, &st);     eqi("Num to ASCII DP 3 >= RL 3 -> status -4", st, -4);
    Num_to_ASCIIINT(-42, 4, 1, 1, 0, s8, &st); eq("Num to ASCIIINT -42 RL4 fmt1 pad1", s8, "- 42");
    Num_to_ASCIIINT(42, 4, 2, 2, 0, s8, &st);  eq("Num to ASCIIINT 42 RL4 fmt2 pad2", s8, "+042");
    Num_to_ASCIIINT(1234, 5, 0, 2, 2, s8, &st); eq("Num to ASCIIINT 1234 RL5 DP2 pad2", s8, " 2.34");
    Num_to_ASCIIINT(-42, 4, 1, 0, 0, s8, &st); eq("Num to ASCIIINT -42 RL4 fmt1 pad0", s8, "-42");

    ConvertIntToString(5, 2, s8);    eq("ConvertIntToString 5", s8, "5");
    ConvertIntToString(15, 2, s8);   eq("ConvertIntToString 15", s8, "15");
    ConvertIntToString(20, 2, s8);   eq("ConvertIntToString 20", s8, "20");
    ConvertIntToString(0, 2, s8);    eq("ConvertIntToString 0", s8, "0");
    ConvertIntToString(123, 2, s8);  eq("ConvertIntToString 123 (keeps last 2)", s8, "23");
    ConvertIntToString(-7, 2, s8);   eq("ConvertIntToString -7 (sign lost)", s8, "7");
    ConvertIntToString(100, 2, s8);  eq("ConvertIntToString 100 (all zeros -> empty)", s8, "");

    uint16_t u[9];
    ConvertIntToStringUTF16(17, 2, u);
    eqi("ConvertIntToStringUTF16 17 [0]", u[0], '1'); eqi("  [1]", u[1], '7'); eqi("  [2]", u[2], 0);

    /* project settings: DecimPart 4, FractPart 3 */
    ConvertRealOrIntToString(12.5f, 4, 3, s12);    eq("Real 12.5 (4,3)", s12, "12.");
    ConvertRealOrIntToString(12.3456f, 4, 3, s12); eq("Real 12.3456 (4,3)", s12, "12.456");
    ConvertRealOrIntToString(12.05f, 4, 3, s12);   eq("Real 12.05 (4,3)", s12, "12.00");
    ConvertRealOrIntToString(12.0f, 4, 3, s12);    eq("Real 12.0 (4,3)", s12, "12");
    ConvertRealOrIntToString(0.0f, 4, 3, s12);     eq("Real 0.0 (4,3)", s12, "0");
    /* the pairing the rungs appear designed for: DecimalNum 2, FractionLength 3 */
    ConvertRealOrIntToString(12.05f, 2, 3, s12);   eq("Real 12.05 (2,3)", s12, "12.05");
    ConvertRealOrIntToString(12.5f, 2, 3, s12);    eq("Real 12.5 (2,3)", s12, "12.50");
    ConvertRealOrIntToString(-3.25f, 2, 3, s12);   eq("Real -3.25 (2,3)", s12, "-3.25");
    ConvertRealOrIntToString(-0.5f, 2, 3, s12);    eq("Real -0.5 (2,3)", s12, "-0.50");
    ConvertRealOrIntToString(0.001f, 2, 3, s12);   eq("Real 0.001 (2,3) both parts 0", s12, "0");

    uint8_t a[20] = {0};
    eqi("SearchInArray none -> 25", SearchInArray(a, 0), 25);
    a[7] = 3; a[9] = 9;
    eqi("SearchInArray first > 2", SearchInArray(a, 2), 7);
    eqi("SearchInArray first > 3", SearchInArray(a, 3), 9);
    eqi("SearchInArray max -1", SearchInArray(a, -1), 0);

    /* app_vmmux: FillNumbers / FillNumbersMUX write the globals, as in the original (41g) */
    FillNumbers(); eqi("FillNumbers [0]", Sequence1.NumberArray[0], 1); eqi("FillNumbers [19]", Sequence2.NumberArray[19], 20);
    FillNumbersMUX(); eqi("FillNumbersMUX [10]", SequenceMUX.NumberArray[10], 11);

    LoggingTableStruct_t row; memset(&row, 0, sizeof row);
    strcpy(row.Col3, "NOx ppm"); strcpy(row.Col22, "Flow");
    CheckIfVacant(0, &row, s19);  eq("CheckIfVacant 0 -> Col3", s19, "NOx ppm");
    CheckIfVacant(19, &row, s19); eq("CheckIfVacant 19 -> Col22", s19, "Flow");
    CheckIfVacant(5, &row, s19);  eq("CheckIfVacant 5 (blank)", s19, "empty");
    CheckIfVacant(20, &row, s19); eq("CheckIfVacant 20 (out of range)", s19, "");
    CheckIfVacant(-1, &row, s19); eq("CheckIfVacant -1 (ConflictingIndex start)", s19, "");
    int p1 = fails; printf("Part 1: %d checks, %d failures\n", checks, fails);

    /* ---- Part 2: new vs faithful over the ranges used ---- */
    int c2 = checks, diff = 0;
    for (unsigned v = 0; v <= 99; v++) {            /* RTC month/day/hour/minute */
        Num_to_ASCII(v, 3, 0, 2, 0, s4, &st); util_two_digit_field(v, n); eq("two-digit field", n, s4);
    }
    for (unsigned y = 1000; y <= 9999; y++) {       /* RTC year */
        Num_to_ASCII(y, 4, 0, 0, 0, s4, &st); util_year_field(y, n); eq("year field", n, s4);
    }
    for (int v = 0; v <= 99; v++) {                 /* valve numbers */
        ConvertIntToString((int16_t)v, 2, s8); util_int_to_string(v, n, sizeof n); eq("int to string", n, s8);
    }
    for (int v = 100; v <= 32767; v++) {            /* outside the project's range: count differences */
        ConvertIntToString((int16_t)v, 2, s8); util_int_to_string(v, n, sizeof n); if (strcmp(n, s8)) diff++;
    }
    for (int k = 0; k < 100000; k++) {              /* SearchInArray, pseudo-random */
        uint32_t r = 2463534242u ^ (uint32_t)k * 2654435761u; uint8_t x[20];
        for (int i = 0; i < 20; i++) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; x[i] = (uint8_t)(r % 22); }
        int m = (int)(r % 23) - 1;
        eqi("first index above", util_first_index_above(x, m), SearchInArray(x, (int16_t)m));
    }
    for (int col = -2; col <= 21; col++) {
        CheckIfVacant((int16_t)col, &row, s19); util_log_column_text(col, &row, n); eq("log column text", n, s19);
    }
    printf("Part 2: %d checks, %d failures (ConvertIntToString 100..32767: new differs from original in %d of 32668, by design)\n",
           checks - c2, fails - p1, diff);

    /* ---- Table for the spec: ConvertRealOrIntToString, original vs new ---- */
    float tv[] = {12.5f, 12.3456f, 12.05f, 12.0f, 0.25f, -3.25f, 101.007f, 0.0f};
    printf("\n| Value | Original, project settings (4,3) | Original, (2,3) | New, 2 decimals |\n|---|---|---|---|\n");
    for (unsigned i = 0; i < sizeof tv / sizeof tv[0]; i++) {
        char o1[13], o2[13], nw[24];
        ConvertRealOrIntToString(tv[i], 4, 3, o1); ConvertRealOrIntToString(tv[i], 2, 3, o2);
        util_real_to_string(tv[i], 2, nw, sizeof nw);
        printf("| %g | `%s` | `%s` | `%s` |\n", tv[i], o1, o2, nw);
    }
    printf("\nTOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
