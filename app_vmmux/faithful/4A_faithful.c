/* 4A_faithful.c - rung-by-rung C transliteration of the Stage 4A utility functions.
 *
 * Purpose: to pin down exactly what the ORIGINAL UniLogic code does, so it can be tested.
 * This is not the code for the new firmware; see 4A_new.c for that.
 *
 * Conventions (41_Utilities.md section 2):
 *  - ASSUMPTION A4.2: locals start at 0 on every call, except locals that have an initial
 *    value in the project, which start at that value (only `Dec Point Code` = 46 here).
 *  - ASSUMPTION A4.3: OUT parameters start empty/0 on every call and are copied back to the
 *    caller's tag when the function returns.
 *  - Each block runs only when its rung power is ON and passes power on (E4).
 *  - JumpToRegion is a goto (E9).
 *  Comments "R<region>.<rung>" give the original location.
 */
#include "ul_lf.h"
#include "4A.h"
#include "4D.h"   /* Sequence1, Sequence2, SequenceMUX (FillNumbers) */

/* ======================================================================
 * Num to ASCII / Num to ASCIIINT / Num to ASCII_INT16  (VM_Code, ladder)
 * The three functions have identical rungs. They differ only in:
 *              Value    Result String   Result Array / Result Buffer
 *  Num to ASCII       UINT32   STRING[4]       UINT8[4]
 *  Num to ASCIIINT    INT16    STRING[8]       UINT8[8]
 *  Num to ASCII_INT16 INT32    STRING[6]       UINT8[7]
 * ====================================================================== */
