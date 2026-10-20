/* test_4G.c - tests for 4G_faithful.c (VM ModbusMode, ModbusValveNames; MUX ModbusModeMUX, ModbusValveSelection).
 * Build: gcc -std=c11 -Wall -Wextra -O1 test_4G.c 4G_faithful.c ../4A/4A_faithful.c -lm
 * No rungs are disabled in these functions. MUX Modbus and ModbusValveNamesMUX are empty. */
#include <stdio.h>
#include <string.h>
#include "4G.h"

static int checks, fails;
static void eqi(const char *what, long got, long want) {
    checks++; if (got != want) { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); } else printf("ok   %s\n", what);
}
static void eqs(const char *what, const char *got, const char *want) {
    checks++; if (strcmp(got, want)) { fails++; printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want); } else printf("ok   %s\n", what);
}
static char sbuf[21];
static const char *bs(const uint8_t b[20]) { memcpy(sbuf, b, 20); sbuf[20] = 0; return sbuf; }   /* buffer as text */
static int all_zero(const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) if (b[i]) return 0; return 1; }
static int onehot(void) {           /* index of the only inlet on, -1 none, -2 several */
    int at = -1; for (int i = 0; i < 19; i++) if (SingularInletActivate[i]) { if (at >= 0) return -2; at = i; } return at;
}

/* ---------------- VM ModbusMode ---------------- */
static void reset_vm(void) {
    StartModbus = StartModbusCoil = false; ModbusMode();           /* settle the edge memories */
    ModbusModeNotActive = ValveSelectionErrorModbus = ResetMbError = FlushValveBitModbus = false;
    SelectedFirstInletMb = SelectedFirstOutletMb = SelectedSecondInletMb = SelectedSecondOutletMb = 0;
    SelectedFirstInlet = SelectedFirstOutlet = SelectedSecondInlet = SelectedSecondOutlet = 0;
    NumInterconnections = 1; FirstInlets = 3; FirstOutlets = 2; SecondInlets = 4; SecondOutlets = 2;
    ModbusTimer.Preset = 1000; ModbusTimer2.Preset = 2000; treset(&ModbusTimer); treset(&ModbusTimer2);
    ModbusModeState = 1; g_dt_ms = 10;
}
static void start_vm(int fi, int fo, int si, int so) {
    StartModbus = true; SelectedFirstInletMb = (int16_t)fi; SelectedFirstOutletMb = (int16_t)fo;
    SelectedSecondInletMb = (int16_t)si; SelectedSecondOutletMb = (int16_t)so; StartModbusCoil = true;
}

/* ---------------- MUX ---------------- */
static void reset_mux(void) {
    StartModbusMUX = StartModbusCoilMUX = false; StopSeqBit = PauseMUX = AnalyzerOkBitAlarm = false; ModbusModeMUX();
    ModbusNotActiveMUX = ValveSelectionErrorModbusMUX = ResetMbErrorMUX = PurgeInletActivateModbus = false;
    EnableLockValvesIfAnalyzerNotOk = false; NumberOfMUXValves = 5; SelectedValveMbMUX = 0; TempSelectedModbusValveMUX = 0;
    ModbusTimerMUX.Preset = 1000; treset(&ModbusTimerMUX); ModbusModeStateMUX = 1;
    memset(SingularInletActivate, 0, sizeof SingularInletActivate); g_dt_ms = 10;
}
/* 45c (ScheduleModeSTValveAssignMUX) with no schedule running: it runs before ModbusModeMUX every scan and
 * writes OFF to SingularInletActivate[SingulValves[k]] for k = 0..19 (unused rows hold 0, so inlet 1 too). */
static void idle_45c(const uint8_t sv[20]) { for (int k = 0; k < 20; k++) if (sv[k] <= 18) SingularInletActivate[sv[k]] = false; }

