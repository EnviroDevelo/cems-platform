/* 4C_faithful.c - C copy of the Stage 4C I/O-layer functions, rung by rung / line by line.
 * Not the new-firmware code. Conventions as 4A/4B: E4 power flow, E7 one memory bit per
 * edge contact (starting FALSE, 11 section 6.4), A4.1 out-of-range array writes skipped,
 * A4.2 locals start at 0 on every call, A4.8 an ST read outside an array gives a value
 * that fails the code's own range check (see 43_IO_Layer.md section 2).
 * Comments: "R<n>" = rung n, "Lnn" = ST line.
 */
#include <string.h>
#include "4C.h"

int g_oob, g_alarm_desc_calls[64], g_twoorfb22_calls;

/* ST array access with bounds check (test aid; the original has no check) */
#define WR(arr, n, i, v) do { int _i = (i); if (_i >= 0 && _i < (n)) (arr)[_i] = (v); else g_oob++; } while (0)
static int16_t rd_i16(const int16_t *a, int n, int i) { if (i >= 0 && i < n) return a[i]; g_oob++; return -1; /* A4.8 */ }
static bool    rd_b(const bool *a, int n, int i)       { if (i >= 0 && i < n) return a[i]; g_oob++; return false; /* A4.8 */ }

/* ModifyAlarmProperty(Description, alarm id, text) + TagToBuffer(text -> ...DescriptionBuf):
 * the panel's alarm table and the CSV/e-mail copy of the text. Modelled as a call count;
 * the alarm itself is Stage 7. */
static void alarm_description(int id, int8_t *status) { if (id >= 0 && id < 64) g_alarm_desc_calls[id]++; *status = 0; }

/* =========================== DI_Assigning (VM, ladder) =========================== */
void DI_Assigning(void)
{
    static bool prev_R1 = false;                                   /* E7 edge memory */
    /* R1: falling edge of AdditionalDiscreteModule -> Di_FunctionVM[14..21] = 0 */
    bool fall = !AdditionalDiscreteModule && prev_R1; prev_R1 = AdditionalDiscreteModule;
    if (fall) for (int i = 14; i <= 21; i++) Di_FunctionVM[i] = 0;
    /* R2 */
    DI_Control();
    /* R3, R4: 22 parallel branches, alarm IDs 1..22 */
    for (int i = 0; i < 22; i++)
        if (AlarmDescriptionEntered[i]) { alarm_description(i + 1, &AlarmStatus[i]); AlarmDescriptionEntered[i] = false; }
    /* R7, R8: live in the original; TempBoolsDI/TempBoolsDI1 are never written (item 8) */
    for (int i = 0; i < 14; i++) DiscreteInputsArray[i] = TempBoolsDI[i];
    for (int i = 0; i < 6; i++)  DiscreteInputsArray[14 + i] = TempBoolsDI1[i];
#ifdef ENABLE_DISABLED_RUNGS
    /* R9, R10: DISABLED IN ORIGINAL. The real inputs, as the new firmware will use them. */
    for (int i = 0; i < 14; i++) DiscreteInputsArray[i] = TA30_DI[i];
    for (int i = 0; i < 6; i++)  DiscreteInputsArray[14 + i] = UID_DI[i];
#endif
}

/* =========================== DI_Control (VM, ST) =========================== */
void DI_Control(void)
{
    int16_t i; bool tmpDO[20] = {0}, tmpFlush = false;             /* locals, A4.2 */
    tmpFlush = false;                                              /* L3 */
    for (i = 0; i <= 21; i++) {                                    /* L4 */
        AlarmsVM[i] = false;
        if (Di_FunctionVM[i] == 1) EnableVisibleVM[i] = true;      /* L9 */
        else { EnableVisibleVM[i] = false; AlarmEnableVM[i] = false; EmailEnableVM[i] = false; }
        if (Di_FunctionVM[i] == 1 && AlarmEnableVM[i]) {           /* L19 */
            if (AlarmConditionVM[i] == 0 && !DiscreteInputsArray[i]) AlarmsVM[i] = true;
            else if (AlarmConditionVM[i] == 1 && DiscreteInputsArray[i]) AlarmsVM[i] = true;
        }
        if (Di_FunctionVM[i] == 2 && DiscreteInputsArray[i]) tmpFlush = true;   /* L28 */
        if (Di_FunctionVM[i] >= 3 && Di_FunctionVM[i] <= 20)       /* L35 */
            if (DiscreteInputsArray[i]) WR(tmpDO, 20, Di_FunctionVM[i] - 3, true);
    }
    FlushValveBit = tmpFlush;                                      /* L44 */
    for (i = 0; i <= 20; i++) {                                    /* L45: 0..20, arrays are 0..19 */
        bool v = rd_b(tmpDO, 20, i);
        WR(DOList, 20, i, v);
    }
    if (!AdditionalDiscreteModule)                                 /* L51 */
        for (i = 14; i <= 21; i++) { EnableVisibleVM[i] = false; AlarmEnableVM[i] = false; }
}

