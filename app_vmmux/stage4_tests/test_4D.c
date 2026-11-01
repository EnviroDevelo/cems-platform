/* test_4D.c - tests for the Stage 4D copy functions.
 * Build: gcc -std=c11 -Wall -Wextra test_4D.c 4D_faithful.c
 * Alternative reading of A4.9: add -DUL_UINT8_WRAP (the N = 0 test then reports the hang). */
#include <stdio.h>
#include <string.h>
#include "4D.h"

Schedule_t Schedule1, Schedule2, ScheduleHMI;
Sequences_t Sequence1, Sequence2, HMISequences;
ScheduleMUX_t SequenceMUX, HMISequenceMUX;

static int fails, checks;
static void eqi(const char *w, long g, long x) { checks++; if (g != x) { fails++; printf("FAIL %-58s got %ld want %ld\n", w, g, x); } }
static void eqs(const char *w, const char *g, const char *x) { checks++; if (strcmp(g, x)) { fails++; printf("FAIL %-58s got '%s' want '%s'\n", w, g, x); } }

static void fill_seq(Sequences_t *s, int base, uint8_t n) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 20; i++) { s->NumberArray[i] = base + i; s->Timers[i] = 1000u * (base + i); s->Inlets[i] = base; s->Outlets[i] = base + 1; }
    for (int k = 1; k <= 20; k++) snprintf(seq_name(s, k), 13, "N%d_%d", base, k);
    s->NumberOfPeriods = n;
}
static void fill_mux(ScheduleMUX_t *s, int base, uint8_t n) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 20; i++) { s->NumberArray[i] = base + i; s->SingulValves[i] = base; s->DwellTimer[i] = base * 10u + i;
        s->SampleTimer[i] = base * 20u + i; s->FastLoopPump[i] = (i & 1); s->FastLoopPumpTimer[i] = base * 30u + i; }
    s->NumberOfPeriods = n;
}

