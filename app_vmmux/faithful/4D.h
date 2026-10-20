/* 4D.h - Stage 4D data copies: types (21_Types.h.md), globals (20_Data_Dictionary) and prototypes. */
#ifndef STAGE4D_H
#define STAGE4D_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {                       /* UniLogic system type Schedule */
    uint16_t From_Year, To_Year;
    bool     Month[12];
    bool     Day_Of_Month[31];
    uint8_t  Spare1;
    bool     Day_Of_Week[7];
    uint8_t  Spare2;
    uint8_t  From_Hour[3];             /* [Seconds, Minutes, Hours] */
    uint8_t  Spare3;
    uint8_t  To_Hour[3];
    uint8_t  Spare4;
} Schedule_t;

typedef struct {                       /* project type Sequences (VM) */
    uint8_t  NumberArray[20];
    char     Name1[13], Name2[13], Name3[13], Name4[13], Name5[13], Name6[13], Name7[13],
             Name8[13], Name9[13], Name10[13], Name11[13], Name12[13], Name13[13], Name14[13],
             Name15[13], Name16[13], Name17[13], Name18[13], Name19[13], Name20[13];
    uint32_t Timers[20];
    uint8_t  Inlets[20];
    uint8_t  Outlets[20];
    uint8_t  NumberOfPeriods;
} Sequences_t;

typedef struct {                       /* project type ScheduleMUX (MUX sequence) */
    uint8_t  NumberArray[20];
    uint8_t  SingulValves[20];
    uint32_t DwellTimer[20];
    uint32_t SampleTimer[20];
    bool     FastLoopPump[20];
    uint32_t FastLoopPumpTimer[20];
    uint8_t  NumberOfPeriods;
} ScheduleMUX_t;

extern Schedule_t    Schedule1, Schedule2, ScheduleHMI;           /* Schedule1/2 retained (FRAM) */
extern Sequences_t   Sequence1, Sequence2, HMISequences;           /* Sequence1/2 retained (FRAM) */
extern ScheduleMUX_t SequenceMUX, HMISequenceMUX;                  /* SequenceMUX retained (FRAM) */

/* test aids */
extern int g_oob;      /* array accesses outside the declared bounds (A4.1) */
extern int g_hang;     /* set when a FOR loop could never end (A4.9 alternative only) */

void CopySchedule1_To_HMI(void);  void CopySchedule2_To_HMI(void);
void CopyScheduleHMI_To_1(void);  void CopyScheduleHMI_To_2(void);
void CopySequences1_To_HMI(void); void CopySequences2_To_HMI(void);
void CopySequenceHMI_To_1(void);  void CopySequenceHMI_To_2(void);
void CopySequenceToHMI(void);     void CopySequenceFromHMI(void);

char *seq_name(Sequences_t *s, int k);   /* Name1..Name20 by number, NULL outside 1..20 */
#endif