/* =========================== DI_AssigningMUX (MUX, ladder) =========================== */
void DI_AssigningMUX(void)
{
    static bool prev_R1 = false;
    bool fall = !AdditionalDiscreteModule && prev_R1; prev_R1 = AdditionalDiscreteModule;   /* R1 */
    if (fall) for (int i = 14; i <= 21; i++) DI_FunctionMUX[i] = 0;
    DI_ControlMUX();                                                                        /* R2 */
    AnalyzerOkBitAlarm = AnalyzerFunctionIsActive && !AnalyzerOkBit;                        /* R3 coil */
    for (int i = 0; i < 22; i++)                                                            /* R4, R5: IDs 33..54 */
        if (AlarmDescriptionEnteredMUX[i]) { alarm_description(33 + i, &AlarmStatusMUX[i]); AlarmDescriptionEnteredMUX[i] = false; }
}

/* =========================== DI_ControlMUX (MUX, ST) =========================== */
void DI_ControlMUX(void)
{
    int16_t i, idx = 0, idxFB = 0; bool DOPurgeInletBitTmp, tmpDO[20];
    DOPurgeInletBitTmp = false; AnalyzerOkBit = false; StopSeqBit = false; AnalyzerFunctionIsActive = false; /* L2-5 */
    for (i = 0; i <= 19; i++) tmpDO[i] = false;                                 /* L8 */
    for (i = 0; i <= 21; i++) AlarmsMUX[i] = false;                             /* L13 */
    for (i = 0; i <= 19; i++) { FBAlarmsMUX[i] = false; FBEmailEnableMUX[i] = false; }  /* L17 */
    for (i = 0; i <= 21; i++) {                                                 /* L23 */
        uint8_t f = DI_FunctionMUX[i];
        bool A = AdditionalDiscreteModule;
        if (f == 1) {                                                           /* L25 alarm channel */
            EnableVisibleMUX[i] = true; EmailVisibMUX[i] = true; FeedbackStateAlarm[i] = false;
        } else if ((A && f >= 3 && f <= 22) || (!A && f >= 3 && f <= 12)) {     /* L31 feedback channel */
            FeedbackStateAlarm[i] = true; EmailVisibMUX[i] = true;
            EnableVisibleMUX[i] = false; AlarmEnableMUX[i] = false;
            idxFB = (int16_t)(f - 3);
            if (idxFB >= 0 && idxFB <= 19 && EmailEnableMUX[i]) FBEmailEnableMUX[idxFB] = true;
        } else {                                                                /* L45 unused */
            EnableVisibleMUX[i] = false; AlarmEnableMUX[i] = false; EmailEnableMUX[i] = false;
            FeedbackStateAlarm[i] = false; EmailVisibMUX[i] = false;
        }
        if (f == 1 && AlarmEnableMUX[i]) {                                      /* L55 */
            if (AlarmConditionMUX[i] == 0 && !DiscreteInputsArray[i]) AlarmsMUX[i] = true;
            else if (AlarmConditionMUX[i] == 1 && DiscreteInputsArray[i]) AlarmsMUX[i] = true;
        }
        if (f == 2 && DiscreteInputsArray[i]) DOPurgeInletBitTmp = true;         /* L64 */
        int lo_do = A ? 24 : 14, hi_do = A ? 43 : 23, hi_fb = A ? 22 : 12, an = A ? 23 : 13, stop = A ? 44 : 24;
        if (f >= lo_do && f <= hi_do) {                                         /* L71 / L104 */
            idx = (int16_t)(f - lo_do);
            if (idx >= 0 && idx <= 19 && DiscreteInputsArray[i]) tmpDO[idx] = true;
        } else if (f >= 3 && f <= hi_fb) {                                      /* L78 / L111 */
            idxFB = (int16_t)(f - 3);
            if (idxFB >= 0 && idxFB <= 19) {
                bool expected = (InitialDOMUX[idxFB] && FeedbackConditionMUX[i] == 0) ||
                                (!InitialDOMUX[idxFB] && FeedbackConditionMUX[i] != 0);
                if (DiscreteInputsArray[i] != expected) FBAlarmsMUX[idxFB] = true;
            }
        } else if (f == an) {                                                   /* L93 / L123 */
            AnalyzerFunctionIsActive = true;
            if (DiscreteInputsArray[i]) AnalyzerOkBit = true;
        } else if (f == stop && DiscreteInputsArray[i]) {                       /* L98 / L128 */
            StopSeqBit = true;
        }
    }
    PurgeInletActivateDI = DOPurgeInletBitTmp;                                  /* L135 */
    for (i = 0; i <= 19; i++) DOListMUX[i] = tmpDO[i];                          /* L138 */
    if (!AdditionalDiscreteModule)                                              /* L143 */
        for (i = 14; i <= 21; i++) { EnableVisibleMUX[i] = false; AlarmEnableMUX[i] = false; }
}