static void num_to_ascii_core(int64_t Value, uint8_t ResultLength, uint8_t Format_1,
                              uint8_t Padding, uint8_t DecimalPointLocation,
                              size_t NA, size_t STRCAP,
                              char *ResultString_out, int16_t *Status_out)
{
    /* OUT parameters (A4.3) */
    char    ResultString[16] = {0};
    int16_t Status = 0;
    /* locals (A4.2) */
    uint32_t ArraySize = 0;
    uint8_t  ASCIICode = 0, ConvertByte = 0, ConvertIndex = 0, DecPointCode = 46;
    uint8_t  DecPointLocationPlus1 = 0, I = 0, LengthToCopy = 0, NumericResult = 0;
    uint8_t  OffsetInArray = 0, PaddingLookup = 0, PaddingLookupPlus1 = 0, ReplaceCount = 0;
    uint8_t  ResultLengthMinus1 = 0, SignCode = 0, ValueFromArray = 0, ValueOfNextChar = 0;
    int32_t  FunctionValue = 0;
    bool     NegativeValue = false, ReplacePaddingChar = false;
    uint8_t  ResultArray[8] = {0}, ResultBuffer[8] = {0};

    /* ---- Region1 ---- */
    Status = 0;                                                         /* R1.1 */
    if (ResultLength > 11 || ResultLength < 1) Status = -1;             /* R1.2 */
    if (Format_1 > 2) Status = -2;                                      /* R1.3 */
    if (Padding > 2) Status = -3;                                       /* R1.4 */
    if (DecimalPointLocation >= ResultLength) Status = -4;              /* R1.5 */
    if (Status < 0) goto End_Function;                                  /* R1.6 */
    ConvertIndex = 0;                                                   /* R1.7 */
    lf_fill_u8(ResultBuffer, NA, 0, 0, 16);       /* count 16 > array size: A4.1 clip */
    NegativeValue = (Value < 0);                                        /* R1.8 coil */
    if (Value < 0) FunctionValue = (int32_t)(Value * -1);
    if (Value >= 0) FunctionValue = (int32_t)Value;                     /* R1.9 */
    I = 1;                                                              /* R1.10 */
    lf_fill_u8(ResultArray, NA, 0, 0, 16);        /* count 16 > array size: A4.1 clip */
    goto Region2;

Region2:                       /* "Create an Array from Number" */
    NumericResult = (uint8_t)(FunctionValue % 10);                      /* R2.2 */
    FunctionValue = FunctionValue / 10;
    ASCIICode = (uint8_t)(NumericResult + 48);
    if (DecimalPointLocation != 0) {                                    /* R2.3 */
        DecPointLocationPlus1 = (uint8_t)(DecimalPointLocation + 1);
        if (I == DecPointLocationPlus1) {
            OffsetInArray = (uint8_t)(ResultLength - I);
            lf_fill_u8(ResultArray, NA, OffsetInArray, DecPointCode, 1);
            I++;
        }
    }
    OffsetInArray = (uint8_t)(ResultLength - I);                        /* R2.4 */
    lf_fill_u8(ResultArray, NA, OffsetInArray, ASCIICode, 1);
    if (I < ResultLength) { I++; goto Region2; }                        /* R2.5 */
    PaddingLookup = 0;                                                  /* R2.6 */

Padding:                       /* 0 no padding, 1 leading spaces, 2 leading zeroes */
    PaddingLookupPlus1 = (uint8_t)(PaddingLookup + 1);                  /* R3.2 */
    lf_load_u8(ResultArray, NA, PaddingLookup, &ValueFromArray);        /* R3.3 */
    lf_load_u8(ResultArray, NA, PaddingLookupPlus1, &ValueOfNextChar);
    ReplacePaddingChar = (ValueFromArray == 48 && ValueOfNextChar != 46); /* R3.4 coil */
    if (ReplacePaddingChar) ReplaceCount++;
    if (Padding == 0 && ReplacePaddingChar) lf_store_u8(ResultArray, NA, PaddingLookup, 0);  /* R3.5 */
    if (Padding == 1 && ReplacePaddingChar) lf_store_u8(ResultArray, NA, PaddingLookup, 32); /* R3.6 */
    if (!ReplacePaddingChar) PaddingLookup = ResultLength;              /* R3.7 */
    if (PaddingLookup < ResultLength) { PaddingLookup++; goto Padding; } /* R3.8 */
    if (Padding == 0) {                                                 /* R3.9 */
        ArraySize = (uint32_t)NA;
        LengthToCopy = (uint8_t)(ArraySize - ReplaceCount);
        lf_copy_part_u8(ResultArray, NA, ReplaceCount, LengthToCopy, ResultArray, NA, 0);
    }

    /* ---- Format: 0 no sign, 1 '-' only, 2 '+' or '-' ---- */
    if (Format_1 == 0) SignCode = 32;                                   /* R4.2 */
    if (Format_1 == 1) SignCode = NegativeValue ? 45 : 32;              /* R4.3 */
    if (Format_1 == 2) SignCode = NegativeValue ? 45 : 43;              /* R4.4 */
    if (Padding >= 1 && Padding <= 2)                                   /* R4.5 (A4.7 inclusive) */
        lf_store_u8(ResultArray, NA, 0, SignCode);
    if (Padding == 0 && Format_1 >= 1 && Format_1 <= 2) {               /* R4.6 */
        lf_copy_part_u8(ResultArray, NA, 0, 15, ResultArray, NA, 1);    /* 15 > size: A4.1 */
        lf_store_u8(ResultArray, NA, 0, SignCode);
    }

Region4:                       /* "Convert Result Array into a String" (region 5) */
    lf_load_u8(ResultArray, NA, ConvertIndex, &ConvertByte);            /* R5.2 */
    lf_fill_u8(ResultBuffer, NA, ConvertIndex, ConvertByte, 1);
    ResultLengthMinus1 = (uint8_t)(ResultLength - 1);                   /* R5.3 */
    if (ConvertIndex < ResultLengthMinus1) { ConvertIndex++; goto Region4; }
    lf_buffer_to_string(ResultBuffer, NA, 0, ResultLength, ResultString, STRCAP); /* R5.4 */

End_Function:                  /* region 6, empty */
    (void)ArraySize;
    memcpy(ResultString_out, ResultString, STRCAP + 1);                 /* A4.3 copy-out */
    *Status_out = Status;
}

void Num_to_ASCII(uint32_t Value, uint8_t RL, uint8_t Fmt, uint8_t Pad, uint8_t DP,
                  char ResultString[4 + 1], int16_t *Status)
{ num_to_ascii_core((int64_t)Value, RL, Fmt, Pad, DP, 4, 4, ResultString, Status); }

