/* 4F4_faithful.c - rung-by-rung C of Interconnection settings and Function1 (VM top level).
 * Sources: stage0/project_db/ladder/VM_Code__Interconnection_settings.txt (11 rungs, none disabled; 8 and 11 empty),
 *          stage0/project_db/ladder/VM_Code__Function1.txt (39 rungs, none disabled; 39 empty).
 * Function1 is the VM_Code cyclic function, run once per scan. Rules E4/E5/E7 and 11.6.1 as 11_UniLogic_Semantics.
 * Links with 4B_faithful.c (AdjustNumberOfValves, TubeVisibility, ResetDOassigningOver10). */
#include <string.h>
#include "4F4.h"

/* The default written to an empty Password on the first scan (Function1 rung 34). The project's literal is
 * deliberately not copied into the analysis (security rule); the new firmware must not ship a fixed default. */
#define DEFAULT_PASSWORD "<default>"

static bool pcont(bool *m, bool b) { bool e = b && !*m; *m = b; return e; }
static bool ncont(bool *m, bool b) { bool e = !b && *m; *m = b; return e; }
static bool in_range(int v, int lo, int hi) { return v >= lo && v <= hi; }            /* A4.7 */
static void ton(ul_timer_t *t, bool in) {                                             /* TimerOnDelay, A4.10 */
    if (!in) { t->Current = t->Preset; t->Out = false; t->On = false; return; }
    if (!t->On) { t->Current = t->Preset; t->On = true; }
    if (!t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
/* TimerPulse: a rising input starts the pulse, which runs to the end whatever the input does (11 section 4;
 * mid-pulse behaviour is ASSUMPTION 11 6.9). Counts on its start scan, as A4.10. */
static void tp(ul_timer_t *t, bool in, bool *m) {
    bool rise = in && !*m; *m = in;
    if (rise && !t->On) { t->On = true; t->Out = true; t->Current = t->Preset; }
    if (t->On) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->On = false; t->Out = false; } }
    else if (!in) t->Current = t->Preset;
}
static void fill_b(bool *a, int n, bool v) { for (int i = 0; i < n; i++) a[i] = v; }
static int16_t search_in_array(const uint8_t a[20], int16_t maxv) {   /* SearchInArray (41f): first element above maxv, else 25 */
    for (int i = 0; i <= 19; i++) if ((int16_t)a[i] > maxv) return (int16_t)i;
    return 25;
}

