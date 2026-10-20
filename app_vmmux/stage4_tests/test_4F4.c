/* test_4F4.c - tests for 4F4_faithful.c (Interconnection settings, Function1).
 * Build: gcc -std=c11 -Wall -Wextra test_4F4.c 4F4_faithful.c ../4B/4B_faithful.c
 * Functions from other chunks are recording stubs here; the checks are on what Function1 itself does and on
 * the order of its calls. No rungs are disabled in these two functions. */
#include <stdio.h>
#include <string.h>
#include "4F4.h"

/* 4B globals */
int16_t NumberOfMUXValves; bool ConfigWith10, ConfigWith20;
bool SingularInletVisib[19], SingularInletVisib1[19], SingularInletVisib2[19];
int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;
bool TubeVisibility1[39], TubeVisibility2[37];
int16_t DOInlet[19], DOOutlet[19], BackFlushDONumber;
bool ColumnIsntVacantArray[20], ParameterColNumberLoggingArray[20]; int16_t ParameterColNumberArray[20];
/* 4F4 globals */
uint32_t g_dt_ms = 100; bool Ladder_Initial_Cycle, Touched_Bit, VNC_Touched_Bit;
bool MaxNumberEntered, NumInterconnectionsEntried, Tube1InletEntered, Tube1OutletEntered, Tube2InletEntered, Tube2OutletEntered;
bool OneInterconnectingTubeVisible, TwoInterconnectingTubeVisible, ResetSequences, ChangeModbusNamesVM, ChangeVisibility;
bool BackFlushOptionVisib, BackFlushOptionVisib20, BackFlushOptionVisib10;
int16_t MaxNumberOfValves, MaxValveDOnumber, ValveSum, ValueChanged, DOSingularInlet[19], DOPurgeInlet;
int16_t SelectedFirstInlet, SelectedFirstOutlet, SelectedSecondInlet, SelectedSecondOutlet;
bool EnableInletValve[19], EnableOutletValve[19]; uint8_t Di_FunctionVM[22], DI_FunctionMUX[22];
bool LoadSystemConfig, LoadSettings, Load1_5, Load1_9_2, Load1_19_2, LoadVMSettings, LoadMUXSettings;
bool LoadDOMappingValves1_5, LoadDOMappingValves1_9, LoadDOMappingValves1_19, ActivateScreenSaverBit, DeactivateScreenSaverBit, SetBrightness;
uint8_t NumberOfDInputs; bool AdditionalDiscreteModule, WithoutModules, CheckEmptySettingsBack, AnalogOrDiscrete, LoadDOMappingValvesCheckEmpty;
int16_t TempInlets, TempOutlets;
bool DOInletVisibility20[18], DOOutletVisibility20[18], DOInletVisibility10[18], DOOutletVisibility10[18];
bool OneManualTimer, TwoManualTimers, ManualModeStart, ManualModeEnable, StartButtonVisib;
ul_timer_t ManualTimer, ManualTimer2, ProcessTimer, ProcessTimer2;
bool InletValveTouch[19], OutletValveTouch[19], Temp1, temp;
bool OpenMain, StartModbus, StartSchedule, ResetManualAttempt, YouHaveIncorrectInletOutletConfig;
uint8_t DefaultModeOnPowerup, ModbusModeState, ScheduleButtonState;
int16_t InletIndex1, OutletIndex1, InletIndex2, OutletIndex2, CurrentMode, VMorMUXState;
bool ModbusModeEnable, SchedulerModeEnable, TimerVisible, TimerVisible1, LoadVMorMUX, ScreenSaverActive, BrightessEntered, ModeIsActive, PauseMUX;
ul_timer_t ActivateScreenSaver, YouCantLeave, SetsettingsBrightness;
char Password[17]; seq_view_t Sequence1v, Sequence2v;

