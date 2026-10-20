/* 4A.h - prototypes for the Stage 4A utilities (faithful and new versions). */
#ifndef STAGE4A_H
#define STAGE4A_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {                 /* from 21_Types.h.md */
    char DateTime[18 + 1];
    char OpenedValves[220 + 1];
    char Col3[18 + 1],  Col4[18 + 1],  Col5[18 + 1],  Col6[18 + 1],  Col7[18 + 1];
    char Col8[18 + 1],  Col9[18 + 1],  Col10[18 + 1], Col11[18 + 1], Col12[18 + 1];
    char Col13[18 + 1], Col14[18 + 1], Col15[18 + 1], Col16[18 + 1], Col17[18 + 1];
    char Col18[18 + 1], Col19[18 + 1], Col20[18 + 1], Col21[18 + 1], Col22[18 + 1];
} LoggingTableStruct_t;

/* ---- faithful transliterations (4A_faithful.c) ---- */
void Num_to_ASCII(uint32_t Value, uint8_t RL, uint8_t Fmt, uint8_t Pad, uint8_t DP, char ResultString[4 + 1], int16_t *Status);
void Num_to_ASCIIINT(int16_t Value, uint8_t RL, uint8_t Fmt, uint8_t Pad, uint8_t DP, char ResultString[8 + 1], int16_t *Status);
void Num_to_ASCII_INT16(int32_t Value, uint8_t RL, uint8_t Fmt, uint8_t Pad, uint8_t DP, char ResultString[6 + 1], int16_t *Status);
void ConvertIntToString(int16_t It, uint8_t IntLength, char FinalASCIIINT[8 + 1]);
void ASCII_UTF16_8Chars(const char StringInput[8 + 1], uint16_t StringOutput[8 + 1]);
void ConvertIntToStringUTF16(int16_t It, uint8_t IntLength, uint16_t FinalASCIIINT[8 + 1]);
void ConvertRealOrIntToString(float RealValue, uint8_t DecimalNum, uint8_t FractionLength, char StringValue[12 + 1]);
int16_t SearchInArray(const uint8_t InputArray[20], int16_t MaxValue);
void FillNumbers(uint8_t Sequence1_NumberArray[20], uint8_t Sequence2_NumberArray[20]);
void FillNumbersMUX(uint8_t SequenceMUX_NumberArray[20]);
void CheckIfVacant(int16_t InputColNumber, const LoggingTableStruct_t *ReadTableStruct, char TempTableString[18 + 1]);

/* ---- proposed new-firmware versions (4A_new.c) ---- */
void util_two_digit_field(unsigned value, char out[4]);
void util_year_field(unsigned year, char out[5]);
void util_int_to_string(int value, char *out, unsigned cap);
void util_real_to_string(float value, unsigned decimals, char *out, unsigned cap);
int  util_first_index_above(const uint8_t a[20], int max_value);
void util_fill_1_to_20(uint8_t a[20]);
void util_log_column_text(int col, const LoggingTableStruct_t *row, char out[18 + 1]);
#endif