/* ================= Interconnection settings (ladder, 11 rungs; Function1 rung 7, every scan) ================= */
void Interconnection_settings(void) {
    static bool m5, m6, m7, m9, m10;
    /* R1: an entry with fewer than 4 valves forces 1 interconnection */
    if ((MaxNumberEntered || NumInterconnectionsEntried) && MaxNumberOfValves < 4) NumInterconnections = 1;
    /* R2: interconnection count or maximum entered: tube pictures, clear selections, enable all valves,
     * erase both sequences, refresh Modbus names. Nothing happens for a count other than 1 or 2 (bit stays set). */
    if (NumInterconnectionsEntried || MaxNumberEntered) {
        bool a = NumInterconnections == 1, b = NumInterconnections == 2;
        if (a) { OneInterconnectingTubeVisible = true; TwoInterconnectingTubeVisible = false; }
        if (b) { TwoInterconnectingTubeVisible = true; OneInterconnectingTubeVisible = false; }
        if (a || b) {
            NumInterconnectionsEntried = false; ResetSequences = true; ChangeModbusNamesVM = true;
            SelectedFirstInlet = 0; SelectedSecondInlet = 0; SelectedFirstOutlet = 0; SelectedSecondOutlet = 0;
            fill_b(EnableInletValve, 19, true); fill_b(EnableOutletValve, 19, true);
        }
    }
    /* R3: a tube count or the maximum entered: note which (ValueChanged; the maximum leaves it as it was), trim
     * the counts (42a), clear selections, enable all, recompute the pictures, erase both sequences, refresh names */
    {   bool w4 = false;
        if (Tube1InletEntered)  { ValueChanged = 1; Tube1InletEntered = false;  w4 = true; }
        if (Tube1OutletEntered) { ValueChanged = 2; Tube1OutletEntered = false; w4 = true; }
        if (Tube2InletEntered)  { ValueChanged = 3; Tube2InletEntered = false;  w4 = true; }
        if (Tube2OutletEntered) { ValueChanged = 4; Tube2OutletEntered = false; w4 = true; }
        if (MaxNumberEntered)   { MaxNumberEntered = false; w4 = true; }
        if (w4) {
            int16_t a, b, c, d;
            AdjustNumberOfValves(FirstInlets, FirstOutlets, SecondInlets, SecondOutlets, ValueChanged, NumInterconnections,
                                 MaxNumberOfValves, &a, &b, &c, &d);
            FirstInlets = a; FirstOutlets = b; SecondInlets = c; SecondOutlets = d;
            SelectedFirstInlet = 0; SelectedSecondInlet = 0; SelectedFirstOutlet = 0; SelectedSecondOutlet = 0;
            fill_b(EnableInletValve, 19, true); fill_b(EnableOutletValve, 19, true);
            ChangeVisibility = true; ResetSequences = true; ChangeModbusNamesVM = true;
        }
    }
    /* R4: Sum4 (float, 11 section 3) of the four counts; a spare output exists -> back-flush option.
     * The 20/10 coils come after, so the 20/10 back-flush flags use last scan's ConfigWith20/10 (one-scan lag). */
    {   ValveSum = (int16_t)((float)FirstInlets + (float)FirstOutlets + (float)SecondInlets + (float)SecondOutlets);
        MaxValveDOnumber = (int16_t)(MaxNumberOfValves - 1);
        bool w5 = ValveSum < MaxNumberOfValves;
        BackFlushOptionVisib = w5;
        BackFlushOptionVisib20 = w5 && ConfigWith20;
        BackFlushOptionVisib10 = w5 && ConfigWith10;
        ConfigWith20 = MaxNumberOfValves > 10;
        ConfigWith10 = MaxNumberOfValves <= 10;
    }
    /* R5-R10: falling edge of ConfigWith20 (20 -> 10 outputs): clear settings above the 10-output range.
     * Each rung has its own edge contact (E7). Index lists checked by script: complete and in order. */
    bool f5 = ncont(&m5, ConfigWith20), f6 = ncont(&m6, ConfigWith20), f7 = ncont(&m7, ConfigWith20);
    bool f9 = ncont(&m9, ConfigWith20), f10 = ncont(&m10, ConfigWith20);
    if (f5) for (int i = 0; i < 22; i++) if (Di_FunctionVM[i] > 12) Di_FunctionVM[i] = 0;       /* VM input codes 13..20 = outputs 10..17 */
    if (f6) { for (int i = 0; i < 19; i++) if (DOInlet[i] > 9) DOInlet[i] = 0;                 /* 0-based: 0 is output 1 (G4) */
              if (BackFlushDONumber > 9) BackFlushDONumber = 0; }
    if (f7) for (int i = 0; i < 19; i++) if (DOOutlet[i] > 9) DOOutlet[i] = 0;
    /* R8 empty */
    if (f9) for (int i = 0; i < 22; i++) if (DI_FunctionMUX[i] > 24) DI_FunctionMUX[i] = 0;     /* V19 */
    if (f10) { for (int i = 0; i < 19; i++) if (DOSingularInlet[i] > 9) DOSingularInlet[i] = 0; /* 1-based: wipes output 10 (V18) */
               if (DOPurgeInlet > 9) DOPurgeInlet = 0; }
    /* R11 empty */
}

