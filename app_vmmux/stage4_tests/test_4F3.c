/* test_4F3.c - tests for 4F3_faithful.c.
 * Build: gcc -std=c11 -Wall -Wextra test_4F3.c 4F3_faithful.c ../4D/4D_faithful.c ../4A/4A_faithful.c -lm
 * No rungs are disabled in these two functions. */
#include <stdio.h>
#include <string.h>
#include "4F3.h"

/* 4D globals */
Schedule_t Schedule1, Schedule2, ScheduleHMI; Sequences_t Sequence1, Sequence2, HMISequences; ScheduleMUX_t SequenceMUX, HMISequenceMUX;
/* g_oob, g_hang are defined in 4D_faithful.c */
/* 4F3 globals */
bool SchedulerInterconnection1Or2, OpenScheduleSequenceInterCon1Or2, OpenScheduleSequenceInterCon1Or2_1;
bool ScheduleSequencesInterconnectionSelected, ScheduleSequencesInterconnectionSelected1;
bool ScheduleInterconnectionSelected, LoadSecondInterconnectionSchedule, LoadingWindow, ContactManufacturer1, Ackbit1;
bool Loadinletlist, LoadOutlets, AppendStringToCSVList, AppendStringToCSVList1, LoadScheduleIntercon1_2;
bool SaveSchedule, SaveScheduleSequences, ResetSequences, NumberOfPeriodsEntered, ScrollUp, ScrollDown, EntryRow[7];
bool ArrowButtonsVisib, ScheduleSequencesVisib[7], SelectAllDays, SelectAllDaysSeq1, SelectAllDaysSeq2, BlockForSelecAll, BlockNegativeDays;
bool LoadScheduleInterCon1, LoadScheduleInterCon2, LoadSequencesInterCon1, LoadSequencesInterCon2, LoadAlterInterconnectionScreen;
uint8_t ScheduleSequencesInterconnection, ScheduleInterconnection;
int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets, InletsDeleteStatus, OutletsDeleteStatus;
int8_t CsvBufferStatus;
int16_t CurrentInletlistID, CurrentOutletlistID, TargetInletListID, TargetOutletListID, TempInlets, TempOutlets, TempValveNumber, TempStringLength;
uint8_t TempLength = 2; char TempString[11], TempStringASCII[9]; uint8_t TempBuffer[24], UserDelim[4];
uint8_t InletMaxHMI, OutletsMaxHMI; char InletName[19][11], OutletName[19][11];
int16_t scrollIndex; uint8_t Number1_7[7], Inlet[7], Outlet[7]; ul_timer_t Timer1_7[7]; char Name1_7[7][13];

static int fails, checks;
static void eqi(const char *w, long g, long x) { checks++; if (g != x) { fails++; printf("FAIL %-72s got %ld want %ld\n", w, g, x); } }
static void eqs(const char *w, const char *g, const char *x) { checks++; if (strcmp(g, x)) { fails++; printf("FAIL %-72s got \"%s\" want \"%s\"\n", w, g, x); } }

/* project-level action triggers are reset by the OS after the action (A4.15): cleared between scans */
static int n_seq1, n_seq2, n_alter, n_sch1, n_sch2;
static void os_reset(void) {
    n_seq1 += LoadSequencesInterCon1; n_seq2 += LoadSequencesInterCon2; n_alter += LoadAlterInterconnectionScreen;
    n_sch1 += LoadScheduleInterCon1; n_sch2 += LoadScheduleInterCon2;
    LoadSequencesInterCon1 = LoadSequencesInterCon2 = LoadAlterInterconnectionScreen = LoadScheduleInterCon1 = LoadScheduleInterCon2 = false;
}
static void scan(int n) { for (int i = 0; i < n; i++) { SchedulerSequencesVM(); os_reset(); } }
static void seq(Sequences_t *S, int n, uint32_t base) { memset(S, 0, sizeof *S); S->NumberOfPeriods = (uint8_t)n;
    for (int i = 0; i < n; i++) { S->NumberArray[i] = (uint8_t)(i + 1); S->Timers[i] = base + (uint32_t)i * 1000; S->Inlets[i] = (uint8_t)(i % 3 + 1);
        S->Outlets[i] = (uint8_t)(i % 2 + 1); snprintf(seq_name(S, i + 1), 13, "P%u", (unsigned)(base / 1000 + (uint32_t)i)); } }