/* recording stubs */
static char order[256]; static int n_fill, n_fb, sel_at_do, sel_at_route;
static void rec(char c) { size_t n = strlen(order); if (n < sizeof order - 1) { order[n] = c; order[n + 1] = 0; } }
void Loading(void) { rec('L'); }
void FillNumbers(void) { rec('F'); n_fill++; }
void FB_ValveSelectionInter(void) { rec('V'); n_fb++; SelectedFirstInlet = 2; SelectedFirstOutlet = 1; }   /* a completed pair */
void SchedulerSequencesVM(void) { rec('Q'); }
void Route(void) { rec('R'); sel_at_route = SelectedFirstInlet * 10 + SelectedFirstOutlet; }
void ModbusMode(void) { rec('M'); }
void ScheduleMode(void) { rec('S'); ProcessTimer.Preset = 4321; }   /* the real ST writes it every scan (46e, V10) */
void ModbusValveNames(void) { rec('N'); }
void DI_Assigning(void) { rec('I'); }
void Email(void) { rec('E'); }
void DO_Control(void) { rec('D'); sel_at_do = SelectedFirstInlet * 10 + SelectedFirstOutlet; }
void Analog_signals(void) { rec('A'); }
void MUX(void) { rec('X'); }

static int fails, checks;
static void eqi(const char *w, long g, long x) { checks++; if (g != x) { fails++; printf("FAIL %-72s got %ld want %ld\n", w, g, x); } }
static void eqs(const char *w, const char *g, const char *x) { checks++; if (strcmp(g, x)) { fails++; printf("FAIL %-72s got \"%s\" want \"%s\"\n", w, g, x); } }

/* project-level action triggers: reset by the OS between scans (A4.15) */
static void os_reset(void) {
    LoadSystemConfig = LoadSettings = Load1_5 = Load1_9_2 = Load1_19_2 = LoadVMSettings = LoadMUXSettings = false;
    LoadDOMappingValves1_5 = LoadDOMappingValves1_9 = LoadDOMappingValves1_19 = false;
    ActivateScreenSaverBit = DeactivateScreenSaverBit = SetBrightness = false;
}
static void scan(int n) { for (int i = 0; i < n; i++) { order[0] = 0; Function1(); Ladder_Initial_Cycle = false; } }
static void cfg(int ni, int fi, int fo, int si, int so, int max) {
    NumInterconnections = (int16_t)ni; FirstInlets = (int16_t)fi; FirstOutlets = (int16_t)fo; SecondInlets = (int16_t)si; SecondOutlets = (int16_t)so;
    MaxNumberOfValves = (int16_t)max; }
static void reset(void) {
    ManualModeStart = StartSchedule = StartModbus = false; ModeIsActive = false; Touched_Bit = true; ScreenSaverActive = false;
    memset(InletValveTouch, 0, sizeof InletValveTouch); memset(OutletValveTouch, 0, sizeof OutletValveTouch);
    cfg(1, 3, 2, 0, 0, 10); scan(3); os_reset();
    YouHaveIncorrectInletOutletConfig = ResetManualAttempt = false; StartModbus = StartSchedule = false; n_fill = n_fb = 0;
    ResetSequences = ChangeVisibility = false; ScheduleButtonState = 0; ModbusModeState = 0;
}
static void powerup(void) { Ladder_Initial_Cycle = true; scan(1); }

