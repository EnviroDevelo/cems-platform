/* 4D.h - Stage 4D data copies: types (21_Types.h.md), globals (20_Data_Dictionary) and prototypes. */
#ifndef STAGE4D_H
#define STAGE4D_H
#include "vm_types.h"   /* shared types and system tags (app_vmmux) */
#include <stdint.h>
#include <stdbool.h>




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