/* ---------------- names ---------------- */
static void reset_names(void) {
    memset(InletName, 0, sizeof InletName); memset(OutletName, 0, sizeof OutletName);
    memset(FirstInterconInletName, 0, sizeof FirstInterconInletName); memset(FirstInterconOutletName, 0, sizeof FirstInterconOutletName);
    memset(SecondInterconInletName, 0, sizeof SecondInterconInletName); memset(SecondInterconOutletName, 0, sizeof SecondInterconOutletName);
    memset(TempBuffer1, 0, sizeof TempBuffer1); memset(TempString, 0, sizeof TempString);
    ChangeModbusNamesVMFirstInlets = ChangeModbusNamesVMFirstOutlets = ChangeModbusNamesVMSecondInlets = ChangeModbusNamesVMSecondOutlets = false;
    LoadingWindow = false; g_tag2buf_whole = true; NumInterconnections = 1;
}
static int busy(void) { return ChangeModbusNamesVMFirstInlets || ChangeModbusNamesVMFirstOutlets || ChangeModbusNamesVMSecondInlets || ChangeModbusNamesVMSecondOutlets; }
static int run_names(void) { ChangeModbusNamesVM = true; int n = 0; do { ModbusValveNames(); n++; } while (busy() && n < 100); return n; }
static void fill_numbered(void) { for (int i = 0; i < 19; i++) { sprintf(InletName[i], "I%d", i); sprintf(OutletName[i], "O%d", i); } }

