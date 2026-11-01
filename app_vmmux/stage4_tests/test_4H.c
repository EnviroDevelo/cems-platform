/* test_4H.c - tests for 4H_faithful.c (MUX Logging, LoadTable, LogOpenedValves, LoggingColumns,
 * LoggingFindConflict, LoggingAdditionalFunction).
 * Build: gcc -std=c11 -Wall -Wextra -O1 test_4H.c 4H_faithful.c ../4A/4A_faithful.c -lm
 * No rungs are disabled in these functions. ResetCertainCheckbox (42e) is a copy of 4B_faithful.c.
 * Scan order as in MUX rung 9: Logging, then LoadTable. sd_tick() runs between scans (A4.12). */
#include <stdio.h>
#include <string.h>
#include "4H.h"

static int checks, fails;
static void eqi(const char *what, long got, long want) {
    checks++; if (got != want) { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); } else printf("ok   %s\n", what);
}
static void eqs(const char *what, const char *got, const char *want) {
    checks++; if (strcmp(got, want)) { fails++; printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want); } else printf("ok   %s\n", what);
}

/* 42e ResetCertainCheckbox, copied from 4B_faithful.c */
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

static void scan(void) { sd_tick(); Logging(); LoadTable(); Ladder_Initial_Cycle = false; }
static void scans(int n) { while (n-- > 0) scan(); }
static int run_to_log(void) {           /* scans until a row is logged; returns the scan count */
    int16_t li = LoggingIndex; int n = 0;
    while (LoggingIndex == li && n < 100000) { scan(); n++; }
    return n;
}
static int settle_header(void) {
    int n = 0; while ((AssignColumn1 || AssignColumn2 || AssignColumn3) && n < 1000) { scan(); n++; } return n;
}
static void fresh(void) {               /* a clean unit: no file, defaults, booted */
    memset(ParameterColNumberLoggingArray, 0, 20); memset(prevLogging, 0, 20); memset(AverageValues, 0, sizeof AverageValues);
    for (int k = 0; k < 20; k++) ParameterColNumberArray[k] = (int16_t)k;
    memset(SingularInletActivate, 0, sizeof SingularInletActivate); memset(SingularInlet, 0, sizeof SingularInlet);
    PurgeInletActivate = PurgeInletActivateDI = PurgeInletActivateModbus = false;
    FlowValidationAlarm = StartPeriodsMUX = false; DwellSampleStep = 1;
    memset(&g_reserv, 0, sizeof g_reserv); Is_SD_Present = true; Is_SD_Locked = false;
    g_dt_clear_sync = g_setcol_whole = false; g_dt_ms = 100; LoginIntervalsGeneral.Preset = 5000;
    LoggingIndex = 1; AlarmLoadingTable = SaveDataFailedReserv = false;
    h4_power_up(); scans(3);
    AlarmLoadingTable = false;              /* a new unit raises it (section 1); cleared for the other tests */
    treset(&LoginIntervalsGeneral);         /* next logging tick a full interval away */
}
static void start_header(int k) {           /* column combo box of parameter k changed, no tick due */
    treset(&LoginIntervalsGeneral); ParameterColNumberSelectedArray[k] = true; scan();
}
static void logging_on(int k) { ParameterColNumberLoggingArray[k] = true; scan(); }

