/* 4B.h - Stage 4B configuration and sizing functions: globals they touch, and prototypes. */
#ifndef STAGE4B_H
#define STAGE4B_H
#include <stdint.h>
#include <stdbool.h>

/* Globals (types and sizes from 20_Data_Dictionary) */
extern int16_t NumberOfMUXValves;
extern bool    ConfigWith10, ConfigWith20;
extern bool    SingularInletVisib[19], SingularInletVisib1[19], SingularInletVisib2[19];
extern int16_t NumInterconnections, FirstInlets, FirstOutlets, SecondInlets, SecondOutlets;
extern bool    TubeVisibility1[39], TubeVisibility2[37];
extern int16_t DOInlet[19], DOOutlet[19], BackFlushDONumber;
extern bool    ColumnIsntVacantArray[20], ParameterColNumberLoggingArray[20];
extern int16_t ParameterColNumberArray[20];
extern int     g_out_of_range_writes;   /* test aid: ST array writes outside bounds (A4.1) */

void AdjustNumberOfValves(int16_t FirstInlets1, int16_t FirstOutlets1, int16_t SecondInlets1,
                          int16_t SecondOutlets1, int16_t ChangedParameter,
                          int16_t NumInterconnections1, int16_t NumberOfValve,
                          int16_t *AdjFirstInlets, int16_t *AdjFirstOutlets,
                          int16_t *AdjSecondInlets, int16_t *AdjSecondOutlets);
void AdjustNumberOfValvesMUX(void);
void TubeVisibility(void);
void ResetDOassigningOver10(void);
void ResetCertainCheckbox(void);
#endif