void Num_to_ASCIIINT(int16_t Value, uint8_t RL, uint8_t Fmt, uint8_t Pad, uint8_t DP,
                     char ResultString[8 + 1], int16_t *Status)
{ num_to_ascii_core((int64_t)Value, RL, Fmt, Pad, DP, 8, 8, ResultString, Status); }

void Num_to_ASCII_INT16(int32_t Value, uint8_t RL, uint8_t Fmt, uint8_t Pad, uint8_t DP,
                        char ResultString[6 + 1], int16_t *Status)
{ num_to_ascii_core((int64_t)Value, RL, Fmt, Pad, DP, 7, 6, ResultString, Status); }

/* ======================================================================
 * ConvertIntToString (VM_Code, ladder, 1 rung)
 * ====================================================================== */
void ConvertIntToString(int16_t It, uint8_t IntLength, char FinalASCIIINT_out[8 + 1])
{
    char FinalASCIIINT[8 + 1] = {0};          /* OUT, A4.3 */
    char ASCIIINT[8 + 1] = {0}; int16_t Status = 0;   /* locals, A4.2 */
    if (It != 0) {                                                      /* R1.1 branch 1 */
        Num_to_ASCIIINT(It, IntLength, 0, 0, 0, ASCIIINT, &Status);
        lf_remove_from_string(ASCIIINT, " ", 0, 8, FinalASCIIINT, 8);
    }
    if (It == 0) lf_store_string("0", FinalASCIIINT, 8);               /* R1.1 branch 2 */
    memcpy(FinalASCIIINT_out, FinalASCIIINT, sizeof FinalASCIIINT);
}

/* ======================================================================
 * ASCII_UTF16_8Chars (VM_Code, ladder) - NEVER CALLED IN ORIGINAL (only from
 * ConvertIntToStringUTF16, which is itself never called).
 * Output: UTF-16 string of up to 8 characters, as uint16_t code units.
 * ====================================================================== */
void ASCII_UTF16_8Chars(const char StringInput[8 + 1], uint16_t StringOutput_out[8 + 1])
{
    uint16_t StringOutput[8 + 1] = {0};       /* OUT, A4.3 */
    uint8_t BufferOf = 0; uint16_t length = 0, StringLength = 0;   /* locals, A4.2 */
    char OneASCIITemp[1 + 1] = {0}, TempString_1[8 + 1] = {0};
    uint8_t TempBuffer_1[18] = {0};

    lf_store_string(StringInput, TempString_1, 8);                      /* R1.1 */
    StringLength = (uint16_t)lf_strlen(TempString_1);
    length = (uint16_t)(StringLength * 2);
    lf_fill_u8(TempBuffer_1, 18, 0, 0, 18);
    BufferOf = 0;
Region2:
    if (StringLength >= 1) {                                            /* R2.1 */
        lf_string_left(TempString_1, 1, OneASCIITemp, 1);
        StringLength--;
        lf_string_right(TempString_1, StringLength, TempString_1, 8);
        /* TagToBuffer copies "the entire tag". Whether that is 1 byte or 2 (with the
         * NUL) does not matter here: the buffer was zero-filled, so the byte after the
         * character is 0 either way. Result: low byte = char, high byte = 0 (UTF-16LE). */
        if (BufferOf < 18) TempBuffer_1[BufferOf] = (uint8_t)OneASCIITemp[0];
        BufferOf = (uint8_t)(BufferOf + 2);
        goto Region2;
    }
    /* R3.1 BufferToTag(TempBuffer_1, 0, length -> StringOutput): UTF-16[8] holds 16 bytes */
    if (length <= 16)
        for (unsigned k = 0; k < length / 2u; ++k)
            StringOutput[k] = (uint16_t)(TempBuffer_1[2 * k] | (TempBuffer_1[2 * k + 1] << 8));
    memcpy(StringOutput_out, StringOutput, sizeof StringOutput);
}

/* ======================================================================
 * ConvertIntToStringUTF16 (VM_Code, ladder) - NEVER CALLED IN ORIGINAL
 * ====================================================================== */
