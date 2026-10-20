/* 4G_faithful.c - Stage 4G Modbus functions, transliterated rung by rung (specs 47a-47d).
 * Semantics: E4 blocks pass power on, E5 compares pass power only when true, E7 one edge memory per element
 * (updated every scan), 11 6.1.1 a "joins wire" path runs the shared tail once, after all its contributors,
 * with OR power. Links with ../4A/4A_faithful.c (ConvertIntToString). */
#include <string.h>
#include "4G.h"

void ConvertIntToString(int16_t It, uint8_t IntLength, char FinalASCIIINT_out[8 + 1]);   /* 4A, 41b */

/* app_vmmux: the tags are defined once in src/vm_tags.c (generated, power-up values from the
 * data dictionary); g_dt_ms is the platform's (ul_system.h). */

/* ---- element models ---- */
static void ton(ul_timer_t *t, bool in) {                                             /* TimerOnDelay, A4.10 */
    if (!in) { t->Current = t->Preset; t->Out = false; t->On = false; return; }
    if (!t->On) { t->Current = t->Preset; t->On = true; }
    if (!t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
static void tacc(ul_timer_t *t, bool in) {        /* TimerAccumulated: counts while powered, holds while not */
    if (in && !t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
/* treset (TimerReset): one definition in src/ul_blocks.c (app_vmmux) */
static bool pcont(bool *m, bool b) { bool e = b && !*m; *m = b; return e; }
static bool ncont(bool *m, bool b) { bool e = !b && *m; *m = b; return e; }
static bool in_range(int16_t v, int16_t lo, int16_t hi) { return v >= lo && v <= hi; }   /* 11 6.7: inclusive */
/* Tag To Buffer, Overwrite Data, offset 0, from a string tag of declared size n (A4.17) */
static void tag_to_buffer(uint8_t dst[20], const char *s, size_t n) {
    size_t len = strlen(s);
    if (g_tag2buf_whole) { memset(dst, 0, n); memcpy(dst, s, len); }
    else memcpy(dst, s, len);
}

/* ===================== VM ModbusMode (47a) ===================== */
static bool m4_coil, m6_fall;
void ModbusMode(void) {
    /* R1: coil set by the master while the mode is not running */
    if (StartModbusCoil && !StartModbus) { ModbusModeNotActive = true; StartModbusCoil = false; }
    /* R2: copy the master's route into the selections and range-check it, every scan */
    if (StartModbus && StartModbusCoil) {
        bool err = false;
        SelectedFirstInlet = SelectedFirstInletMb;
        if (SelectedFirstInlet > FirstInlets || SelectedFirstInlet < 1) { SelectedFirstInlet = 0; SelectedFirstInletMb = 0; err = true; }
        SelectedFirstOutlet = SelectedFirstOutletMb;
        if (SelectedFirstOutlet > FirstOutlets || SelectedFirstOutlet < 1) { SelectedFirstOutlet = 0; SelectedFirstOutletMb = 0; err = true; }
        if (NumInterconnections == 2) {
            SelectedSecondInlet = SelectedSecondInletMb;
            if (SelectedSecondInlet > SecondInlets || SelectedSecondInlet < 1) { SelectedSecondInlet = 0; SelectedSecondInletMb = 0; err = true; }
            SelectedSecondOutlet = SelectedSecondOutletMb;
            if (SelectedSecondOutlet > SecondOutlets || SelectedSecondOutlet < 1) { SelectedSecondOutlet = 0; SelectedSecondOutletMb = 0; err = true; }
        }
        if (err) { ValveSelectionErrorModbus = true; StartModbusCoil = false; }   /* w10 tail, once */
    }
    /* R3 */
    if (ResetMbError) { ModbusModeNotActive = false; ValveSelectionErrorModbus = false; ResetMbError = false; }
    /* R4 (the duplicated p110 parameter is one edge element) */
    {   bool rise = pcont(&m4_coil, StartModbusCoil);
        if (StartModbus && rise) { ModbusModeNotActive = false; ValveSelectionErrorModbus = false; } }
    /* R5: run timers; the w8 path joins w5, so with 2 interconnections both timers run */
    {   bool p = StartModbus && StartModbusCoil;
        bool b4 = p && NumInterconnections == 1, b8 = p && NumInterconnections == 2;
        ton(&ModbusTimer2, b8);
        if (b8) ProcessTimer2.Preset = ModbusTimer2.Current;
        bool w5 = b4 || b8;
        if (w5) ProcessTimer.Preset = ModbusTimer.Current;     /* Store before the timer: last scan's value */
        ton(&ModbusTimer, w5);
        if (!StartModbus && FlushValveBitModbus) FlushValveBitModbus = false; }
    /* R6: end of the run, or the mode switched off */
    {   bool w5 = StartModbus && ((NumInterconnections == 1 && ModbusTimer.Out) ||
                                  (NumInterconnections == 2 && ModbusTimer.Out && ModbusTimer2.Out));
        if (w5) { StartModbus = false; ModbusModeState = 0; }
        bool fall = ncont(&m6_fall, StartModbus);               /* branch w17, after w1: sees this scan's reset */
        if (w5 || fall) {
            SelectedFirstInlet = 0; SelectedFirstOutlet = 0; SelectedSecondInlet = 0; SelectedSecondOutlet = 0;
            StartModbusCoil = false;
        } }
}

/* ===================== VM ModbusValveNames (47b) ===================== */
/* Rungs 4-7: one list position per scan for interconnection 1. lo..hi is the rung's ID range (0..10 or
 * 11..18). The blank-name path always writes the number into FirstInterconInletName1 (M10). */
static void names_first(bool *bit, int16_t *cur, const int16_t *target, char names[19][11], uint8_t dest[19][20],
                        int lo, int hi, bool reset_loading) {
    if (!*bit) return;
    if (*cur < *target && *cur >= lo && *cur <= hi) {
        strcpy(TempString, names[*cur]);                         /* the matching StoreString head */
        TempStringLength = (int16_t)strlen(TempString);
        bool a = TempStringLength > 0, b;
        if (a) tag_to_buffer(TempBuffer1, TempString, sizeof TempString);
        b = TempStringLength <= 0;
        if (b) {
            TempValveNumber = (int16_t)(*cur + 1);
            ConvertIntToString(TempValveNumber, TempLength, TempStringASCII);
            TempStringLength = (int16_t)strlen(TempStringASCII);
            tag_to_buffer(FirstInterconInletName[0], TempStringASCII, sizeof TempStringASCII);
        }
        if (a || b) {                                            /* w12 tail: copy by the (old) ID, then Inc */
            if (*cur >= lo && *cur <= hi) { memcpy(dest[*cur], TempBuffer1, 20); (*cur)++; }
        }
    }
    if (*cur >= *target) { *bit = false; if (reset_loading) LoadingWindow = false; }
}
/* Rungs 8-11: interconnection 2. Rung 8 reads the name and works out TempValveNumber2Interc (9 - ID or
 * 19 - ID); rung 9 copies TempBuffer1 into slot TempValveNumber2Interc (0-based) and moves on. */
static void names_second_read(bool *bit, const int16_t *cur, const int16_t *target, char names[19][11]) {
    if (!*bit) return;
    if (*cur < *target && *cur >= 0 && *cur <= 18) {
        strcpy(TempString, names[*cur]);
        TempStringLength = (int16_t)strlen(TempString);
        bool r9 = in_range(TempInletsModbus, 1, 9) || in_range(TempOutletsModbus, 1, 9);
        if (r9) TempValveNumber2Interc = (int16_t)(9 - *cur);
        bool g9 = TempInletsModbus > 9 || TempOutletsModbus > 9;
        if (g9) TempValveNumber2Interc = (int16_t)(19 - *cur);
        if (r9 || g9) {                                          /* w13 tail, once */
            if (TempStringLength > 0) tag_to_buffer(TempBuffer1, TempString, sizeof TempString);
            if (TempStringLength <= 0) {
                TempValveNumber2IntercTemp = (int16_t)(TempValveNumber2Interc + 1);
                ConvertIntToString(TempValveNumber2IntercTemp, TempLength, TempStringASCII);
                TempStringLength = (int16_t)strlen(TempStringASCII);
                tag_to_buffer(FirstInterconInletName[0], TempStringASCII, sizeof TempStringASCII);
            }
        }
    }
    if (*cur >= *target) *bit = false;
}
static void names_second_copy(bool bit, int16_t *cur, const int16_t *target, uint8_t dest[18][20]) {
    if (bit && *cur < *target && TempValveNumber2Interc >= 0 && TempValveNumber2Interc <= 17) {
        memcpy(dest[TempValveNumber2Interc], TempBuffer1, 20);
        (*cur)++;
    }
}
void ModbusValveNames(void) {
    /* R1, R2: clear all 74 name buffers (checked by script: 19 + 19 + 18 + 18) */
    if (ChangeModbusNamesVM) {
        memset(FirstInterconInletName, 0, sizeof FirstInterconInletName);
        memset(FirstInterconOutletName, 0, sizeof FirstInterconOutletName);
        memset(SecondInterconInletName, 0, sizeof SecondInterconInletName);
        memset(SecondInterconOutletName, 0, sizeof SecondInterconOutletName);
    }
    /* R3: start the lists */
    if (ChangeModbusNamesVM) {
        ChangeModbusNamesVM = false; LoadingWindow = true;
        CurrentInletlistID = 0; CurrentOutletlistID = 0;
        TargetInletListID = FirstInlets; TargetOutletListID = FirstOutlets;
        ChangeModbusNamesVMFirstInlets = true; ChangeModbusNamesVMFirstOutlets = true;
        if (NumInterconnections == 2) {
            ChangeModbusNamesVMSecondInlets = true; ChangeModbusNamesVMSecondOutlets = true;
            TempInletsModbus = (int16_t)(FirstInlets + SecondInlets);
            TempOutletsModbus = (int16_t)(FirstOutlets + SecondOutlets);
            if (in_range(TempInletsModbus, 1, 9) || in_range(TempOutletsModbus, 1, 9)) {
                CurrentInletlistID2Inter = (int16_t)(9 - SecondInlets); CurrentOutletlistID2Interc = (int16_t)(9 - SecondOutlets);
                TargetInletListID2Interc = 9; TargetOutletListID2Interc = 9;
            }
            if (TempInletsModbus > 9 || TempOutletsModbus > 9) {   /* later branch: wins when both pass */
                CurrentInletlistID2Inter = (int16_t)(19 - SecondInlets); CurrentOutletlistID2Interc = (int16_t)(19 - SecondOutlets);
                TargetInletListID2Interc = 19; TargetOutletListID2Interc = 19;
            }
        }
    }
    names_first(&ChangeModbusNamesVMFirstInlets, &CurrentInletlistID, &TargetInletListID, InletName, FirstInterconInletName, 0, 10, true);    /* R4 */
    names_first(&ChangeModbusNamesVMFirstInlets, &CurrentInletlistID, &TargetInletListID, InletName, FirstInterconInletName, 11, 18, true);   /* R5 */
    names_first(&ChangeModbusNamesVMFirstOutlets, &CurrentOutletlistID, &TargetOutletListID, OutletName, FirstInterconOutletName, 0, 10, false);  /* R6 */
    names_first(&ChangeModbusNamesVMFirstOutlets, &CurrentOutletlistID, &TargetOutletListID, OutletName, FirstInterconOutletName, 11, 18, false); /* R7 */
    names_second_read(&ChangeModbusNamesVMSecondInlets, &CurrentInletlistID2Inter, &TargetInletListID2Interc, InletName);                        /* R8 */
    names_second_copy(ChangeModbusNamesVMSecondInlets, &CurrentInletlistID2Inter, &TargetInletListID2Interc, SecondInterconInletName);           /* R9 */
    names_second_read(&ChangeModbusNamesVMSecondOutlets, &CurrentOutletlistID2Interc, &TargetOutletListID2Interc, OutletName);                  /* R10 */
    names_second_copy(ChangeModbusNamesVMSecondOutlets, &CurrentOutletlistID2Interc, &TargetOutletListID2Interc, SecondInterconOutletName);     /* R11 */
    /* R12-R15 empty */
}

/* ===================== MUX ModbusModeMUX (47c) ===================== */
void ModbusValveSelection(void) {                                /* ST, 8 lines */
    if (TempSelectedModbusValveMUX >= 0 && TempSelectedModbusValveMUX <= 18) {
        for (int i = 0; i <= 18; i++) SingularInletActivate[i] = (i == TempSelectedModbusValveMUX);
    } else {
        for (int i = 0; i <= 18; i++) SingularInletActivate[i] = false;
    }
}
static bool m3_coil, m5_stop, m5_pause, m5_alarm, m6_fallmux;
void ModbusModeMUX(void) {
    /* R1 */
    if (StartModbusCoilMUX && !StartModbusMUX) { ModbusNotActiveMUX = true; StartModbusCoilMUX = false; }
    /* R2: 1-based valve from the master, made 0-based; the upper check is "> N" (M1) */
    if (StartModbusMUX && StartModbusCoilMUX) {
        TempSelectedModbusValveMUX = SelectedValveMbMUX;
        TempSelectedModbusValveMUX--;
        if (TempSelectedModbusValveMUX > NumberOfMUXValves || TempSelectedModbusValveMUX < 0) {
            SelectedValveMbMUX = 0; TempSelectedModbusValveMUX = 0; ValveSelectionErrorModbusMUX = true; StartModbusCoilMUX = false;
        }
    }
    /* R3 */
    {   bool w3 = false;
        if (ResetMbErrorMUX) { ResetMbErrorMUX = false; w3 = true; }
        bool rise = pcont(&m3_coil, StartModbusCoilMUX);
        if (StartModbusMUX && rise) w3 = true;
        if (w3) { ModbusNotActiveMUX = false; ValveSelectionErrorModbusMUX = false; } }
    /* R4 empty. R5: run */
    {   bool p = StartModbusMUX && StartModbusCoilMUX;
        bool w6 = p && ((EnableLockValvesIfAnalyzerNotOk && !AnalyzerOkBitAlarm) || !EnableLockValvesIfAnalyzerNotOk);
        bool w9 = w6 && !StopSeqBit && !PauseMUX;
        tacc(&ModbusTimerMUX, w9);
        if (w9) ProcessTimerMUX.Preset = ModbusTimerMUX.Current;
        if (w9) ModbusValveSelection();
        bool ps = pcont(&m5_stop, StopSeqBit), pp = pcont(&m5_pause, PauseMUX), pa = pcont(&m5_alarm, AnalyzerOkBitAlarm);
        if (p && (ps || pp || (EnableLockValvesIfAnalyzerNotOk && pa)))
            memset(SingularInletActivate, 0, sizeof SingularInletActivate);   /* RemoveFromArr whole array, 11 6.2 */
        if (!StartModbusMUX && PurgeInletActivateModbus) PurgeInletActivateModbus = false; }
    /* R6: end of the run, or the mode switched off. SingularInletActivate is not cleared (M2). */
    {   bool w4 = StartModbusMUX && ModbusTimerMUX.Out;
        if (w4) StartModbusMUX = false;
        bool fall = ncont(&m6_fallmux, StartModbusMUX);
        if (w4 || fall) {
            ModbusModeStateMUX = 0; SelectedValveMbMUX = 0; TempSelectedModbusValveMUX = 0; StartModbusCoilMUX = false;
            treset(&ModbusTimerMUX);
        } }
    /* R7 empty */
}
void Modbus(void) {}               /* MUX: 5 empty rungs */
void ModbusValveNamesMUX(void) {}  /* MUX: 5 empty rungs */
