/* 4B_faithful.c - line-by-line C of the Stage 4B structured-text functions.
 * ST maps almost directly to C. Points that differ:
 *  - FOR i := a TO b runs while i <= b (inclusive); ST INT is int16_t.
 *  - A write outside an array's bounds is skipped and counted (ASSUMPTION A4.1; the
 *    UniLogic behaviour is not stated). It cannot happen with valid settings.
 *  - Locals start at 0 (ASSUMPTION A4.2). OUT parameters are copied back on return (A4.3).
 * Comments "Lnn" give the line in stage0/project_db/st/<module>__<function>.st.
 */
#include "4B.h"

int g_out_of_range_writes;
#define SETB(arr, n, idx, v) do { int _i = (idx); if (_i >= 0 && _i < (n)) (arr)[_i] = (v); else g_out_of_range_writes++; } while (0)

/* ===================== AdjustNumberOfValves (VM_Code) ===================== */
#define SUM1 (*AFI + *AFO)
#define SUM2 (*AFI + *AFO + *ASI + *ASO)
#define DEC_WHILE(sum, var) while (((sum) > NumberOfValve) && (*(var) > 1)) *(var) = (int16_t)(*(var) - 1)

void AdjustNumberOfValves(int16_t FirstInlets1, int16_t FirstOutlets1, int16_t SecondInlets1,
                          int16_t SecondOutlets1, int16_t ChangedParameter,
                          int16_t NumInterconnections1, int16_t NumberOfValve,
                          int16_t *AdjFirstInlets_out, int16_t *AdjFirstOutlets_out,
                          int16_t *AdjSecondInlets_out, int16_t *AdjSecondOutlets_out)
{
    int16_t a = 0, b = 0, c = 0, d = 0;         /* OUT params (A4.3) */
    int16_t *AFI = &a, *AFO = &b, *ASI = &c, *ASO = &d;
    int16_t SumValves = 0;                       /* local (A4.2) */

    *AFI = FirstInlets1; *AFO = FirstOutlets1;                       /* L3-6 */
    *ASI = SecondInlets1; *ASO = SecondOutlets1;
    if (NumInterconnections1 == 1) {                                 /* L10 */
        *ASI = 0; *ASO = 0;                                          /* L13-14 */
        SumValves = (int16_t)SUM1;                                   /* L17 */
        if (SumValves > NumberOfValve) {                             /* L19 */
            switch (ChangedParameter) {
            case 1: DEC_WHILE(SUM1, AFO); DEC_WHILE(SUM1, AFI); break;   /* L21-30 */
            case 2: DEC_WHILE(SUM1, AFI); DEC_WHILE(SUM1, AFO); break;   /* L31-39 */
            default: break;                                              /* no ELSE */
            }
        }
    } else if (NumInterconnections1 == 2) {                          /* L45 */
        SumValves = (int16_t)SUM2;                                   /* L47 */
        if (SumValves > NumberOfValve) {                             /* L49 */
            switch (ChangedParameter) {
            case 1: DEC_WHILE(SUM2, ASI); DEC_WHILE(SUM2, ASO);          /* L52-71 */
                    DEC_WHILE(SUM2, AFO); DEC_WHILE(SUM2, AFI); break;
            case 2: DEC_WHILE(SUM2, ASI); DEC_WHILE(SUM2, ASO);          /* L72-91 */
                    DEC_WHILE(SUM2, AFI); DEC_WHILE(SUM2, AFO); break;
            case 3: DEC_WHILE(SUM2, AFI); DEC_WHILE(SUM2, AFO);          /* L92-111 */
                    DEC_WHILE(SUM2, ASO); DEC_WHILE(SUM2, ASI); break;
            case 4: DEC_WHILE(SUM2, AFI); DEC_WHILE(SUM2, AFO);          /* L112-131 */
                    DEC_WHILE(SUM2, ASI); DEC_WHILE(SUM2, ASO); break;
            default: break;
            }
        }
    }
    (void)SumValves;
    *AdjFirstInlets_out = a; *AdjFirstOutlets_out = b;               /* copy-out (A4.3) */
    *AdjSecondInlets_out = c; *AdjSecondOutlets_out = d;
}