/* =========================== DO_Control (VM, ladder) =========================== */
void TwoORFb_22(void) { g_twoorfb22_calls++; }   /* stub: the real function is specified in 4F */

void DO_Control(void)
{
    DO_Assign();                                                    /* R1 */
    TwoORFb_22();                                                   /* R2: writes DOFinal */
    for (int i = 0; i < 10; i++) TA30_DO[i] = DOFinal[i] || DOFinalMUX[i];          /* R3 */
    for (int i = 0; i < 10; i++) UID_DO[i]  = DOFinal[10 + i] || DOFinalMUX[10 + i]; /* R4 */
    BackFlushValveAvailable = BackFlushExists && BackFlushOptionVisib;  /* R5 branch w1 (coil) */
    FlushValveState = 0;                                                /* R5 branch w4 */
    if (FlushValveBit || FlushValveBitModbus) FlushValveState = 1;      /* R5 branches w5/w8 (after w4, 11.6.1) */
}

/* =========================== DO_Assign (VM, ST) =========================== */
static void set_from_slot(const int16_t *slotmap, const int16_t *domap, int legacyDO)
{
    int16_t slot = rd_i16(slotmap, 19, legacyDO);
    if (slot >= 0 && slot <= 18) {
        int16_t configuredDO = domap[slot];
        if (configuredDO >= 0 && configuredDO <= 19) InitialDO[configuredDO] = true;
    }
}
void DO_Assign(void)
{
    for (int i = 0; i <= 19; i++) InitialDO[i] = false;                                          /* L2 */
    if (SelectedFirstInlet >= 1 && SelectedFirstInlet <= FirstInlets)                            /* L7 */
        set_from_slot(InletSlotByLegacyDO, DOInlet, SelectedFirstInlet - 1);
    if (SelectedFirstOutlet >= 1 && SelectedFirstOutlet <= FirstOutlets)                         /* L22 */
        set_from_slot(OutletSlotByLegacyDO, DOOutlet, FirstInlets + SecondInlets + (SelectedFirstOutlet - 1));
    if (NumInterconnections == 2) {                                                              /* L34 */
        if (SelectedSecondInlet >= 1 && SelectedSecondInlet <= SecondInlets)                     /* L36 */
            set_from_slot(InletSlotByLegacyDO, DOInlet, FirstInlets + SecondInlets - SelectedSecondInlet);
        if (SelectedSecondOutlet >= 1 && SelectedSecondOutlet <= SecondOutlets)                  /* L48 */
            set_from_slot(OutletSlotByLegacyDO, DOOutlet,
                          FirstInlets + SecondInlets + FirstOutlets + SecondOutlets - SelectedSecondOutlet);
    }
}