void ConvertIntToStringUTF16(int16_t It, uint8_t IntLength, uint16_t FinalASCIIINT_out[8 + 1])
{
    uint16_t FinalASCIIINT[8 + 1] = {0};      /* OUT, A4.3 */
    char ASCIIINT[8 + 1] = {0}, TempASCII[8 + 1] = {0}; int16_t Status = 0;
    if (It != 0) {                                                      /* R1.1 */
        Num_to_ASCIIINT(It, IntLength, 0, 0, 0, ASCIIINT, &Status);
        lf_remove_from_string(ASCIIINT, " ", 0, 8, TempASCII, 8);
    }
    if (It == 0) lf_store_string("0", TempASCII, 8);
    ASCII_UTF16_8Chars(TempASCII, FinalASCIIINT);                       /* R1.2 */
    memcpy(FinalASCIIINT_out, FinalASCIIINT, sizeof FinalASCIIINT);
}

/* ======================================================================
 * ConvertRealOrIntToString (VM_Code, ladder, 1 rung)
 * ====================================================================== */
void ConvertRealOrIntToString(float RealValue, uint8_t DecimalNum, uint8_t FractionLength,
                              char StringValue_out[12 + 1])
{
    char StringValue[12 + 1] = {0};           /* OUT, A4.3: starts empty (see A4.3 note) */
    /* locals, A4.2 (the ones with initial value 0 in the project are 0 anyway) */
    int16_t FracStatus = 0, SignStatus = 0, SignLength = 0;
    int16_t FractTemp1 = 0, ResultTemp = 0, SignTemp = 0, SignTemp1 = 0;
    int32_t FractionalPart = 0, SignificPart = 0;
    uint8_t Padding = 0; float ReadValueTemp = 0.0f;
    char FracString[6 + 1] = {0}, SignString[6 + 1] = {0};
    int64_t c, d;

    if (RealValue != 0.0f) {                                            /* branch w1 */
        lf_float_to_int(RealValue, 2, &c, &d); SignTemp = (int16_t)c; FractTemp1 = (int16_t)d;
        SignTemp = (int16_t)(SignTemp * 10);
        ReadValueTemp = RealValue * 10.0f;
        lf_float_to_int(ReadValueTemp, 2, &c, &d); SignTemp1 = (int16_t)c; FractTemp1 = (int16_t)d;
        ResultTemp = (int16_t)(SignTemp1 - SignTemp);   /* = first decimal digit, signed */
        if (ResultTemp <= 0) Padding = 2;     /* leading zeroes */          /* w8 */
        if (ResultTemp > 0)  Padding = 0;     /* no padding */              /* w53 */
        lf_float_to_int(RealValue, DecimalNum, &c, &d);                     /* w11 */
        SignificPart = (int32_t)c; FractionalPart = (int32_t)d;

        if (SignificPart != 0 && FractionalPart != 0) {                     /* w13 */
            Num_to_ASCII_INT16(SignificPart, 6, 0, 0, 0, SignString, &SignStatus);
            Num_to_ASCII_INT16(FractionalPart, FractionLength, 0, Padding, 0, FracString, &FracStatus);
        } else if (SignificPart == 0 && FractionalPart == 0) {              /* w41 */
            lf_store_string("0", SignString, 6);
            lf_store_string("0", FracString, 6);
        } else if (SignificPart != 0 && FractionalPart == 0) {              /* w45 */
            Num_to_ASCII_INT16(SignificPart, 6, 0, 0, 0, SignString, &SignStatus);
            lf_store_string("0", FracString, 6);
        } else {                                                            /* w49 */
            lf_store_string("0", SignString, 6);
            Num_to_ASCII_INT16(FractionalPart, FractionLength, 0, Padding, 0, FracString, &FracStatus);
        }
        /* w17 join: the four branches above are mutually exclusive, so this runs once */
        lf_remove_from_string(SignString, " ", 0, 6, SignString, 6);
        lf_remove_from_string(FracString, " ", 0, 6, FracString, 6);
        SignLength = (int16_t)lf_strlen(SignString);
        if (FractionalPart != 0) {                                          /* w22 */
            bool w28 = false;
            if (RealValue < 0.0f) {                                         /* w24 */
                lf_insert_to_string(StringValue, "-", 0, StringValue, 12);
                lf_insert_to_string(StringValue, SignString, 1, StringValue, 12);
                SignLength++;
                w28 = true;
            }
            if (RealValue > 0.0f) {                                         /* w32 */
                lf_insert_to_string(StringValue, SignString, 0, StringValue, 12);
                w28 = true;
            }
            if (w28) {
                lf_insert_to_string(StringValue, ".", SignLength, StringValue, 12);
                SignLength++;
                lf_insert_to_string(StringValue, FracString, SignLength, StringValue, 12);
            }
        }
        if (FractionalPart == 0) {                                          /* w34 */
            if (RealValue < 0.0f) {
                lf_insert_to_string(StringValue, "-", 0, StringValue, 12);
                lf_insert_to_string(StringValue, SignString, 1, StringValue, 12);
            }
            if (RealValue > 0.0f) lf_insert_to_string(StringValue, SignString, 0, StringValue, 12);
        }
    }
    if (RealValue == 0.0f) lf_store_string("0", StringValue, 12);       /* branch w55 */
    (void)FractTemp1; (void)FracStatus; (void)SignStatus;
    memcpy(StringValue_out, StringValue, sizeof StringValue);
}