/* ===================== AdjustNumberOfValvesMUX (MUX_Code) ===================== */
void AdjustNumberOfValvesMUX(void)
{
    int16_t i = 0, n = 0;
    n = (int16_t)(NumberOfMUXValves - 1);        /* L1: exclude the purge valve */
    if (n < 1) n = 1;                            /* L4-8: clamp 1..19 */
    else if (n > 19) n = 19;
    for (i = 0; i <= 18; i++) {                  /* L11 */
        SingularInletVisib[i] = (i < n);
        if (ConfigWith20)      SingularInletVisib2[i] = (i < n);
        else if (ConfigWith10) SingularInletVisib1[i] = (i < n);
    }
}

/* ===================== TubeVisibility (VM_Code) ===================== */
void TubeVisibility(void)
{
    int16_t count = 0, FirstVisibNum = 0, i = 0, SecondVisibNum = 0, startIndex = 0;

    if (NumInterconnections == 1) {                                          /* L3 */
        if (FirstInlets <= 9 && FirstOutlets <= 9) { FirstVisibNum = 18; SecondVisibNum = 16; }
        else                                       { FirstVisibNum = 38; SecondVisibNum = 36; }
    } else if (NumInterconnections == 2) {                                   /* L11 */
        if ((FirstInlets + SecondInlets) <= 9 && (FirstOutlets + SecondOutlets) <= 9)
             { FirstVisibNum = 18; SecondVisibNum = 16; }
        else { FirstVisibNum = 38; SecondVisibNum = 36; }
    }
    for (i = 0; i <= FirstVisibNum; i++)  SETB(TubeVisibility1, 39, i, false);   /* L21 */
    for (i = 0; i <= SecondVisibNum; i++) SETB(TubeVisibility2, 37, i, false);   /* L25 */

    if (NumInterconnections == 1) {                                          /* L30 */
        count = (int16_t)(2 * FirstInlets);
        for (i = 0; i <= count - 1; i++) SETB(TubeVisibility1, 39, i, true);
        count = (int16_t)(2 * FirstOutlets - 1);
        for (i = 0; i <= count - 1; i++) SETB(TubeVisibility2, 37, i, true);
    } else if (NumInterconnections == 2) {                                   /* L43 */
        count = (int16_t)(2 * FirstInlets);
        for (i = 0; i <= count - 1; i++) SETB(TubeVisibility1, 39, i, true);
        count = (int16_t)(2 * SecondInlets);
        startIndex = (int16_t)((FirstVisibNum + 1) - count);
        for (i = startIndex; i <= FirstVisibNum; i++) SETB(TubeVisibility1, 39, i, true);
        count = (int16_t)(2 * FirstOutlets - 1);
        for (i = 0; i <= count - 1; i++) SETB(TubeVisibility2, 37, i, true);
        count = (int16_t)(2 * SecondOutlets - 1);
        startIndex = (int16_t)((SecondVisibNum + 1) - count);
        for (i = startIndex; i <= SecondVisibNum; i++) SETB(TubeVisibility2, 37, i, true);
    }
}

/* ===================== ResetDOassigningOver10 (VM_Code) ===================== */
void ResetDOassigningOver10(void)
{
    for (int16_t i = 0; i <= 18; i++) {
        if (DOInlet[i] >= 10)  DOInlet[i] = 0;
        if (DOOutlet[i] >= 10) DOOutlet[i] = 0;
    }
    if (BackFlushDONumber >= 10) BackFlushDONumber = 0;
}

/* ===================== ResetCertainCheckbox (MUX_Code) ===================== */
void ResetCertainCheckbox(void)
{
    int16_t i, j, mainCol = 0;
    for (i = 0; i <= 19; i++) {
        if (ColumnIsntVacantArray[i]) {
            mainCol = ParameterColNumberArray[i];
            for (j = 0; j <= 19; j++)
                if (j != i && ParameterColNumberArray[j] == mainCol)
                    ParameterColNumberLoggingArray[j] = false;
            ColumnIsntVacantArray[i] = false;
            ParameterColNumberLoggingArray[i] = true;
        }
    }
}
