/* 4H.h - Stage 4H logging (MUX): Logging, LoadTable, LogOpenedValves, LoggingColumns, LoggingFindConflict,
 * LoggingAdditionalFunction. Links with ../4A/4A_faithful.c (Num to ASCII, ConvertRealOrIntToString,
 * CheckIfVacant). ResetCertainCheckbox (42e) is supplied by the test as a recording stub.
 * The data table LoggingTable and the SD file "Reserv" are modelled here (section "platform models"). */
#ifndef STAGE4H_H
#define STAGE4H_H
#include "vm_types.h"   /* shared types and system tags (app_vmmux) */
#include "4A.h"

extern uint32_t g_dt_ms;
extern bool Ladder_Initial_Cycle;                       /* General.Ladder_Initial_Cycle */
extern rtc_date_t RTC_Date; extern rtc_time_t RTC_Time;
extern bool Is_SD_Present, Is_SD_Locked;                /* External_Storage.* */

/* ---- platform models ---- */
#define DT_ROWS 1441
extern LoggingTableStruct_t LoggingTable[DT_ROWS];
extern int8_t LoggingTable_Status;                      /* LoggingTable.Status (DTI status) */
/* The SD file DT/Reserv.udtf */
typedef struct { bool exists; int16_t rows; LoggingTableStruct_t data[DT_ROWS]; } sd_file_t;
extern sd_file_t g_reserv;
/* A4.12 / A4.18: asynchronous table and file blocks report 1 in the request scan and finish between scans.
 * sd_tick() is that "between scans" step; the test calls it before each scan. g_dt_clear_sync = true models
 * the other reading of A4.18: the table clear finishes at once and its status never shows 1. */
extern bool g_dt_clear_sync;
/* A4.19: Set DTI Column Value with start row 0 and 1 row writes row 0 only; g_setcol_whole = the whole column. */
extern bool g_setcol_whole;
void sd_tick(void);

/* ---- parameters and logging ---- */
extern bool ParameterColNumberLoggingArray[20], ParameterColNumberSelectedArray[20], ColumnIsntVacantArray[20], prevLogging[20];
extern int16_t ParameterColNumberArray[20];             /* column 0..19 = Col3..Col22 */
extern bool ColNumberNameEntered[20], ResetCertainCheckBox, ResetLastLogCheckBox, ColumnIsntVacant, ReadOccupColumnName;
extern int16_t ConflictingIndex, index_, TempColumn, OpenValveShift, ValvesStringLength, LoggingIndex;   /* index_ = tag "index" */
extern bool TempParameter, AssignColumn1, AssignColumn2, AssignColumn3;
extern bool ContactManufacturer1, ContactManufacturer2, Ackbit1;
extern char OccupiedColumnName[19], TempCol[19];
extern char ParamColName[20][19];   /* AI0ColNumberName, AI1ColNumberName, RTD0ColNumberName, RTD1ColNumberName, ParameterName4..19 */
extern LoggingTableStruct_t ReadTableStruct, InsertTableStruct;
extern float Values[20], AverageValues[20];
extern ul_timer_t EverySecond, LoginIntervalsGeneral;
extern char YearString[5], MonthString[5], DayString[5], HourString[5], MinuteString[5];
extern int16_t YearStatus, MonthStatus, DayStatus, HourStatus, MinuteStatus;
extern int8_t StatusOfSavingReserv; extern bool SaveReserv, SaveReserv1, SaveDataFailedReserv, AcknowledgeCSVReserv;
extern bool SingularInletActivate[19], PurgeInletActivate, PurgeInletActivateDI, PurgeInletActivateModbus;
extern char SingularInlet[19][11];   /* STRING(10) per the tag export; Stage 4 had [21] (I-3) */
extern char Par[20][13];                                /* Par0..Par19 */
extern uint8_t DecimPart, FractPart;                    /* 4 and 3, never written */
extern bool FlowValidationAlarm, StartPeriodsMUX; extern int16_t DwellSampleStep;
/* LoadTable */
extern int8_t CountStatus, LoadWriteTableStatus; extern int16_t Rows, Columns;
extern bool CheckCountStatus, CheckCountStatus1, CheckLoadStatus, CheckLoadStatus1, AlarmLoadingTable, AckowledgeLoadError;

void Logging(void);
void LoadTable(void);
void LogOpenedValves(void);
void LoggingColumns(void);
void LoggingFindConflict(void);
void LoggingAdditionalFunction(void);
void ResetCertainCheckbox(void);    /* 42e, stub in the test */
void treset(ul_timer_t *t);
void h4_power_up(void);            /* test support: power cycle (retained tags kept) */
#endif