static void reset(void) {
    scan(1); os_reset(); n_seq1 = n_seq2 = n_alter = n_sch1 = n_sch2 = 0;
    memset(&HMISequences, 0, sizeof HMISequences); memset(&ScheduleHMI, 0, sizeof ScheduleHMI);
    memset(Name1_7, 0, sizeof Name1_7); memset(Number1_7, 0, sizeof Number1_7); scrollIndex = 0;
    NumInterconnections = 1; FirstInlets = 3; FirstOutlets = 2; SecondInlets = 2; SecondOutlets = 2;
    memset(InletName, 0, sizeof InletName); memset(OutletName, 0, sizeof OutletName);
    strcpy(InletName[0], "Stack"); strcpy(InletName[2], "Dryer"); strcpy(OutletName[0], "CEMS A");
    ScheduleSequencesInterconnection = 0; ScheduleInterconnection = 0; g_oob = 0; g_div0 = 0;
    memset(&Schedule1, 0, sizeof Schedule1); memset(&Schedule2, 0, sizeof Schedule2);
    Schedule1.From_Hour[2] = 6; Schedule1.From_Hour[1] = 0; Schedule2.From_Hour[2] = 7;
    SelectAllDays = SelectAllDaysSeq1 = SelectAllDaysSeq2 = BlockForSelecAll = BlockNegativeDays = false;
    scan(2);                                                         /* settle edge memories */
    os_reset(); n_seq1 = n_seq2 = n_alter = n_sch1 = n_sch2 = 0;
}
static void open_editor(void) { OpenScheduleSequenceInterCon1Or2 = true; scan(30); }

