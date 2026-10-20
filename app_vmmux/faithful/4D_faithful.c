/* 4D_faithful.c - line-by-line C of the 10 Stage 4D copy functions (ST).
 * Sources: stage0/project_db/st/{VM,MUX}_Code__Copy*.st. Line numbers as in those files.
 * Not for the new firmware: see 44_Data_Copies.md section 4. */
#include <string.h>
#include "4D.h"

int g_oob, g_hang;

char *seq_name(Sequences_t *s, int k) {
    char *t[20] = { s->Name1, s->Name2, s->Name3, s->Name4, s->Name5, s->Name6, s->Name7, s->Name8,
                    s->Name9, s->Name10, s->Name11, s->Name12, s->Name13, s->Name14, s->Name15,
                    s->Name16, s->Name17, s->Name18, s->Name19, s->Name20 };
    return (k >= 1 && k <= 20) ? t[k - 1] : NULL;
}

/* Element copy with the A4.1 rule: an index outside 0..19 reads and writes nothing. */
#define CP(dst, src, i) do { int _i = (i); if (_i >= 0 && _i < 20) (dst)[_i] = (src)[_i]; else g_oob++; } while (0)

/* Upper bound of "FOR i := 0 TO N - 1" with N a UINT8 local.
 * ASSUMPTION A4.9: the expression N - 1 is worked out in a signed type, so N = 0 gives -1
 * and the loop does not run. Build with -DUL_UINT8_WRAP for the other reading (N - 1 = 255
 * in UINT8, so with a UINT8 counter the loop can never end; the C stops after 0..255 and sets g_hang). */
static int last_index_u8(uint8_t n) {
#ifdef UL_UINT8_WRAP
    uint8_t e = (uint8_t)(n - 1u);
    if (e == 255) g_hang = 1;
    return e;
#else
    return (int)n - 1;
#endif
}

/* ---------- VM schedules (18 lines each, identical apart from the source and target) ---------- */
static void copy_schedule(Schedule_t *d, const Schedule_t *s) {
    d->From_Year = s->From_Year;                                   /* 1 */
    d->To_Year   = s->To_Year;                                     /* 2 */
    for (int i = 0; i <= 11; i++) d->Month[i] = s->Month[i];       /* 5-7 */
    for (int i = 0; i <= 30; i++) d->Day_Of_Month[i] = s->Day_Of_Month[i];  /* 8-10 */
    for (int i = 0; i <= 6; i++)  d->Day_Of_Week[i] = s->Day_Of_Week[i];    /* 11-13 */
    for (int i = 0; i <= 2; i++) { d->From_Hour[i] = s->From_Hour[i]; d->To_Hour[i] = s->To_Hour[i]; } /* 16-19 */
    /* Spare1..Spare4 are not copied (not used anywhere) */
}
void CopySchedule1_To_HMI(void) { copy_schedule(&ScheduleHMI, &Schedule1); }
void CopySchedule2_To_HMI(void) { copy_schedule(&ScheduleHMI, &Schedule2); }
void CopyScheduleHMI_To_1(void) { copy_schedule(&Schedule1, &ScheduleHMI); }
void CopyScheduleHMI_To_2(void) { copy_schedule(&Schedule2, &ScheduleHMI); }

/* ---------- VM sequences ---------- */
static void copy_names(Sequences_t *d, Sequences_t *s, uint8_t n) {
    for (int k = 1; k <= 20; k++)                                  /* "IF N >= k THEN NameK := ..." */
        if (n >= k) strcpy(seq_name(d, k), seq_name(s, k));
}
static void copy_rows(Sequences_t *d, const Sequences_t *s, int last) {
    for (int i = 0; i <= last; i++) {
        CP(d->NumberArray, s->NumberArray, i);
        CP(d->Timers, s->Timers, i);
        CP(d->Inlets, s->Inlets, i);
        CP(d->Outlets, s->Outlets, i);
    }
}
/* CopySequences1_To_HMI: lines 2-34. CopySequences2_To_HMI: the same, lines 2-33. */
void CopySequences1_To_HMI(void) {
    uint8_t n = Sequence1.NumberOfPeriods;                         /* 2 */
    HMISequences.NumberOfPeriods = Sequence1.NumberOfPeriods;      /* 3 */
    if (n > 0) copy_rows(&HMISequences, &Sequence1, last_index_u8(n));  /* 4-12 */
    copy_names(&HMISequences, &Sequence1, n);                      /* 15-34 */
}
void CopySequences2_To_HMI(void) {
    uint8_t n = Sequence2.NumberOfPeriods;
    HMISequences.NumberOfPeriods = Sequence2.NumberOfPeriods;
    if (n > 0) copy_rows(&HMISequences, &Sequence2, last_index_u8(n));
    copy_names(&HMISequences, &Sequence2, n);
}
/* CopySequenceHMI_To_1: lines 2-34, with the IF N > 0 guard (line 5). */
void CopySequenceHMI_To_1(void) {
    uint8_t n = HMISequences.NumberOfPeriods;                      /* 2 */
    Sequence1.NumberOfPeriods = HMISequences.NumberOfPeriods;      /* 4 */
    if (n > 0) copy_rows(&Sequence1, &HMISequences, last_index_u8(n)); /* 5-13 */
    copy_names(&Sequence1, &HMISequences, n);                      /* 15-34 */
}
/* CopySequenceHMI_To_2: lines 2-34. NO "IF N > 0" guard (finding D2). */
void CopySequenceHMI_To_2(void) {
    uint8_t n = HMISequences.NumberOfPeriods;                      /* 2 */
    Sequence2.NumberOfPeriods = HMISequences.NumberOfPeriods;      /* 4 */
    copy_rows(&Sequence2, &HMISequences, last_index_u8(n));        /* 7-12 */
    copy_names(&Sequence2, &HMISequences, n);                      /* 15-34 */
}

/* ---------- MUX sequence (locals i, N are INT16, so N - 1 is never a wrap) ---------- */
static void copy_mux(ScheduleMUX_t *d, const ScheduleMUX_t *s) {
    int16_t n = s->NumberOfPeriods;
    d->NumberOfPeriods = s->NumberOfPeriods;
    if (n > 0)
        for (int i = 0; i <= n - 1; i++) {
            CP(d->NumberArray, s->NumberArray, i);
            CP(d->SingulValves, s->SingulValves, i);
            CP(d->DwellTimer, s->DwellTimer, i);
            CP(d->SampleTimer, s->SampleTimer, i);
            CP(d->FastLoopPump, s->FastLoopPump, i);
            CP(d->FastLoopPumpTimer, s->FastLoopPumpTimer, i);
        }
}
void CopySequenceToHMI(void)   { copy_mux(&HMISequenceMUX, &SequenceMUX); }   /* MUX lines 2-14 */
void CopySequenceFromHMI(void) { copy_mux(&SequenceMUX, &HMISequenceMUX); }   /* MUX lines 1-13 */