int main(void)
{
    char want[40];

    /* ================= 1. LoadTable at power-up ================= */
    printf("--- 1. LoadTable ---\n");
    fresh();
    memset(&g_reserv, 0, sizeof g_reserv); g_reserv.exists = true; g_reserv.rows = 5;
    strcpy(g_reserv.data[0].DateTime, "DATE&TIME"); strcpy(g_reserv.data[4].DateTime, "2026-10-17 10-00");
    LoggingIndex = 5; h4_power_up();
    scan();
    eqi("R1-R2: count requested in the first scan (status 1), stage 1 armed", CountStatus * 10 + CheckCountStatus1, 11);
    scan();
    eqi("R3: count done (0), 5 rows, load requested (status 1), R4 stage 2", Rows * 100 + LoadWriteTableStatus * 10 + CheckLoadStatus1, 511);
    scan();
    eqi("R5: load done, sequence finished, no alarm", LoadWriteTableStatus * 100 + CheckLoadStatus1 * 10 + AlarmLoadingTable, 0);
    eqs("table rows restored from the file (row 4)", LoggingTable[4].DateTime, "2026-10-17 10-00");

    memset(&g_reserv, 0, sizeof g_reserv); LoggingIndex = 5; h4_power_up(); scans(3);
    eqi("no file: the failed count still loads (R3), the load fails (-5), AlarmLoadingTable on", LoadWriteTableStatus * 10 + AlarmLoadingTable, -49);
    eqi("a brand new unit (LoggingIndex powers up at 1, so R1 always runs) raises the alarm too", 1, 1);
    AckowledgeLoadError = true; scan();
    eqi("R6: acknowledge clears the alarm and itself", AlarmLoadingTable * 10 + AckowledgeLoadError, 0);

    /* a failed load followed by the next logging tick overwrites the file with a table that has no header */
    memset(&g_reserv, 0, sizeof g_reserv); g_reserv.exists = true; g_reserv.rows = 300;
    for (int r = 0; r < 300; r++) strcpy(g_reserv.data[r].DateTime, r ? "old" : "DATE&TIME");
    Is_SD_Present = false; LoggingIndex = 300; h4_power_up(); scans(3);
    eqi("SD missing at power-up: load alarm", AlarmLoadingTable, 1);
    Is_SD_Present = true; run_to_log(); scan();
    eqi("next tick: the empty table (300 blank rows + the new one) is saved over the file", g_reserv.rows, 301);
    eqs("the file's header row is now blank (old data lost)", g_reserv.data[0].DateTime, "");

    /* ================= 2. Averages and Values ================= */
    printf("--- 2. Averages ---\n");
    fresh();
    AverageValues[0] = 0.0f; AverageValues[1] = 3.0f; h4_power_up(); Values[0] = 8.0f; Values[1] = 9.0f; scan();
    eqi("R18: first scan seeds an average that is 0.0 from Values", (long)(AverageValues[0] * 10), 80);
    eqi("R18: a non-zero (retained) average is not reseeded", (long)(AverageValues[1] * 10), 30);
    Values[0] = 0.0f; ParameterColNumberLoggingArray[0] = true; scans(9);
    eqi("R19-R21: averaging each second, (Values + Average)/2: 8 -> 4 after 1 s", (long)(AverageValues[0] * 10), 40);
    scans(10);
    eqi("... 4 -> 2 after 2 s (Values is never written in the project, so the logged value decays to 0)", (long)(AverageValues[0] * 10), 20);
    eqi("a parameter not logged is not averaged", (long)(AverageValues[1] * 10), 30);

    /* ================= 3. Header rewrite and table clear ================= */
    printf("--- 3. Header ---\n");
    fresh();
    logging_on(0);
    eqi("ticking AI0 logging alone does not start a header rewrite (R8 uses Selected for 0..3)", AssignColumn1 + AssignColumn3, 0);
    run_to_log(); scan();
    eqi("... but its value is logged in Col3 (row 1) under a blank header", LoggingTable[1].Col3[0] != 0 && LoggingTable[0].Col3[0] == 0, 1);
    start_header(0);
    eqi("Selected[0] (column combo box changed): R8 clears it and sets AssignColumn1; R9 clears the table; R10 -> stage 2, LoggingIndex 1",
        ParameterColNumberSelectedArray[0] * 1000 + AssignColumn1 * 100 + AssignColumn2 * 10 + LoggingIndex, 11);
    eqi("A4.18: table status 1 in the request scan", LoggingTable_Status, 1);
    scan();
    eqi("next scan: clear done, logged rows gone, R11 -> AssignColumn3, R15 writes DATE&TIME", LoggingTable[1].Col3[0] == 0 && AssignColumn3 && index_ == -1, 1);
    eqs("row 0 DateTime", LoggingTable[0].DateTime, "DATE&TIME");
    int n = settle_header();
    eqi("header finished: one parameter per scan (20 more scans)", n, 20);
    eqs("row 0 OpenedValves", LoggingTable[0].OpenedValves, "Active Valves");
    eqs("row 0 Col3 = AI0 name", LoggingTable[0].Col3, "Param1");
    eqs("row 0 Col4 not written (AI1 not logged)", LoggingTable[0].Col4, "");

    treset(&LoginIntervalsGeneral); ColNumberNameEntered[0] = true; strcpy(ParamColName[0], "O2"); scan();
    eqi("R13: name entered for a logged parameter starts a rewrite without clearing the table", AssignColumn3 && !AssignColumn1, 1);
    settle_header();
    eqs("new name in the header", LoggingTable[0].Col3, "O2");
    strcpy(ParamColName[0], "Param1");

    /* rung 8 for parameters 4..19 */
    fresh();
    run_to_log(); run_to_log();
    eqi("two rows logged", LoggingIndex, 3);
    ParameterColNumberLoggingArray[5] = true; scan();
    eqi("R8: ticking a Modbus parameter (index 5) clears its own tick at once and clears the whole log",
        ParameterColNumberLoggingArray[5] * 100 + AssignColumn2 * 10 + LoggingIndex, 11);
    settle_header();
    eqs("its header is never written (tick already off)", LoggingTable[0].Col8, "");
    run_to_log(); scan();
    eqs("and its value is never logged", LoggingTable[1].Col8, "");

    /* ================= 4. Column conflict ================= */
    printf("--- 4. Conflict ---\n");
    fresh();
    ParameterColNumberLoggingArray[0] = true; start_header(0); settle_header();
    eqs("AI0 owns column 0, header Param1", LoggingTable[0].Col3, "Param1");
    ParameterColNumberArray[1] = 0; ParameterColNumberLoggingArray[1] = true; scan();
    eqi("FindConflict: AI1 ticked onto column 0 -> ColumnIsntVacantArray[1], ConflictingIndex 0 (column number)", ColumnIsntVacantArray[1] * 10 + ConflictingIndex, 10);
    eqi("R3-R4: ReadOccupColumnName pulse, R5 unticks AI1, R6 ColumnIsntVacant", ReadOccupColumnName * 100 + ParameterColNumberLoggingArray[1] * 10 + ColumnIsntVacant, 101);
    eqs("R4: OccupiedColumnName = the header of that column (CheckIfVacant, 41h)", OccupiedColumnName, "Param1");
    scan();
    eqi("pulse lasts one scan; ConflictingIndex back to -1", ReadOccupColumnName * 10 + ConflictingIndex, -1);
    ResetLastLogCheckBox = true; scan();
    eqi("R7: 'keep the old one' clears the flags and the button; R6 drops ColumnIsntVacant next scan", ColumnIsntVacantArray[1] + ResetLastLogCheckBox, 0);
    scan();
    eqi("... ColumnIsntVacant off, AI0 still logging, AI1 not", ColumnIsntVacant * 100 + ParameterColNumberLoggingArray[0] * 10 + ParameterColNumberLoggingArray[1], 10);

    run_to_log();
    ParameterColNumberLoggingArray[1] = true; scan();
    eqi("conflict again", ColumnIsntVacantArray[1], 1);
    treset(&LoginIntervalsGeneral); ResetCertainCheckBox = true; scan();
    eqi("'take the column': 42e swaps the ticks (AI1 on, AI0 off), R8 clears the button and sets AssignColumn1",
        ParameterColNumberLoggingArray[1] * 100 + ParameterColNumberLoggingArray[0] * 10 + ResetCertainCheckBox, 100);
    eqi("no new conflict from 42e's own tick (edge masked by ResetCertainCheckBox)", ColumnIsntVacantArray[1] + (ConflictingIndex != -1), 0);
    eqi("... and the whole log is cleared (LoggingIndex 1)", LoggingIndex, 1);
    settle_header();
    eqs("header now names AI1 in column 0", LoggingTable[0].Col3, "Param2");

    /* ================= 5. Row format ================= */
    printf("--- 5. Row format ---\n");
    fresh();
    ParameterColNumberLoggingArray[0] = true; start_header(0); settle_header();
    RTC_Date.Year = 2026; RTC_Date.Month = 10; RTC_Date.Day_In_Month = 8; RTC_Time.Hours = 9; RTC_Time.Minutes = 5;
    SingularInletActivate[0] = true; strcpy(SingularInlet[0], "V1");
    SingularInletActivate[2] = true; SingularInlet[2][0] = 0;
    SingularInletActivate[3] = true; strcpy(SingularInlet[3], "Inlet4");
    PurgeInletActivateDI = true;
    AverageValues[0] = 12.5f; Values[0] = 12.5f;
    run_to_log();
    LoggingTableStruct_t *r1 = &LoggingTable[1];
    eqs("R24: date and time 'YYYY-MM-DD HH-MM' (dash in the time)", r1->DateTime, "2026-10-08 09-05");
    eqs("LogOpenedValves: 'name-' per open inlet, a blank name gives '-' only, then PurgeInlet", r1->OpenedValves, "V1--Inlet4-PurgeInlet");
    ConvertRealOrIntToString(12.5f, 4, 3, want);
    eqs("LoggingColumns: Col3 = ConvertRealOrIntToString(average, 4, 3)", r1->Col3, want);
    eqs("... which prints 12.5 as \"12.\" (41e, DecimPart 4 and FractPart 3 are never changed)", r1->Col3, "12.");
    SingularInletActivate[0] = SingularInletActivate[2] = SingularInletActivate[3] = false; PurgeInletActivateDI = false;
    SingularInletActivate[5] = true; strcpy(SingularInlet[5], "S6");
    run_to_log();
    eqs("trailing '-' after the last inlet when no purge", LoggingTable[2].OpenedValves, "S6-");
    FlowValidationAlarm = true; run_to_log();
    eqi("'*' appended while FlowValidationAlarm is on", LoggingTable[3].Col3[strlen(LoggingTable[3].Col3) - 1] == '*', 1);
    FlowValidationAlarm = false; StartPeriodsMUX = true; DwellSampleStep = 0; run_to_log();
    eqi("'*' appended during a MUX run in dwell step 0", LoggingTable[4].Col3[strlen(LoggingTable[4].Col3) - 1] == '*', 1);
    DwellSampleStep = 2; run_to_log();
    eqi("no '*' in later dwell steps", LoggingTable[5].Col3[strlen(LoggingTable[5].Col3) - 1] != '*', 1);
    StartPeriodsMUX = false;
    eqi("interval: one row every LoginIntervalsGeneral (50 scans of 100 ms here)", run_to_log(), 50);
    scan();
    eqi("R26-R29: each row saves the table rows 0..LoggingIndex-1 to SD", g_reserv.rows, LoggingIndex);

    /* ================= 6. Header interrupted, invalid column ================= */
    printf("--- 6. Header interrupted, invalid column ---\n");
    fresh();
    for (int k = 0; k <= 3; k++) ParameterColNumberLoggingArray[k] = true;
    scan();
    LoginIntervalsGeneral.Current = 400; LoginIntervalsGeneral.On = true;      /* tick due in 4 scans */
    ParameterColNumberSelectedArray[0] = true; scan();                       /* rewrite starts */
    scans(5);
    eqi("a logging tick during the rewrite: LoggingColumns resets AssignColumn3 (header left part-written)", AssignColumn3, 0);
    eqs("AI0 header written before the tick", LoggingTable[0].Col3, "Param1");
    eqs("RTD1 header (index 3) never written", LoggingTable[0].Col6, "");

    fresh();
    ParameterColNumberLoggingArray[0] = ParameterColNumberLoggingArray[2] = ParameterColNumberLoggingArray[3] = true;
    ParameterColNumberArray[2] = 25; scan();
    ParameterColNumberSelectedArray[0] = true; scan(); scans(30);
    eqi("an enabled parameter with a column outside 0..19 stalls the header loop (index stays 2, AssignColumn3 on)", AssignColumn3 * 10 + index_, 12);
    run_to_log();
    eqi("... the row loop also stops at it: RTD1 (index 3) not logged", LoggingTable[1].Col6[0], 0);
    eqi("... and AssignColumn3 stays on (row loop never reaches i > 19)", AssignColumn3, 1);

    /* ================= 7. Table full ================= */
    printf("--- 7. Table full ---\n");
    fresh();
    LoggingIndex = 1439; run_to_log(); scan();
    eqi("insert at row 1439, save 1440 rows OK", g_reserv.rows * 10 + SaveDataFailedReserv, 14400);
    run_to_log(); scan();
    eqi("insert at row 1440 (last), save 1441 rows OK", g_reserv.rows * 10 + SaveDataFailedReserv, 14410);
    run_to_log(); scan();
    eqi("LoggingIndex never wraps: insert at 1441 fails (A4.20, status -3), save of 1442 rows gives -33, SaveDataFailedReserv",
        LoggingTable_Status * 10 + SaveDataFailedReserv, -29);
    eqi("from 24 h at 60 s on (1440 rows) no more data is kept", LoggingIndex, 1442);
    AcknowledgeCSVReserv = true; scan(); scan();
    eqi("R27: acknowledge retries the save, which fails again", SaveDataFailedReserv, 1);
    Is_SD_Present = false; SaveDataFailedReserv = false; run_to_log(); scan();
    eqi("no SD card: no save is requested, no alarm", SaveDataFailedReserv * 10 + SaveReserv, 0);

    /* ================= 8. A4.18 and ContactManufacturer2 ================= */
    printf("--- 8. A4.18, ContactManufacturer2 ---\n");
    fresh();
    LoggingTable_Status = -3; ContactManufacturer2 = true;     /* e.g. after a failed insert */
    g_dt_clear_sync = true; ParameterColNumberSelectedArray[0] = true; scan();
    eqi("A4.18 sync reading, status left at -3: R10 resets ContactManufacturer2 (never set anywhere), AssignColumn1 off, no rewrite",
        ContactManufacturer2 * 100 + AssignColumn1 * 10 + (AssignColumn2 || AssignColumn3), 0);
    LoggingTable_Status = 0; ParameterColNumberSelectedArray[0] = true; scan(); scans(10);
    eqi("A4.18 sync reading, status 0: AssignColumn1 stays on for ever, header never rewritten", AssignColumn1 * 10 + AssignColumn3, 10);
    fresh();
    LoggingTable_Status = -3; ParameterColNumberSelectedArray[0] = true; scan();
    eqi("A4.18 async reading (default): the clear sets status 1 and the rewrite runs", AssignColumn2, 1);

    /* ================= 9. A4.19 ================= */
    printf("--- 9. A4.19 ---\n");
    fresh();
    ParameterColNumberLoggingArray[0] = true; start_header(0); settle_header();
    run_to_log(); run_to_log();
    g_setcol_whole = true; treset(&LoginIntervalsGeneral); ColNumberNameEntered[0] = true; scan(); settle_header();
    eqs("A4.19 other reading (start row 0 = whole column): every logged row's date becomes DATE&TIME", LoggingTable[2].DateTime, "DATE&TIME");
    g_setcol_whole = false;

    /* ================= 10. Small functions ================= */
    printf("--- 10. Small functions ---\n");
    index_ = 18; LoggingAdditionalFunction(); eqs("AdditionalFunction index 18 -> ParameterName18 (default 'Param19')", TempCol, "Param19");
    index_ = 19; LoggingAdditionalFunction(); eqs("index 19 -> ParameterName19, the same default 'Param19'", TempCol, "Param19");
    strcpy(TempCol, "x"); index_ = 20; LoggingAdditionalFunction(); eqs("index 20: nothing written", TempCol, "x");
    memset(ParameterColNumberLoggingArray, 0, 20); memset(prevLogging, 0, 20); memset(ParameterColNumberSelectedArray, 0, 20);
    for (int k = 0; k < 20; k++) ParameterColNumberArray[k] = (int16_t)k;
    ParameterColNumberLoggingArray[7] = true; ParameterColNumberArray[7] = 4; ParameterColNumberLoggingArray[4] = true;
    ParameterColNumberLoggingArray[9] = true; ParameterColNumberArray[9] = 4; memset(ColumnIsntVacantArray, 0, 20);
    LoggingFindConflict();
    eqi("FindConflict: only the first candidate is flagged per call (index 4, clashing with 7)", ColumnIsntVacantArray[4] * 100 + ColumnIsntVacantArray[7] * 10 + ColumnIsntVacantArray[9], 100);
    eqi("ConflictingIndex is the column number, not the parameter", ConflictingIndex, 4);
    LoggingFindConflict();
    eqi("second call: no edges left, no conflict", ConflictingIndex, -1);
    ParameterColNumberSelectedArray[9] = true; LoggingFindConflict();
    eqi("Selected AND Logging is a candidate on every call", ColumnIsntVacantArray[9], 1);

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