int main(void) {
    /* ---------- 1. power-up ---------- */
    reset(); cfg(0, 5, 5, 5, 5, 10); powerup();
    eqi("power-up, no interconnection count: system config screen", LoadSystemConfig, 1);
    eqi("... counts set to 0", FirstInlets + FirstOutlets + SecondInlets + SecondOutlets, 0);
    eqi("... no main screen", Load1_5 + Load1_9_2 + Load1_19_2, 0);
    reset(); cfg(1, 3, 2, 0, 0, 10); memset(&Sequence1v, 0, sizeof Sequence1v); Sequence1v.NumberOfPeriods = 2;
    Sequence1v.Inlets[0] = 3; Sequence1v.Outlets[0] = 2; DefaultModeOnPowerup = 1; powerup();
    eqi("power-up, default schedule, valid sequence: schedule started", StartSchedule, 1);
    eqi("... button state 1", ScheduleButtonState, 1);
    eqi("... small main screen", Load1_5, 1);
    eqi("... mode number 2", CurrentMode, 2);
    eqi("... FillNumbers called (sequence 2 empty)", n_fill, 1);
    reset(); cfg(1, 3, 2, 0, 0, 10); Sequence1v.Inlets[5] = 4; DefaultModeOnPowerup = 1; powerup();
    eqi("power-up, default schedule, inlet 4 of 3 in a row: config error", YouHaveIncorrectInletOutletConfig, 1);
    eqi("... no schedule", StartSchedule, 0);
    eqi("... main screen still opened", Load1_5, 1);
    Sequence1v.Inlets[5] = 0;
    reset(); DefaultModeOnPowerup = 0; powerup();
    eqi("power-up, default Modbus: Modbus started, state 1", StartModbus * 10 + ModbusModeState, 11);
    eqi("... mode number 1", CurrentMode, 1);
    reset(); DefaultModeOnPowerup = 2; powerup();
    eqi("power-up, default manual: ResetManualAttempt", ResetManualAttempt, 1);
    reset(); DefaultModeOnPowerup = 3; powerup();
    eqi("power-up, default mode 3: no mode and no main screen", StartModbus + StartSchedule + ResetManualAttempt + Load1_5, 0);
    reset(); cfg(2, 3, 2, 0, 3, 20); DefaultModeOnPowerup = 0; powerup();
    eqi("power-up, 2 interconnections with an empty count: nothing started", StartModbus, 0);

    /* ---------- 2. password (rung 34) ---------- */
    reset(); Password[0] = 0; powerup();
    eqs("first scan, empty password: default written", Password, "<default>");
    strcpy(Password, "abc"); powerup();
    eqs("first scan, password set: kept", Password, "abc");

    /* ---------- 3. manual mode: the selection is cleared every scan (finding V21) ---------- */
    reset(); cfg(1, 3, 2, 0, 0, 10); ManualTimer.Preset = 600000; ManualModeStart = true; scan(2);
    InletValveTouch[1] = true; scan(1);
    eqi("V21 touch in manual mode: selection made", n_fb, 1);
    eqi("V21 ... the route is shown and the outputs driven in that scan", sel_at_route * 100 + sel_at_do, 2121);
    eqi("V21 ... selection is 0 at the end of the scan (rung 37)", SelectedFirstInlet + SelectedFirstOutlet, 0);
    scan(1);
    eqi("V21 next scan: outputs see no selection", sel_at_do, 0);
    eqi("V21 ... touch bit still set, so no new call", n_fb, 1);

    /* ---------- 4. PauseMUX is always OFF (finding V22) ---------- */
    reset(); ModeIsActive = true; ManualModeStart = true; ManualTimer.Preset = 600000; scan(1);
    InletValveTouch[2] = true; scan(1);
    eqi("V22 a VM route selected during the scan, MUX mode active: PauseMUX still OFF", PauseMUX, 0);

    /* ---------- 5. manual timers and the display (rungs 10-12, finding V10) ---------- */
    reset(); cfg(1, 3, 2, 0, 0, 10); ManualTimer.Preset = 1000; ManualModeStart = true; scan(3);
    eqi("V10 manual mode: the display holds the schedule's value at the end of the scan", ProcessTimer.Preset, 4321);
    scan(10);
    eqi("manual, 1 interconnection: ends when its timer ends (13 scans < 1 s + start)", ManualModeStart, 0);
    reset(); cfg(2, 3, 2, 2, 2, 20); ManualTimer.Preset = 1000; ManualTimer2.Preset = 3000; ManualModeStart = true; scan(15);
    eqi("manual, 2 interconnections: still on after timer 1 ends", ManualModeStart, 1);
    eqi("... both timers run (timer 1 done)", ManualTimer.Out, 1);
    eqi("... display 2 shows timer 2's remaining time", ProcessTimer2.Preset > 0 && ProcessTimer2.Preset < 3000, 1);
    scan(20);
    eqi("... ends when both are done", ManualModeStart, 0);
    eqi("manual end: touches cleared, valves enabled", InletValveTouch[1] + !EnableInletValve[5], 0);
    eqi("manual end: CurrentMode back to 0", CurrentMode, 0);

    /* ---------- 6. mode number and button enables (rungs 19, 20, 24-26) ---------- */
    reset(); StartSchedule = true; scan(1);
    eqi("schedule: mode 2, only its button enabled", CurrentMode * 100 + SchedulerModeEnable * 10 + ModbusModeEnable, 210);
    eqi("schedule, 1 interconnection: timer display 1", TimerVisible * 10 + TimerVisible1, 10);
    StartSchedule = false; scan(1);
    eqi("idle: mode 0, all buttons enabled", CurrentMode * 1000 + SchedulerModeEnable * 100 + ModbusModeEnable * 10 + ManualModeEnable, 111);

    /* ---------- 7. leaving settings, output-mapping screens (rungs 4-6, finding V20) ---------- */
    reset(); cfg(1, 0, 2, 0, 0, 10); CheckEmptySettingsBack = true; YouCantLeave.Preset = 2000; scan(1);
    eqi("leave settings with an empty count: stay, message shown", YouCantLeave.Out, 1);
    eqi("... no settings screen, bit cleared", LoadSettings + CheckEmptySettingsBack, 0);
    cfg(1, 3, 2, 0, 0, 10); CheckEmptySettingsBack = true; scan(1);
    eqi("leave settings with a valid count: settings screen", LoadSettings, 1);
    reset(); cfg(1, 3, 4, 0, 0, 10); LoadDOMappingValvesCheckEmpty = true; scan(1);
    eqi("mapping, 3+4 valves: 5-valve screen only", LoadDOMappingValves1_5 * 100 + LoadDOMappingValves1_9 * 10 + LoadDOMappingValves1_19, 100);
    os_reset(); cfg(1, 7, 12, 0, 0, 20); LoadDOMappingValvesCheckEmpty = true; scan(1);
    eqi("V20 mapping, 7 inlets + 12 outlets: both the 9 and 19 screens requested", LoadDOMappingValves1_9 * 10 + LoadDOMappingValves1_19, 11);
    os_reset(); cfg(2, 4, 3, 4, 3, 20); LoadDOMappingValvesCheckEmpty = true; scan(1);
    eqi("mapping, 2 interconnections, 8 + 6: 9-valve screen", LoadDOMappingValves1_9 * 10 + LoadDOMappingValves1_19, 10);
    os_reset(); cfg(3, 4, 3, 4, 3, 20); LoadDOMappingValvesCheckEmpty = true; scan(1);
    eqi("mapping, interconnection count 3: no screen, bit stays set (V6)", LoadDOMappingValvesCheckEmpty, 1);
    LoadDOMappingValvesCheckEmpty = false;

    /* ---------- 8. Interconnection settings ---------- */
    reset(); cfg(1, 6, 5, 0, 0, 10); Tube1InletEntered = true; scan(1);
    eqi("tube 1 inlets entered (6 + 5 > 10): outlets trimmed to fit", FirstInlets + FirstOutlets <= 10 && FirstInlets == 6, 1);
    eqi("... pictures recomputed (ChangeVisibility used and cleared in rung 8)", ChangeVisibility, 0);
    eqi("... both sequences to be erased", ResetSequences, 1);
    eqi("... ValueChanged = 1", ValueChanged, 1);
    reset(); ValueChanged = 0; cfg(1, 6, 4, 0, 0, 10); MaxNumberOfValves = 8; MaxNumberEntered = true; scan(1);
    eqi("G1 confirmed: only the maximum entered (no tube entry since power-up): no trim", FirstInlets + FirstOutlets, 10);
    reset(); ValueChanged = 2; cfg(1, 6, 4, 0, 0, 10); MaxNumberOfValves = 8; MaxNumberEntered = true; scan(1);
    eqi("G1 after an earlier outlet entry, the maximum trims as if outlets changed", FirstInlets + FirstOutlets <= 8, 1);
    reset(); cfg(2, 3, 2, 2, 2, 10); NumInterconnectionsEntried = false; scan(1); ChangeVisibility = false; ResetSequences = false;
    NumInterconnections = 1; NumInterconnectionsEntried = true; scan(1);
    eqi("V23 interconnections 2 -> 1: one-tube picture", OneInterconnectingTubeVisible * 10 + TwoInterconnectingTubeVisible, 10);
    eqi("V23 ... sequences erased", ResetSequences, 1);
    eqi("V23 ... but tube 2 counts kept (no trim call)", SecondInlets * 10 + SecondOutlets, 22);
    reset(); cfg(1, 3, 2, 0, 0, 3); NumInterconnections = 2; NumInterconnectionsEntried = true; scan(1);
    eqi("rung 1: fewer than 4 valves forces 1 interconnection", NumInterconnections, 1);
    reset(); cfg(1, 3, 2, 0, 0, 10); scan(1);
    eqi("rung 4: 5 valves of 10: back-flush option, 10-output config", BackFlushOptionVisib * 100 + BackFlushOptionVisib10 * 10 + ConfigWith10, 111);
    cfg(1, 5, 5, 0, 0, 10); scan(1);
    eqi("rung 4: 10 valves of 10: no back-flush option", BackFlushOptionVisib, 0);

    /* ---------- 9. 20 -> 10 outputs (rungs 5-10, findings V18, V19) ---------- */
    reset(); cfg(1, 3, 2, 0, 0, 20); scan(2);
    DOSingularInlet[0] = 10; DOSingularInlet[1] = 9; DOPurgeInlet = 10; DOInlet[0] = 12; DOInlet[1] = 9; BackFlushDONumber = 15;
    Di_FunctionVM[0] = 13; Di_FunctionVM[1] = 12; DI_FunctionMUX[0] = 30; DI_FunctionMUX[1] = 20;
    MaxNumberOfValves = 10; scan(1);
    eqi("V18 MUX inlet on output 10 (1-based, still valid): wiped to none", DOSingularInlet[0], 0);
    eqi("V18 ... output 9 kept", DOSingularInlet[1], 9);
    eqi("V18 ... purge on output 10 wiped", DOPurgeInlet, 0);
    eqi("G4 VM inlet on output 12 (0-based) moved to output 0", DOInlet[0], 0);
    eqi("... VM inlet on 9 kept; back-flush 15 -> 0", DOInlet[1] * 100 + BackFlushDONumber, 900);
    eqi("VM input code 13 (output 10) cleared, 12 kept", Di_FunctionVM[0] * 100 + Di_FunctionVM[1], 12);
    eqi("V19 MUX input code 30 (with module: drive output 6, still valid) cleared", DI_FunctionMUX[0], 0);
    eqi("V19 ... code 20 (feedback for valve 17, no longer exists) kept", DI_FunctionMUX[1], 20);

    /* ---------- 10. screen saver, brightness, settings screen (rungs 29-33) ---------- */
    reset(); Touched_Bit = false; VNC_Touched_Bit = false; ScreenSaverActive = true; ActivateScreenSaver.Preset = 1000;
    SetsettingsBrightness.Preset = 500;
    { int on = -1; for (int s = 0; s < 20; s++) { scan(1); if (ActivateScreenSaverBit && on < 0) on = s; os_reset(); }
      eqi("screen saver after 1 s without a touch (A4.10: 1 scan early)", on, 9); }
    Touched_Bit = true; scan(1);
    eqi("touch: screen saver off", DeactivateScreenSaverBit, 1);
    { int at = -1; for (int s = 0; s < 10; s++) { os_reset(); scan(1); if (SetBrightness && at < 0) at = s; }
      eqi("brightness restored when the 0.5 s pulse ends (5 scans incl. the touch scan, A4.10)", at, 3); }
    reset(); LoadVMorMUX = true; VMorMUXState = 1; scan(1);
    eqi("settings button, MUX state: MUX settings screen", LoadMUXSettings * 10 + LoadVMSettings, 10);

    /* ---------- 11. constants and call order ---------- */
    reset(); scan(1);
    eqi("rung 2: 22 inputs, so the extra module is always assumed", AdditionalDiscreteModule * 10 + WithoutModules, 10);
    eqs("call order (L Loading, Q editor, R Route, M Modbus, S schedule, N names, I/E inputs and e-mail, D outputs, A analogue, X MUX)",
        order, "LQRMSNIEDAX");
    eqi("no out-of-range write in 4B", g_out_of_range_writes, 0);

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