int main(void) {
    /* ===== 1. VM ModbusMode ===== */
    reset_vm(); StartModbusCoil = true; ModbusMode();
    eqi("R1: coil written while the mode is off: ModbusModeNotActive set, coil cleared", ModbusModeNotActive * 10 + StartModbusCoil, 10);

    reset_vm(); start_vm(2, 1, 0, 0); ModbusMode();
    eqi("R2: route copied from the master (inlet 2, outlet 1)", SelectedFirstInlet * 10 + SelectedFirstOutlet, 21);
    eqi("R2: no error, coil kept", ValveSelectionErrorModbus * 10 + StartModbusCoil, 1);
    eqi("R5: ProcessTimer.Preset is last scan's Current (store before the timer)", (long)ProcessTimer.Preset, 1000);
    ModbusMode();
    eqi("R5: second scan shows the time after one scan", (long)ProcessTimer.Preset, 990);
    SelectedFirstInlet = SelectedFirstOutlet = 0; ModbusMode();      /* Function1 rung 37 clears them (V21) */
    eqi("V21 does not affect Modbus: the route is rewritten every scan", SelectedFirstInlet * 10 + SelectedFirstOutlet, 21);

    reset_vm(); start_vm(4, 1, 0, 0); ModbusMode();
    eqi("R2: inlet 4 of 3 rejected: inlet and its register reset to 0", SelectedFirstInlet * 10 + SelectedFirstInletMb, 0);
    eqi("R2: the valid outlet is still written", SelectedFirstOutlet, 1);
    eqi("R2: error set and coil cleared", ValveSelectionErrorModbus * 10 + StartModbusCoil, 10);

    reset_vm(); start_vm(0, 1, 0, 0); ModbusMode();
    eqi("M3: inlet 0 (none) is an error, so the master cannot close the route and stay in the mode", ValveSelectionErrorModbus, 1);

    reset_vm(); start_vm(1, 1, 5, 1); ModbusMode();
    eqi("one interconnection: the second-tube registers are ignored", SelectedSecondInlet * 10 + ValveSelectionErrorModbus, 0);
    reset_vm(); NumInterconnections = 2; start_vm(1, 1, 5, 1); ModbusMode();
    eqi("two interconnections: second inlet 5 of 4 rejected", ValveSelectionErrorModbus * 10 + SelectedSecondInlet, 10);
    reset_vm(); NumInterconnections = 2; start_vm(1, 2, 4, 2); ModbusMode();
    eqi("two interconnections: full route accepted", SelectedFirstInlet * 1000 + SelectedFirstOutlet * 100 + SelectedSecondInlet * 10 + SelectedSecondOutlet, 1242);

    reset_vm(); start_vm(4, 1, 0, 0); ModbusMode(); ModbusModeNotActive = true; ResetMbError = true; ModbusMode();
    eqi("R3: ResetMbError clears both flags and itself", ModbusModeNotActive * 100 + ValveSelectionErrorModbus * 10 + ResetMbError, 0);
    reset_vm(); start_vm(4, 1, 0, 0); ModbusMode(); SelectedFirstInletMb = 2; StartModbusCoil = true; ModbusMode();
    eqi("R4: a new coil with a good route clears the error", ValveSelectionErrorModbus * 10 + SelectedFirstInlet, 2);

    reset_vm(); ModbusTimer.Preset = 50; start_vm(1, 1, 0, 0);
    { int n = 0; while (StartModbus && n < 100) { ModbusMode(); n++; } eqi("R6: a 50 ms run ends on scan 5 (A4.10)", n, 5); }
    eqi("R6: run end clears the selections, the coil and the state", SelectedFirstInlet + SelectedFirstOutlet + StartModbusCoil + ModbusModeState, 0);

    reset_vm(); NumInterconnections = 2; ModbusTimer.Preset = 50; ModbusTimer2.Preset = 100; start_vm(1, 1, 1, 1);
    ModbusMode(); eqi("R5: ProcessTimer2.Preset is this scan's Current (store after the timer)", (long)ProcessTimer2.Preset, 90);
    { int n = 1; while (StartModbus && n < 100) { ModbusMode(); n++; } eqi("R6: two interconnections end when both timers are done (scan 10)", n, 10); }

    reset_vm(); ModbusTimer.Preset = 0; start_vm(1, 1, 0, 0); ModbusMode();
    eqi("M9: preset 0 (RAM, power-up) ends the run in the same scan as the coil", StartModbus * 10 + SelectedFirstInlet, 0);

    reset_vm(); start_vm(4, 1, 0, 0); for (int i = 0; i < 300; i++) ModbusMode();
    eqi("M5: after a selection error the mode stays on and its timer is held at the preset", StartModbus * 10000L + (long)ModbusTimer.Current, 11000);

    reset_vm(); start_vm(2, 1, 0, 0); ModbusMode(); StartModbus = false; ModbusMode();
    eqi("operator stop (falling edge): selections and coil cleared", SelectedFirstInlet + SelectedFirstOutlet + StartModbusCoil, 0);
    eqi("operator stop: R1 runs first and also sets ModbusModeNotActive", ModbusModeNotActive, 1);

    reset_vm(); FlushValveBitModbus = true; ModbusMode();
    eqi("R5: back-flush bit cleared while the mode is off", FlushValveBitModbus, 0);
    reset_vm(); StartModbus = true; FlushValveBitModbus = true; ModbusMode();
    eqi("R5: back-flush bit kept while the mode is on", FlushValveBitModbus, 1);

    /* ===== 2. MUX ModbusModeMUX and ModbusValveSelection ===== */
    reset_mux(); StartModbusCoilMUX = true; ModbusModeMUX();
    eqi("MUX R1: coil while the mode is off: not-active flag, coil cleared", ModbusNotActiveMUX * 10 + StartModbusCoilMUX, 10);
    reset_mux(); StartModbusMUX = true; SelectedValveMbMUX = 3; StartModbusCoilMUX = true; ModbusModeMUX();
    eqi("MUX: valve 3 opens inlet index 2 only", onehot(), 2);
    eqi("MUX R5: remaining time shown (TA, store after the timer)", (long)ProcessTimerMUX.Preset, 990);
    reset_mux(); StartModbusMUX = true; SelectedValveMbMUX = 6; StartModbusCoilMUX = true; ModbusModeMUX();
    eqi("M1: valve 6 with 5 valves is accepted (check is > N) and opens index 5", ValveSelectionErrorModbusMUX * 100 + onehot(), 5);
    reset_mux(); StartModbusMUX = true; SelectedValveMbMUX = 7; StartModbusCoilMUX = true; ModbusModeMUX();
    eqi("MUX: valve 7 with 5 valves: error, register reset, coil cleared", ValveSelectionErrorModbusMUX * 100 + SelectedValveMbMUX * 10 + StartModbusCoilMUX, 100);
    reset_mux(); StartModbusMUX = true; SelectedValveMbMUX = 0; StartModbusCoilMUX = true; ModbusModeMUX();
    eqi("M3: valve 0 (none) is an error", ValveSelectionErrorModbusMUX, 1);
    reset_mux(); NumberOfMUXValves = 19; StartModbusMUX = true; SelectedValveMbMUX = 20; StartModbusCoilMUX = true; ModbusModeMUX();
    eqi("M1: valve 20 with 19 valves: no error and every inlet off", ValveSelectionErrorModbusMUX * 100 + onehot(), -1);

    reset_mux(); StartModbusMUX = true; SelectedValveMbMUX = 2; StartModbusCoilMUX = true;
    for (int i = 0; i < 3; i++) ModbusModeMUX();
    StopSeqBit = true; ModbusModeMUX();
    eqi("MUX stop: rising edge clears the inlets", onehot(), -1);
    for (int i = 0; i < 5; i++) ModbusModeMUX();
    eqi("MUX stop: accumulating timer holds", (long)ModbusTimerMUX.Current, 970);
    StopSeqBit = false; ModbusModeMUX();
    eqi("MUX stop released: inlet back on and timer continues", onehot() * 1000L + (long)ModbusTimerMUX.Current, 1960);
    PauseMUX = true; ModbusModeMUX();
    eqi("MUX pause: rising edge clears the inlets (V22: PauseMUX is never on in the original)", onehot(), -1);
    PauseMUX = false; ModbusModeMUX();
    EnableLockValvesIfAnalyzerNotOk = true; AnalyzerOkBitAlarm = true; ModbusModeMUX();
    eqi("MUX analyser alarm with the lock on: inlets cleared", onehot(), -1);
    EnableLockValvesIfAnalyzerNotOk = false; AnalyzerOkBitAlarm = false; ModbusModeMUX(); AnalyzerOkBitAlarm = true; ModbusModeMUX();
    eqi("MUX analyser alarm with the lock off: ignored, inlet stays on", onehot(), 1);

    reset_mux(); ModbusTimerMUX.Preset = 30; treset(&ModbusTimerMUX); StartModbusMUX = true; SelectedValveMbMUX = 2; StartModbusCoilMUX = true;
    { int n = 0; while (StartModbusMUX && n < 100) { ModbusModeMUX(); n++; } eqi("MUX R6: 30 ms run ends on scan 3", n, 3); }
    eqi("MUX R6: register, coil and state cleared, timer reset", SelectedValveMbMUX + StartModbusCoilMUX + ModbusModeStateMUX + ModbusTimerMUX.Out + (ModbusTimerMUX.Current != 30), 0);
    eqi("M2: the last inlet is left on at the end of the run", onehot(), 1);
    { uint8_t sv[20] = { 3, 4 }; idle_45c(sv); ModbusModeMUX();
      eqi("M2: it stays on when it is not in the stored MUX sequence (45c only rewrites listed inlets)", onehot(), 1);
      uint8_t sv2[20] = { 1, 4 }; idle_45c(sv2);
      eqi("M2: it is cleared next scan when the stored sequence lists it", onehot(), -1); }

    reset_mux(); ModbusTimerMUX.Preset = 0; treset(&ModbusTimerMUX); StartModbusMUX = true; SelectedValveMbMUX = 4; StartModbusCoilMUX = true; ModbusModeMUX();
    eqi("M9: MUX preset 0 (RAM, power-up): run ends in the first scan with the inlet left on", StartModbusMUX * 100 + onehot(), 3);
    reset_mux(); StartModbusMUX = true; SelectedValveMbMUX = 9; StartModbusCoilMUX = true; for (int i = 0; i < 300; i++) ModbusModeMUX();
    eqi("M5: MUX after an error the mode stays on and the timer holds", StartModbusMUX * 10000L + (long)ModbusTimerMUX.Current, 11000);
    ResetMbErrorMUX = true; ModbusNotActiveMUX = true; ModbusModeMUX();
    eqi("MUX R3: reset clears both flags and itself", ValveSelectionErrorModbusMUX + ModbusNotActiveMUX + ResetMbErrorMUX, 0);
    reset_mux(); PurgeInletActivateModbus = true; ModbusModeMUX();
    eqi("MUX R5: Modbus purge bit cleared while the mode is off", PurgeInletActivateModbus, 0);
    TempSelectedModbusValveMUX = -1; SingularInletActivate[4] = true; ModbusValveSelection();
    eqi("ModbusValveSelection: out of range turns every inlet off", onehot(), -1);
    TempSelectedModbusValveMUX = 18; ModbusValveSelection();
    eqi("ModbusValveSelection: 18 opens the last inlet", onehot(), 18);

    /* ===== 3. VM ModbusValveNames ===== */
    reset_names(); FirstInlets = 3; FirstOutlets = 2;
    strcpy(InletName[0], "A"); strcpy(InletName[1], "BB"); strcpy(InletName[2], "CCC"); strcpy(OutletName[0], "X"); strcpy(OutletName[1], "Y");
    eqi("names: one position per scan, 3 inlets take 3 scans", run_names(), 3);
    eqs("names: inlet 1", bs(FirstInterconInletName[0]), "A");
    eqs("names: inlet 2", bs(FirstInterconInletName[1]), "BB");
    eqs("names: inlet 3", bs(FirstInterconInletName[2]), "CCC");
    eqs("names: outlet 1", bs(FirstInterconOutletName[0]), "X");
    eqs("names: outlet 2 (whole-tag reading of A4.17)", bs(FirstInterconOutletName[1]), "Y");
    eqi("names: LoadingWindow off at the end", LoadingWindow, 0);
    reset_names(); g_tag2buf_whole = false; FirstInlets = 3; FirstOutlets = 2;
    strcpy(InletName[0], "A"); strcpy(InletName[1], "BB"); strcpy(InletName[2], "CCC"); strcpy(OutletName[0], "X"); strcpy(OutletName[1], "Y");
    run_names();
    eqs("M12: characters-only reading of A4.17: outlet 2 keeps a stale tail from inlet 2", bs(FirstInterconOutletName[1]), "YB");

    reset_names(); FirstInlets = 3; FirstOutlets = 2;
    strcpy(InletName[0], "A"); strcpy(InletName[2], "C"); strcpy(OutletName[0], "X"); strcpy(OutletName[1], "Y");
    run_names();
    eqs("M10: blank inlet 2: its number lands in inlet 1's buffer", bs(FirstInterconInletName[0]), "2");
    eqs("M10: blank inlet 2: its own buffer gets the leftover (outlet 1's name)", bs(FirstInterconInletName[1]), "X");
    eqs("M10: inlet 3 normal", bs(FirstInterconInletName[2]), "C");

    reset_names(); FirstInlets = 2; FirstOutlets = 1; strcpy((char *)TempBuffer1, "OLD");
    strcpy(InletName[1], "B"); strcpy(OutletName[0], "X");
    run_names();
    eqs("M10: blank inlet 1: its number is overwritten by the leftover in the same scan", bs(FirstInterconInletName[0]), "OLD");

    reset_names(); FirstInlets = 2; FirstOutlets = 2;
    strcpy(InletName[0], "A"); strcpy(InletName[1], "B"); strcpy(OutletName[1], "Y");
    run_names();
    eqs("M10: blank outlet 1: its number goes into inlet 1's buffer", bs(FirstInterconInletName[0]), "1");
    eqs("M10: blank outlet 1: its own buffer gets inlet 1's name", bs(FirstInterconOutletName[0]), "A");

    reset_names(); fill_numbered(); NumInterconnections = 2; FirstInlets = 2; FirstOutlets = 2; SecondInlets = 3; SecondOutlets = 2;
    run_names();
    eqs("M11: small layout, second-tube valve 3 (ID 6) lands in SecondInterconInletName4", bs(SecondInterconInletName[3]), "I6");
    eqs("M11: valve 2 (ID 7) in Name3", bs(SecondInterconInletName[2]), "I7");
    eqs("M11: valve 1 (ID 8) in Name2", bs(SecondInterconInletName[1]), "I8");
    eqi("M11: SecondInterconInletName1 is never written", all_zero(SecondInterconInletName[0], 20), 1);
    eqs("M11: second-tube outlet 2 (ID 7) in SecondInterconOutletName3", bs(SecondInterconOutletName[2]), "O7");
    eqs("M11: second-tube outlet 1 (ID 8) in SecondInterconOutletName2", bs(SecondInterconOutletName[1]), "O8");
    eqs("small layout: first tube unaffected", bs(FirstInterconInletName[1]), "I1");

    reset_names(); fill_numbered(); NumInterconnections = 2; FirstInlets = 8; FirstOutlets = 3; SecondInlets = 4; SecondOutlets = 3;
    run_names();
    eqs("large layout (inlets 12): second-tube valve 4 (ID 15) in Name5", bs(SecondInterconInletName[4]), "I15");
    eqs("large layout: valve 1 (ID 18) in Name2", bs(SecondInterconInletName[1]), "I18");
    eqs("large layout wins for the outlets too (total 6): outlet 3 (ID 16) in Name4", bs(SecondInterconOutletName[3]), "O16");

    reset_names(); fill_numbered(); InletName[8][0] = 0; NumInterconnections = 2; FirstInlets = 2; FirstOutlets = 2; SecondInlets = 3; SecondOutlets = 2;
    run_names();
    eqs("M10/M11: blank second-tube valve 1: number \"2\" written into FirstInterconInletName1", bs(FirstInterconInletName[0]), "2");
    eqs("M10: its own slot gets the leftover (second-tube outlet ID 8)", bs(SecondInterconInletName[1]), "O8");

    reset_names(); fill_numbered(); FirstInlets = 12; FirstOutlets = 1; ChangeModbusNamesVM = true;
    { int n = 0; for (; n < 10; n++) ModbusValveNames();
      eqi("rungs 4/5: still loading after 10 scans", LoadingWindow * 10 + ChangeModbusNamesVMFirstInlets, 11);
      ModbusValveNames(); n++;
      eqi("rungs 4/5: IDs 10 and 11 both handled in scan 11, list done", LoadingWindow * 10 + ChangeModbusNamesVMFirstInlets, 0); }
    eqs("rungs 4/5: inlet 11", bs(FirstInterconInletName[10]), "I10");
    eqs("rungs 4/5: inlet 12", bs(FirstInterconInletName[11]), "I11");

    reset_names(); fill_numbered(); memset(FirstInterconInletName, 0x55, sizeof FirstInterconInletName); FirstInlets = 1; FirstOutlets = 3;
    ChangeModbusNamesVM = true; ModbusValveNames();
    eqi("rungs 1-2: every buffer cleared first (inlet 2..19 zero)", all_zero(FirstInterconInletName[1], 18 * 20), 1);
    eqi("M13: LoadingWindow off once the inlets are done, outlets still going", LoadingWindow * 10 + ChangeModbusNamesVMFirstOutlets, 1);

    reset_names(); fill_numbered(); NumInterconnections = 2; FirstInlets = 2; FirstOutlets = 2; SecondInlets = 0; SecondOutlets = 1;
    run_names();
    eqi("second tube with 0 inlets: nothing written", all_zero(SecondInterconInletName[0], 18 * 20), 1);

    Modbus(); ModbusValveNamesMUX();
    eqi("MUX Modbus and ModbusValveNamesMUX are empty (M8: ValveNameMUX1..19 never written)", 1, 1);

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
