/* 4A_new.c - proposed C for the new STM32 firmware, replacing the Stage 4A utilities.
 * Plain C library calls instead of the ladder byte-array loops. Each function says which
 * original it replaces and where its output deliberately differs (41_Utilities.md section 4).
 */
#include <stdio.h>
#include <string.h>
#include "4A.h"

/* Replaces Num to ASCII(value, RL 3, Format 0, Padding 2): the original gives a space
 * then two digits, e.g. 9 -> " 09". Same output. Values 0..99 (RTC fields). */
void util_two_digit_field(unsigned value, char out[4])
{
    snprintf(out, 4, " %02u", value % 100u);
}

/* Replaces Num to ASCII(year, RL 4, Format 0, Padding 0): "2026". Same output for
 * 1000..9999. */
void util_year_field(unsigned year, char out[5])
{
    snprintf(out, 5, "%04u", year % 10000u);
}

/* Replaces ConvertIntToString(value, 2). Same output for 0..99 (valve numbers 1..20).
 * The original keeps only the last 2 digits of larger numbers and drops the sign;
 * this prints the whole number. */
void util_int_to_string(int value, char *out, unsigned cap)
{
    snprintf(out, cap, "%d", value);
}

/* Replaces ConvertRealOrIntToString. Fixed number of decimals, a setting; 2 by owner decision 18 Oct.
 * Deliberately different from the original, which loses digits with the project's
 * settings (DecimPart 4, FractPart 3); see 41_Utilities.md finding F3. */
void util_real_to_string(float value, unsigned decimals, char *out, unsigned cap)
{
    snprintf(out, cap, "%.*f", (int)decimals, (double)value);
}

/* Replaces SearchInArray. Same result, including 25 when nothing is found. */
int util_first_index_above(const uint8_t a[20], int max_value)
{
    for (int i = 0; i < 20; i++) if (a[i] > max_value) return i;
    return 25;
}

/* Replaces FillNumbers / FillNumbersMUX (one call per array). */
void util_fill_1_to_20(uint8_t a[20])
{
    for (int i = 0; i < 20; i++) a[i] = (uint8_t)(i + 1);
}

/* Replaces CheckIfVacant. col 0..19 = Col3..Col22. Same output; for a column number
 * outside 0..19 it returns "" as the original does under assumption A4.3. */
void util_log_column_text(int col, const LoggingTableStruct_t *row, char out[18 + 1])
{
    out[0] = 0;
    if (col < 0 || col > 19) return;
    const char *cols[20] = { row->Col3, row->Col4, row->Col5, row->Col6, row->Col7, row->Col8,
        row->Col9, row->Col10, row->Col11, row->Col12, row->Col13, row->Col14, row->Col15,
        row->Col16, row->Col17, row->Col18, row->Col19, row->Col20, row->Col21, row->Col22 };
    const char *src = cols[col];
    snprintf(out, 19, "%s", src[0] ? src : "empty");
}