/* =========================== DO_AssignMUX (MUX, ST) =========================== */
void DO_AssignMUX(void)
{
    int16_t i, idx;                     /* locals hasRising, newIdx are declared but never used */
    ActiveInletIdx = -1;                                                         /* L2 */
    for (i = 0; i <= 19; i++) InitialDOMUX[i] = false;                           /* L5 */
    for (i = 0; i <= 18; i++) {                                                  /* L10 */
        if (SingularInletActivate[i]) {
            SingularInletState[i] = 1;
            idx = (int16_t)(DOSingularInlet[i] - 1);                             /* 1..20 -> 0..19 */
            if (idx >= 0 && idx <= 19) InitialDOMUX[idx] = true;
        } else SingularInletState[i] = 0;
    }
    if (PurgeInletActivate || PurgeInletActivateDI || PurgeInletActivateModbus) { /* L26 */
        PurgeInletState = 1;
        idx = (int16_t)(DOPurgeInlet - 1);
        if (idx >= 0 && idx <= 19) InitialDOMUX[idx] = true;
    } else PurgeInletState = 0;
    for (i = 0; i <= 18; i++) PrevSingularInletActivate[i] = SingularInletActivate[i];   /* L37 */
}

/* =========================== DO_ControlMUX (MUX, ST) =========================== */
void DO_ControlMUX(void)
{
    for (int16_t i = 0; i <= 19; i++) DOFinalMUX[i] = DOListMUX[i] || InitialDOMUX[i];
}

/* =========================== Analog_signals (VM, ladder) =========================== */
void Analog_signals(void)
{
    static bool pAE[2], pAAE[2], pTE[2], pTAE[2];                  /* E7 edge memories */
    for (int c = 0; c < 2; c++) {
        /* R1 / R3: RealLinear = (X*((S-F)/16383))+F with A=X raw, B=S=FirstCalibPoint,
           C=F=SecondCalibPoint (item 9, accepted). Status 2 -> SecondCalibPoint. */
        float X = (float)TA30_AI[c], S = FirstCalibPoint[c], F = SecondCalibPoint[c];
        AnalogSensor[c] = (X * ((S - F) / 16383.0f)) + F;
        if (TA30_AI_Status[c] == 2) AnalogSensor[c] = SecondCalibPoint[c];
        /* R2 / R4 */
        bool fAE = !AnalogEnable[c] && pAE[c]; pAE[c] = AnalogEnable[c];
        if (fAE) { AnalogAlarmEnabled[c] = false; AnalogAlarmEmailEnabled[c] = false; ParameterColNumberLoggingArray[c] = false; }
        bool fAAE = !AnalogAlarmEnabled[c] && pAAE[c]; pAAE[c] = AnalogAlarmEnabled[c];   /* sees the reset above (11.6.1) */
        if (fAAE) AnalogAlarmEmailEnabled[c] = false;
        if (AnalogAlarmDescriptionEntered[c]) { alarm_description(24 + c, &AnalogAlarmStatus[c]); AnalogAlarmDescriptionEntered[c] = false; }
    }
    for (int c = 0; c < 2; c++) {
        /* R5 / R7 */
        TemperatureSensor[c] = (int16_t)(TA30_TEMP[c] + TemperatureShift[c]);
        /* R6 / R8 */
        bool fTE = !TemperatureEnable[c] && pTE[c]; pTE[c] = TemperatureEnable[c];
        bool w4 = false;
        if (fTE) { TemperatureAlarmEnabled[c] = false; ParameterColNumberLoggingArray[2 + c] = false; w4 = true; }
        bool fTAE = !TemperatureAlarmEnabled[c] && pTAE[c]; pTAE[c] = TemperatureAlarmEnabled[c];
        if (fTAE) w4 = true;
        if (w4) TemperatureAlarmEmailEnabled[c] = false;
        if (TemperatureAlarmDescriptionEntered[c]) { alarm_description(28 + c, &TemperatureAlarmStatus[c]); TemperatureAlarmDescriptionEntered[c] = false; }
    }
    /* R9, R10: negated coils, all 4 elements (only 0..1 have hardware) */
    for (int c = 0; c < 4; c++) { AnalogSuppression[c] = !AnalogAlarmEnabled[c]; TemperatureAlarmSuppression[c] = !TemperatureAlarmEnabled[c]; }
}