/* ======================================================================
 * SearchInArray (VM_Code, ST). Returns the index of the first element
 * greater than MaxValue, or 25 if there is none.
 * ====================================================================== */
int16_t SearchInArray(const uint8_t InputArray[20], int16_t MaxValue)
{
    int16_t SearchInArray_1, i;
    SearchInArray_1 = 25;   /* the original comment says "Default to -1", the code says 25 */
    for (i = 0; i <= 19; i++) {
        if ((int16_t)InputArray[i] > MaxValue) {    /* TO_INT_UNI_ of a UINT8: exact */
            SearchInArray_1 = i;
            break;                                  /* EXIT */
        }
    }
    return SearchInArray_1;
}

/* ======================================================================
 * FillNumbers (VM_Code, ST) and FillNumbersMUX (MUX_Code, ST)
 * ====================================================================== */
/* app_vmmux: the original has no parameters and writes the globals (41g); the Stage 4A
 * test passed the arrays in. Integrated form writes the globals directly. */
void FillNumbers(void)
{
    for (uint8_t i = 0; i <= 19; i++) {
        Sequence1.NumberArray[i] = (uint8_t)(i + 1);
        Sequence2.NumberArray[i] = (uint8_t)(i + 1);
    }
}
void FillNumbersMUX(void)
{
    for (uint8_t i = 0; i <= 19; i++) SequenceMUX.NumberArray[i] = (uint8_t)(i + 1);
}

/* ======================================================================
 * CheckIfVacant (MUX_Code, ladder, 1 rung). Returns the text in logging
 * column InputColNumber (0..19 -> Col3..Col22) of ReadTableStruct, or
 * "empty" if that column is blank.
 * ====================================================================== */
void CheckIfVacant(int16_t InputColNumber, const LoggingTableStruct_t *ReadTableStruct,
                   char TempTableString_out[18 + 1])
{
    char TempTableString[18 + 1] = {0};       /* OUT, A4.3 */
    int16_t ColNumber = 0, StringLength = 0;  /* locals, A4.2 */
    const char *col[20] = {
        ReadTableStruct->Col3,  ReadTableStruct->Col4,  ReadTableStruct->Col5,  ReadTableStruct->Col6,
        ReadTableStruct->Col7,  ReadTableStruct->Col8,  ReadTableStruct->Col9,  ReadTableStruct->Col10,
        ReadTableStruct->Col11, ReadTableStruct->Col12, ReadTableStruct->Col13, ReadTableStruct->Col14,
        ReadTableStruct->Col15, ReadTableStruct->Col16, ReadTableStruct->Col17, ReadTableStruct->Col18,
        ReadTableStruct->Col19, ReadTableStruct->Col20, ReadTableStruct->Col21, ReadTableStruct->Col22 };

    ColNumber = (int16_t)(InputColNumber + 2);                          /* R1.1 */
    if (ColNumber >= 2 && ColNumber <= 21) {  /* the 20 parallel Equal(ColNumber, #2..#21) branches */
        lf_store_string(col[ColNumber - 2], TempTableString, 18);
        /* w5 join */
        StringLength = (int16_t)lf_strlen(TempTableString);
        if (StringLength == 0) lf_store_string("empty", TempTableString, 18);
    }
    memcpy(TempTableString_out, TempTableString, sizeof TempTableString);
}