/* ================= Function1 (ladder, 39 rungs; the VM cyclic task) ================= */
void Function1(void) {
    static bool mTouchIn[19], mTouchOut[19], m12r, m12f, m14, m31, m32, m33, mTPleave, mTPbright;
    /* R1: first scan with no interconnection count: system configuration screen, counts 0 */
    if (Ladder_Initial_Cycle && NumInterconnections == 0) {
        LoadSystemConfig = true; FirstInlets = 0; FirstOutlets = 0; SecondInlets = 0; SecondOutlets = 0; }
    /* R2: input count is a constant 22, so AdditionalDiscreteModule is always ON */
    NumberOfDInputs = 22;
    AdditionalDiscreteModule = NumberOfDInputs > 14;
    WithoutModules = NumberOfDInputs <= 14;
    Loading();
    /* R3 */
    if (Ladder_Initial_Cycle && (Sequence2v.NumberOfPeriods == 0 || Sequence1v.NumberOfPeriods == 0)) FillNumbers();
    /* R4: leaving a settings screen with an empty configuration: stay, show "you can't leave" */
    {   bool p = CheckEmptySettingsBack || AnalogOrDiscrete || LoadDOMappingValvesCheckEmpty;
        bool empty = NumInterconnections == 0
                  || (NumInterconnections == 1 && (FirstInlets == 0 || FirstOutlets == 0))
                  || (NumInterconnections == 2 && (FirstInlets == 0 || FirstOutlets == 0 || SecondInlets == 0 || SecondOutlets == 0));
        bool w4 = p && empty;
        if (w4) { CheckEmptySettingsBack = false; AnalogOrDiscrete = false; LoadDOMappingValvesCheckEmpty = false; }
        tp(&YouCantLeave, w4, &mTPleave);
    }
    /* R5 */
    if (CheckEmptySettingsBack) { LoadSettings = true; CheckEmptySettingsBack = false; }
    /* R6: open the output-mapping screen for the layout (small 5, medium 9, large 19). Both the 9 and 19 screens
     * can be requested in one scan (V20). Any interconnection count other than 1 or 2 does nothing. */
    if (LoadDOMappingValvesCheckEmpty) {
        int NI = NumInterconnections; bool w7 = false;
        if (NI == 1 && FirstInlets <= 5 && FirstOutlets <= 5) { LoadDOMappingValves1_5 = true; w7 = true; }
        bool w12 = NI == 1 && (in_range(FirstInlets, 6, 9) || in_range(FirstOutlets, 6, 9));
        if (NI == 2) { TempInlets = (int16_t)(FirstInlets + SecondInlets); TempOutlets = (int16_t)(FirstOutlets + SecondOutlets);
                       w12 = w12 || in_range(TempInlets, 1, 9) || in_range(TempOutlets, 1, 9); }
        if (w12) { LoadDOMappingValves1_9 = true; w7 = true; }
        bool w24 = (NI == 1 && (FirstInlets > 9 || FirstOutlets > 9)) || (NI == 2 && (TempInlets > 9 || TempOutlets > 9));
        if (w24) { LoadDOMappingValves1_19 = true; w7 = true; }
        if (w7) LoadDOMappingValvesCheckEmpty = false;
    }
    /* R7 */
    Interconnection_settings();
    /* R8: ChangeVisibility -> TubeVisibility (42c); then (or on the first scan, which joins after the call)
     * copy the pipe pictures into the output-mapping screen arrays for 20 or 10 outputs */
    {   bool w4 = false;
        if (ChangeVisibility) { TubeVisibility(); ChangeVisibility = false; w4 = true; }
        if (Ladder_Initial_Cycle) w4 = true;
        if (w4 && ConfigWith20) {
            memcpy(DOInletVisibility20, TubeVisibility1, 18 * sizeof(bool)); memcpy(DOOutletVisibility20, TubeVisibility2, 18 * sizeof(bool));
            fill_b(DOInletVisibility10, 18, false); fill_b(DOOutletVisibility10, 18, false); }
        if (w4 && ConfigWith10) {
            ResetDOassigningOver10();
            memcpy(DOInletVisibility10, TubeVisibility1, 18 * sizeof(bool)); memcpy(DOOutletVisibility10, TubeVisibility2, 18 * sizeof(bool));
            fill_b(DOInletVisibility20, 18, false); fill_b(DOOutletVisibility20, 18, false); }
    }
    /* R9 */
    OneManualTimer = NumInterconnections == 1;
    TwoManualTimers = NumInterconnections == 2;
    /* R10: manual mode timers. The remaining-time display gets last scan's Current (Store before the timer).
     * With 2 interconnections the timer-2 branch joins wire w4, so both timers run. */
    {   bool a = ManualModeStart && NumInterconnections == 1, b = ManualModeStart && NumInterconnections == 2;
        ton(&ManualTimer2, b);
        if (b) ProcessTimer2.Preset = ManualTimer2.Current;
        if (a || b) ProcessTimer.Preset = ManualTimer.Current;
        ton(&ManualTimer, a || b);
    }
    /* R11: manual time over (both timers with 2 interconnections) */
    if ((NumInterconnections == 1 && ManualTimer.Out) || (NumInterconnections == 2 && ManualTimer.Out && ManualTimer2.Out))
        ManualModeStart = false;
    /* R12: manual mode starts: clear touches, enable all; ends: clear selections and ManualModeEnable, then the same */
    {   bool r = pcont(&m12r, ManualModeStart), f = ncont(&m12f, ManualModeStart);
        if (f) { SelectedFirstInlet = 0; SelectedFirstOutlet = 0; SelectedSecondInlet = 0; SelectedSecondOutlet = 0; ManualModeEnable = false; }
        if (r || f) { fill_b(InletValveTouch, 19, false); fill_b(OutletValveTouch, 19, false);
                      fill_b(EnableInletValve, 19, true); fill_b(EnableOutletValve, 19, true); }
    }
    /* R13: a valve touched (rising edge, one memory per contact) in manual mode or with the start button shown.
     * The 38 contacts (inlets 0..18, outlets 0..18) were checked by script. */
    {   bool any = false;
        for (int i = 0; i < 19; i++) any = pcont(&mTouchIn[i], InletValveTouch[i]) || any;
        for (int i = 0; i < 19; i++) any = pcont(&mTouchOut[i], OutletValveTouch[i]) || any;
        if ((ManualModeStart || StartButtonVisib) && any) FB_ValveSelectionInter();
    }
    /* R14: Temp1 is never written (always OFF), so this only runs on the first scan */
    if (pcont(&m14, Temp1) || Ladder_Initial_Cycle) { fill_b(EnableInletValve, 19, true); fill_b(EnableOutletValve, 19, true); }
    /* R15 */
    SchedulerSequencesVM();
    /* R16: TimeFunction on the tag Schedule, which nothing writes; the coil temp is read nowhere. With an all-zero
     * schedule the window 0..0 is empty (A4.11), so temp is OFF. Dead rung. */
    temp = false;
    /* R17 */
    Route();
    /* R18: open the main screen for the layout (OpenMain button), and on the first scan, with a valid
     * configuration, start the default mode and then open the main screen too (all its paths join wire w2). */
    {   bool w2 = OpenMain;
        int NI = NumInterconnections;
        bool w38 = Ladder_Initial_Cycle && ((NI == 1 && FirstInlets != 0 && FirstOutlets != 0)
                || (NI == 2 && FirstInlets != 0 && FirstOutlets != 0 && SecondInlets != 0 && SecondOutlets != 0));
        if (w38) {
            InletIndex2 = search_in_array(Sequence2v.Inlets, SecondInlets);  OutletIndex2 = search_in_array(Sequence2v.Outlets, SecondOutlets);
            InletIndex1 = search_in_array(Sequence1v.Inlets, FirstInlets);   OutletIndex1 = search_in_array(Sequence1v.Outlets, FirstOutlets);
            if (DefaultModeOnPowerup == 0) { StartModbus = true; ModbusModeState = 1; w2 = true; }
            if (DefaultModeOnPowerup == 1) {
                bool ok1 = InletIndex1 == 25 && OutletIndex1 == 25, ok2 = InletIndex2 == 25 && OutletIndex2 == 25;
                if ((NI == 1 && ok1) || (NI == 2 && ok1 && ok2)) { StartSchedule = true; ScheduleButtonState = 1; w2 = true; }
                if ((NI == 1 && !ok1) || (NI == 2 && !(ok1 && ok2))) { YouHaveIncorrectInletOutletConfig = true; w2 = true; }
            }
            if (DefaultModeOnPowerup == 2) { ResetManualAttempt = true; w2 = true; }
        }
        if (w2) {
            bool w7 = false;
            if (NI == 1 && FirstInlets <= 5 && FirstOutlets <= 5) { Load1_5 = true; w7 = true; }
            bool w12 = NI == 1 && (in_range(FirstInlets, 6, 9) || in_range(FirstOutlets, 6, 9));
            if (NI == 2) { TempInlets = (int16_t)(FirstInlets + SecondInlets); TempOutlets = (int16_t)(FirstOutlets + SecondOutlets);
                           w12 = w12 || in_range(TempInlets, 1, 9) || in_range(TempOutlets, 1, 9); }
            if (w12) { Load1_9_2 = true; w7 = true; }
            bool w24 = (NI == 1 && (FirstInlets > 9 || FirstOutlets > 9)) || (NI == 2 && (TempInlets > 9 || TempOutlets > 9));
            if (w24) { Load1_19_2 = true; w7 = true; }
            if (w7) OpenMain = false;
        }
    }
    /* R19, R20: mode number and which mode buttons are enabled (a button is enabled when its mode runs or none does) */
    {   bool none = !StartSchedule && !StartModbus && !ManualModeStart;
        if (StartModbus) CurrentMode = 1;
        if (none) CurrentMode = 0;
        ModbusModeEnable = StartModbus || none;
        if (StartSchedule) CurrentMode = 2;
        SchedulerModeEnable = StartSchedule || none;     /* none recomputed in the ladder from the same bits */
    }
    /* R21-R23 */
    ModbusMode();
    ScheduleMode();
    ModbusValveNames();
    /* R24 */
    {   bool none = !StartSchedule && !StartModbus && !ManualModeStart;
        if (ManualModeStart) CurrentMode = 3;
        ManualModeEnable = ManualModeStart || none;
    }
    /* R25, R26: remaining-time display visible for 1 or 2 interconnections while a mode runs */
    {   bool anym = StartSchedule || StartModbus || ManualModeStart;
        TimerVisible = anym && NumInterconnections == 1;
        TimerVisible1 = anym && NumInterconnections == 2;
    }
    /* R27, R28 */
    DI_Assigning(); Email();
    DO_Control();
    /* R29 */
    if (LoadVMorMUX) { LoadVMorMUX = false;
        if (VMorMUXState == 0) LoadVMSettings = true;
        if (VMorMUXState == 1) LoadMUXSettings = true; }
    /* R30-R33: screen saver and brightness */
    ton(&ActivateScreenSaver, !Touched_Bit && !VNC_Touched_Bit && ScreenSaverActive);
    if (pcont(&m31, ActivateScreenSaver.Out)) ActivateScreenSaverBit = true;
    {   bool w3 = false;
        if (ncont(&m32, ActivateScreenSaver.Out)) { DeactivateScreenSaverBit = true; w3 = true; }
        if (BrightessEntered) { BrightessEntered = false; w3 = true; }
        tp(&SetsettingsBrightness, w3, &mTPbright);
    }
    if (ncont(&m33, SetsettingsBrightness.Out)) SetBrightness = true;
    /* R34: first scan with an empty password: write the default (literal not copied, see DEFAULT_PASSWORD) */
    if (Ladder_Initial_Cycle && strcmp(Password, "") == 0) { strncpy(Password, DEFAULT_PASSWORD, 16); Password[16] = 0; }
    /* R35, R36 */
    Analog_signals();
    MUX();
    /* R37: all four selections set to 0, unconditionally, every scan (V21) */
    SelectedFirstInlet = 0; SelectedFirstOutlet = 0; SelectedSecondInlet = 0; SelectedSecondOutlet = 0;
    /* R38: MUX pause while a VM route is selected. Runs right after R37, so it is always OFF (V22). */
    PauseMUX = ModeIsActive && ((SelectedFirstInlet != 0 && SelectedFirstOutlet != 0)
                             || (NumInterconnections == 2 && SelectedSecondInlet != 0 && SelectedSecondOutlet != 0));
    /* R39 empty */
}