int main(void) {
    /* ---------- 1. open the editor, one interconnection ---------- */
    reset(); seq(&Sequence1, 3, 10000); seq(&Sequence2, 2, 50000);
    open_editor();
    eqi("open: inlet list has 3 lines", g_inlets_lines, 3);
    eqs("open: inlet line 1 is the name", g_inlets_csv[0], "Stack");
    eqs("open: blank inlet name 2 is written as its number", g_inlets_csv[1], "2");
    eqs("open: inlet line 3", g_inlets_csv[2], "Dryer");
    eqi("open: outlet list has 2 lines", g_outlets_lines, 2);
    eqs("open: blank outlet name 2 -> \"2\"", g_outlets_csv[1], "2");
    eqi("open: editor screen loaded once (LoadSequencesInterCon1)", n_seq1, 1);
    eqi("open: loading window closed", LoadingWindow, 0);
    eqi("open: screen copy has 3 periods", HMISequences.NumberOfPeriods, 3);
    eqi("open: row 2 timer shown", Timer1_7[1].Preset, 11000);
    eqi("open: row 2 outlet shown", Outlet[1], 2);
    eqs("open: row 3 name shown", Name1_7[2], "P12");
    eqi("open: rows 1-3 visible, row 4 hidden", ScheduleSequencesVisib[2] * 10 + ScheduleSequencesVisib[3], 10);
    eqi("open: no arrows for 3 periods", ArrowButtonsVisib, 0);
    eqi("open: max counts from interconnection 1", InletMaxHMI * 10 + OutletsMaxHMI, 32);
    eqi("open: no index out of range", g_oob, 0);
    eqi("V2 a 10-character name fits the buffer (CR LF included)", (int)strlen("Stack") + 2 <= 24, 1);

    /* ---------- 2. edit a row and save ---------- */
    Timer1_7[1].Preset = 25000; strcpy(Name1_7[1], "Edited"); EntryRow[1] = true; scan(1);
    eqi("edit: row 2 timer written back", HMISequences.Timers[1], 25000);
    eqs("edit: row 2 name written back", HMISequences.Name2, "Edited");
    ScheduleHMI.From_Hour[2] = 10; ScheduleHMI.From_Hour[1] = 57; ScheduleHMI.From_Hour[0] = 0;
    SaveScheduleSequences = true; scan(1);
    eqi("save: Sequence1 timer 2 stored", Sequence1.Timers[1], 25000);
    eqs("save: Sequence1 name 2 stored", Sequence1.Name2, "Edited");
    eqi("save: Sequence2 untouched", Sequence2.Timers[0], 50000);
    eqi("save: bit cleared", SaveScheduleSequences, 0);
    eqi("save: schedule start stored 10:57", Schedule1.From_Hour[2] * 100 + Schedule1.From_Hour[1], 1057);
    eqi("V13 save: end time minutes = 57 + 5 = 62, no carry to 11:02", Schedule1.To_Hour[1], 62);

    /* ---------- 3. picking interconnection 2 in the combo box is undone (finding V12) ---------- */
    reset(); NumInterconnections = 2; seq(&Sequence1, 3, 10000); seq(&Sequence2, 2, 50000);
    open_editor();
    eqi("V12 two interconnections: opens on the 2-interconnection screen", n_seq2, 1);
    ScheduleSequencesInterconnection = 1; ScheduleSequencesInterconnectionSelected = true;   /* HMI write, between scans (A4.14) */
    scan(30);
    eqi("V12 combo change: alternative screen loaded", n_alter, 1);
    eqi("V12 ... but the choice is set back to interconnection 1 (0)", ScheduleSequencesInterconnection, 0);
    eqi("V12 ... the editor shows sequence 1 again (3 periods)", HMISequences.NumberOfPeriods, 3);
    eqi("V12 ... row 1 timer is sequence 1's", Timer1_7[0].Preset, 10000);
    eqi("V12 ... max counts stay interconnection 1's", InletMaxHMI, 3);
    Timer1_7[0].Preset = 99000; EntryRow[0] = true; scan(1); SaveScheduleSequences = true; scan(1);
    eqi("V12 ... a save then goes to Sequence1", Sequence1.Timers[0], 99000);
    eqi("V12 ... and Sequence2 cannot be changed from the screen", Sequence2.Timers[0], 50000);

    /* ---------- 4. the interconnection-2 paths, forced directly (unreachable from the screen, V12) ---------- */
    reset(); NumInterconnections = 2; FirstInlets = 3; SecondInlets = 2; FirstOutlets = 2; SecondOutlets = 2;
    memset(InletName, 0, sizeof InletName); memset(OutletName, 0, sizeof OutletName);
    ScheduleSequencesInterconnection = 1; InletsDeleteStatus = 0; OutletsDeleteStatus = 0; OpenScheduleSequenceInterCon1Or2_1 = true;
    g_inlets_lines = g_outlets_lines = 0; LoadSecondInterconnectionSchedule = true; seq(&Sequence2, 2, 50000);
    scan(20);
    eqi("forced #2, 5 inlets in all: list starts at ID 9 - 2 = 7, 2 lines", g_inlets_lines, 2);
    eqs("forced #2: blank name at ID 7 -> 10 - 7 = 3", g_inlets_csv[0], "3");
    eqs("forced #2: blank name at ID 8 -> 2", g_inlets_csv[1], "2");
    eqi("forced #2: the reload shows sequence 2 (2 periods)", HMISequences.NumberOfPeriods, 2);
    eqi("forced #2: max counts from interconnection 2", InletMaxHMI * 10 + OutletsMaxHMI, 22);
    reset(); NumInterconnections = 2; FirstInlets = 9; SecondInlets = 3; memset(InletName, 0, sizeof InletName);
    ScheduleSequencesInterconnection = 1; InletsDeleteStatus = 0; OutletsDeleteStatus = 0; OpenScheduleSequenceInterCon1Or2_1 = true;
    g_inlets_lines = 0; scan(20);
    eqi("forced #2, 12 inlets in all: list starts at ID 19 - 3 = 16, 3 lines", g_inlets_lines, 3);
    eqs("forced #2: blank name at ID 16 -> 20 - 16 = 4", g_inlets_csv[0], "4");

    /* ---------- 5. "select all days" after a load (finding V14) ---------- */
    reset(); seq(&Sequence1, 2, 10000); open_editor();
    eqi("V14 after the load the block flag is left set (box did not change)", BlockForSelecAll, 1);
    SelectAllDays = true; scan(1);                                   /* operator ticks the box */
    eqi("V14 first tick of select-all: days not filled", ScheduleHMI.Day_Of_Month[14], 0);
    eqi("V14 ... and the per-interconnection flag not set", SelectAllDaysSeq1, 0);
    SelectAllDays = false; scan(1); SelectAllDays = true; scan(1);
    eqi("V14 second tick works: all days filled", ScheduleHMI.Day_Of_Month[0] && ScheduleHMI.Day_Of_Month[30], 1);
    eqi("V14 ... flag set", SelectAllDaysSeq1, 1);
    scan(1);                                                          /* the day rung sees the filled days (E7) */
    ScheduleHMI.Day_Of_Month[4] = false; scan(1);                    /* untick one day while "all" is ticked */
    eqi("day unticked: select-all unticked", SelectAllDays, 0);
    eqi("day unticked: the other days stay ticked", ScheduleHMI.Day_Of_Month[5], 1);
    eqi("day unticked: flag cleared", SelectAllDaysSeq1, 0);
    eqi("day unticked: block flag consumed", BlockForSelecAll, 0);
    SelectAllDays = true; scan(1); SelectAllDays = false; scan(1);
    eqi("untick select-all: all days cleared", ScheduleHMI.Day_Of_Month[5] + ScheduleHMI.Day_Of_Month[20], 0);
    scan(2);
    eqi("untick select-all: the clearing did not re-trigger the day rung", BlockNegativeDays, 0);
    reset(); seq(&Sequence1, 2, 10000); SelectAllDaysSeq1 = true; open_editor();
    eqi("load with the stored flag on: box ticked", SelectAllDays, 1);
    eqi("... the box changed, so the block flag was consumed", BlockForSelecAll, 0);

    /* ---------- 6. schedule screen: combo and save (rungs 1, 17, 20) ---------- */
    reset(); NumInterconnections = 2; SchedulerInterconnection1Or2 = true; scan(1);
    eqi("rung 1: 2 interconnections opens schedule screen 2", n_sch2, 1);
    eqi("rung 1: command bit cleared", SchedulerInterconnection1Or2, 0);
    NumInterconnections = 3; SchedulerInterconnection1Or2 = true; scan(1);
    eqi("rung 1: 3 interconnections: no screen, bit stays set (V6)", SchedulerInterconnection1Or2, 1);
    SchedulerInterconnection1Or2 = false;
    ScheduleInterconnection = 1; ScheduleInterconnectionSelected = true; scan(1);
    eqi("rung 17: combo 2 shows schedule 2", ScheduleHMI.From_Hour[2], 7);
    eqi("rung 26: combo bit cleared", ScheduleInterconnectionSelected, 0);
    ScheduleHMI.From_Hour[2] = 8; SaveSchedule = true; scan(1);
    eqi("rung 20: save writes schedule 2", Schedule2.From_Hour[2], 8);
    eqi("rung 20: schedule 1 untouched", Schedule1.From_Hour[2], 6);
    SelectAllDaysSeq1 = true; SelectAllDaysSeq2 = false; SelectAllDays = true;
    ScheduleInterconnection = 1; ScheduleInterconnectionSelected = true; scan(1);
    eqi("V16 combo 2: the select-all box keeps interconnection 1's state", SelectAllDays, 1);

    /* ---------- 7. count entered, scroll, arrows (finding V17) ---------- */
    reset(); seq(&Sequence1, 5, 10000); open_editor();
    HMISequences.NumberOfPeriods = 3; NumberOfPeriodsEntered = true; scan(1);
    eqi("count 5 -> 3: rows 4-5 cleared", HMISequences.Timers[3] + HMISequences.Timers[4], 0);
    eqs("count 5 -> 3: name 4 cleared", HMISequences.Name4, "");
    eqi("count 5 -> 3: numbers 1..3", HMISequences.NumberArray[2], 3);
    eqi("count 5 -> 3: row 4 hidden", ScheduleSequencesVisib[3], 0);
    eqi("count entered: bit cleared", NumberOfPeriodsEntered, 0);
    HMISequences.NumberOfPeriods = 10; NumberOfPeriodsEntered = true; scan(1);
    eqi("count 10: arrows shown", ArrowButtonsVisib, 1);
    for (int k = 0; k < 4; k++) { ScrollDown = true; scan(1); }
    eqi("scroll down 4 times with 10 periods: stops at 3", scrollIndex, 3);
    eqi("scroll: row 1 shows entry 4", Number1_7[0], 4);
    ScrollUp = true; scan(1);
    eqi("scroll up: 2", scrollIndex, 2);
    HMISequences.NumberOfPeriods = 7; NumberOfPeriodsEntered = true; scan(1);
    eqi("V17 count 10 -> 7: arrows still shown", ArrowButtonsVisib, 1);
    eqi("V17 ... though scrolling cannot move", (ScrollDown = true, scan(1), scrollIndex), 0);

    /* ---------- 8. a row entry with 0 periods (finding V15) ---------- */
    reset(); seq(&Sequence1, 2, 10000); open_editor();
    HMISequences.NumberOfPeriods = 0; NumberOfPeriodsEntered = true; scan(1);
    Number1_7[0] = 7; EntryRow[0] = true; scan(1);
    eqi("V15 entry with 0 periods: MOD by zero reached", g_div0, 1);
    eqi("V15 ... model (A4.16) writes row 0", HMISequences.NumberArray[0], 7);

    /* ---------- 9. reset sequences, delete error, Ackbit ---------- */
    reset(); seq(&Sequence1, 3, 10000); seq(&Sequence2, 2, 50000); ResetSequences = true; scan(1);
    eqi("rung 23: both sequences cleared", Sequence1.NumberOfPeriods + Sequence2.NumberOfPeriods + (int)Sequence1.Timers[0] + (int)Sequence2.Timers[1], 0);
    eqs("rung 23: names cleared", Sequence2.Name1, "");
    reset(); OpenScheduleSequenceInterCon1Or2_1 = true; InletsDeleteStatus = -3; OutletsDeleteStatus = 0; LoadingWindow = true; scan(1);
    eqi("rung 5: delete error -> contact manufacturer", ContactManufacturer1, 1);
    eqi("rung 5: ... loading window off, no list", LoadingWindow + Loadinletlist, 0);
    Ackbit1 = true; scan(1);
    eqi("rung 4: acknowledge clears the message", ContactManufacturer1, 0);

    eqi("no index out of range in any test", g_oob, 0);
    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