int main(void) {
    /* 1. Schedules: every used member copied both ways, spares left alone */
    memset(&Schedule1, 0, sizeof Schedule1); memset(&ScheduleHMI, 0xAA, sizeof ScheduleHMI);
    Schedule1.From_Year = 2026; Schedule1.To_Year = 2030; Schedule1.Month[11] = 1; Schedule1.Day_Of_Month[30] = 1;
    Schedule1.Day_Of_Week[6] = 1; Schedule1.From_Hour[2] = 7; Schedule1.To_Hour[1] = 45; Schedule1.Spare1 = 9;
    CopySchedule1_To_HMI();
    eqi("Sch1->HMI From_Year", ScheduleHMI.From_Year, 2026); eqi("Sch1->HMI To_Year", ScheduleHMI.To_Year, 2030);
    eqi("Sch1->HMI Month[11]", ScheduleHMI.Month[11], 1); eqi("Sch1->HMI Month[0]", ScheduleHMI.Month[0], 0);
    eqi("Sch1->HMI Day_Of_Month[30]", ScheduleHMI.Day_Of_Month[30], 1); eqi("Sch1->HMI Day_Of_Week[6]", ScheduleHMI.Day_Of_Week[6], 1);
    eqi("Sch1->HMI From_Hour[Hours]", ScheduleHMI.From_Hour[2], 7); eqi("Sch1->HMI To_Hour[Minutes]", ScheduleHMI.To_Hour[1], 45);
    eqi("Sch1->HMI Spare1 not copied", ScheduleHMI.Spare1, 0xAA);
    memset(&Schedule2, 0, sizeof Schedule2); ScheduleHMI.From_Year = 2027;
    CopyScheduleHMI_To_2(); eqi("HMI->Sch2 From_Year", Schedule2.From_Year, 2027); eqi("HMI->Sch2 Day_Of_Week[6]", Schedule2.Day_Of_Week[6], 1);
    memset(&ScheduleHMI, 0, sizeof ScheduleHMI); CopySchedule2_To_HMI(); eqi("Sch2->HMI From_Year", ScheduleHMI.From_Year, 2027);
    ScheduleHMI.To_Year = 2031; CopyScheduleHMI_To_1(); eqi("HMI->Sch1 To_Year", Schedule1.To_Year, 2031);

    /* 2. VM sequence load with N = 3: rows 0..2 and names 1..3 copied; the rest of the HMI buffer is stale */
    fill_seq(&Sequence1, 10, 3); fill_seq(&HMISequences, 50, 7); g_oob = 0;
    CopySequences1_To_HMI();
    eqi("Seq1->HMI count", HMISequences.NumberOfPeriods, 3);
    eqi("Seq1->HMI row 2 number", HMISequences.NumberArray[2], 12); eqi("Seq1->HMI row 2 timer", HMISequences.Timers[2], 12000);
    eqi("Seq1->HMI row 3 STALE (from before)", HMISequences.NumberArray[3], 53);
    eqs("Seq1->HMI Name3", HMISequences.Name3, "N10_3"); eqs("Seq1->HMI Name4 STALE", HMISequences.Name4, "N50_4");
    eqi("Seq1->HMI no out-of-bounds", g_oob, 0);

    /* 3. VM sequence save with N = 20: everything copied */
    fill_seq(&HMISequences, 60, 20); fill_seq(&Sequence2, 0, 0);
    CopySequenceHMI_To_2();
    eqi("HMI->Seq2 N20 row 19", Sequence2.NumberArray[19], 79); eqs("HMI->Seq2 N20 Name20", Sequence2.Name20, "N60_20");
    eqi("HMI->Seq2 N20 no out-of-bounds", g_oob, 0);

    /* 4. Finding D2: CopySequenceHMI_To_2 with N = 0 (no IF N > 0 guard) */
    fill_seq(&HMISequences, 70, 0); fill_seq(&Sequence2, 5, 4); g_oob = 0; g_hang = 0;
    CopySequenceHMI_To_2();
    eqi("HMI->Seq2 N0 count", Sequence2.NumberOfPeriods, 0);
#ifdef UL_UINT8_WRAP
    eqi("HMI->Seq2 N0 (UINT8 wrap) loop never ends", g_hang, 1);
    eqi("HMI->Seq2 N0 (UINT8 wrap) 236 indexes x 4 arrays past the end", g_oob, 236 * 4);
#else
    eqi("HMI->Seq2 N0 (A4.9) row 0 untouched", Sequence2.NumberArray[0], 5);
    eqi("HMI->Seq2 N0 (A4.9) no out-of-bounds", g_oob, 0);
#endif
    /* same N = 0 through the guarded twin: nothing copied whichever reading */
    fill_seq(&Sequence1, 5, 4); g_oob = 0; CopySequenceHMI_To_1();
    eqi("HMI->Seq1 N0 row 0 untouched", Sequence1.NumberArray[0], 5); eqi("HMI->Seq1 N0 no out-of-bounds", g_oob, 0);
    eqs("HMI->Seq1 N0 Name1 untouched", Sequence1.Name1, "N5_1");

    /* 5. Finding D3: a count above 20 (if a screen or FRAM fault allows it) runs past the arrays */
    fill_seq(&HMISequences, 80, 25); g_oob = 0; CopySequenceHMI_To_1();
    eqi("HMI->Seq1 N25 rows 20..24 out of bounds x 4 arrays", g_oob, 5 * 4);
    eqi("HMI->Seq1 N25 row 19 copied", Sequence1.NumberArray[19], 99); eqs("HMI->Seq1 N25 Name20", Sequence1.Name20, "N80_20");

    /* 6. Sequence2 -> HMI */
    fill_seq(&Sequence2, 30, 2); fill_seq(&HMISequences, 0, 0); g_oob = 0; CopySequences2_To_HMI();
    eqi("Seq2->HMI row 1", HMISequences.Outlets[1], 31); eqs("Seq2->HMI Name2", HMISequences.Name2, "N30_2");
    eqs("Seq2->HMI Name3 untouched", HMISequences.Name3, "N0_3");

    /* 7. MUX both ways */
    fill_mux(&SequenceMUX, 3, 4); fill_mux(&HMISequenceMUX, 9, 9); g_oob = 0; CopySequenceToHMI();
    eqi("MUX->HMI count", HMISequenceMUX.NumberOfPeriods, 4); eqi("MUX->HMI row 3 dwell", HMISequenceMUX.DwellTimer[3], 33);
    eqi("MUX->HMI row 3 pump", HMISequenceMUX.FastLoopPump[3], 1); eqi("MUX->HMI row 3 pump timer", HMISequenceMUX.FastLoopPumpTimer[3], 93);
    eqi("MUX->HMI row 4 STALE", HMISequenceMUX.NumberArray[4], 13);
    fill_mux(&HMISequenceMUX, 2, 20); CopySequenceFromHMI();
    eqi("HMI->MUX N20 row 19 sample", SequenceMUX.SampleTimer[19], 59); eqi("HMI->MUX valves", SequenceMUX.SingulValves[0], 2);
    fill_mux(&HMISequenceMUX, 2, 0); fill_mux(&SequenceMUX, 4, 6); CopySequenceFromHMI();
    eqi("HMI->MUX N0 count", SequenceMUX.NumberOfPeriods, 0); eqi("HMI->MUX N0 row 0 untouched", SequenceMUX.NumberArray[0], 4);
    eqi("MUX no out-of-bounds", g_oob, 0);

    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
