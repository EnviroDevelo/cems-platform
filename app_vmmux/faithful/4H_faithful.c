/* 4H_faithful.c - rung-by-rung C of the MUX logging functions (Stage 4H).
 *
 * Purpose: to pin down exactly what the ORIGINAL UniLogic code does, so it can be tested.
 * Not the code for the new firmware (see the pseudo-C in 48_Logging.md).
 * Sources: stage0/project_db/ladder/MUX_Code__{Logging,LoadTable,LogOpenedValves,LoggingColumns,
 * LoggingAdditionalFunction}.txt and st/MUX_Code__LoggingFindConflict.st. No rung is disabled.
 * Conventions as in 4A-4G: blocks pass power on (E4), compare blocks pass power only when true (E5),
 * locals start at 0 on each call (E6, A4.2), one edge memory per element updated every scan (E7),
 * parallel branches top first with a "joins wire" tail run once with OR power (11 6.1),
 * JumpToRegion is a goto (E9). Comments "R<n>" give the rung.
 */
#include <string.h>
#include "../4A/ul_lf.h"
#include "4H.h"

/* ---------------- system and platform ---------------- */
uint32_t g_dt_ms = 10;
bool Ladder_Initial_Cycle;
rtc_date_t RTC_Date = { 1, 18, 10, 2026 };
rtc_time_t RTC_Time = { 0, 5, 9 };
bool Is_SD_Present = true, Is_SD_Locked;

LoggingTableStruct_t LoggingTable[DT_ROWS];            /* RAM: not retained (99 item 6) */
int8_t LoggingTable_Status;
sd_file_t g_reserv;
bool g_dt_clear_sync;                                   /* A4.18 alternative reading */
bool g_setcol_whole;                                    /* A4.19 alternative reading */

/* ---------------- tags ---------------- */
bool ParameterColNumberLoggingArray[20], ParameterColNumberSelectedArray[20], ColumnIsntVacantArray[20], prevLogging[20];
int16_t ParameterColNumberArray[20] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 };
bool ColNumberNameEntered[20], ResetCertainCheckBox, ResetLastLogCheckBox, ColumnIsntVacant, ReadOccupColumnName;
int16_t ConflictingIndex = -1, index_, TempColumn, OpenValveShift, ValvesStringLength, LoggingIndex = 1;
bool TempParameter, AssignColumn1, AssignColumn2, AssignColumn3;
bool ContactManufacturer1, ContactManufacturer2, Ackbit1;
char OccupiedColumnName[19], TempCol[19];
char ParamColName[20][19] = { "Param1", "Param2", "Param3", "Param4", "Param5", "Param6", "Param7", "Param8",
    "Param9", "Param10", "Param11", "Param12", "Param13", "Param14", "Param15", "Param16", "Param17", "Param18",
    "Param19", "Param19" };                             /* ParameterName18 and 19 both default to "Param19" */
LoggingTableStruct_t ReadTableStruct, InsertTableStruct;
float Values[20], AverageValues[20];
ul_timer_t EverySecond = { 1000, 1000, false, false }, LoginIntervalsGeneral = { 60000, 60000, false, false };
char YearString[5], MonthString[5], DayString[5], HourString[5], MinuteString[5];
int16_t YearStatus, MonthStatus, DayStatus, HourStatus, MinuteStatus;
int8_t StatusOfSavingReserv; bool SaveReserv, SaveReserv1, SaveDataFailedReserv, AcknowledgeCSVReserv;
bool SingularInletActivate[19], PurgeInletActivate, PurgeInletActivateDI, PurgeInletActivateModbus;
char SingularInlet[19][21];
char Par[20][13];
uint8_t DecimPart = 4, FractPart = 3;
bool FlowValidationAlarm, StartPeriodsMUX; int16_t DwellSampleStep;
int8_t CountStatus, LoadWriteTableStatus; int16_t Rows, Columns;
bool CheckCountStatus, CheckCountStatus1, CheckLoadStatus, CheckLoadStatus1, AlarmLoadingTable, AckowledgeLoadError;

static const char DATETIMEColumn[] = "DATE&TIME";       /* constant tags (power-up values, never written) */
static const char ValvesNames[] = "Active Valves";

/* ---------------- element models ---------------- */
static void ton(ul_timer_t *t, bool in) {                                             /* TimerOnDelay, A4.10 */
    if (!in) { t->Current = t->Preset; t->Out = false; t->On = false; return; }
    if (!t->On) { t->Current = t->Preset; t->On = true; }
    if (!t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
void treset(ul_timer_t *t) { t->Current = t->Preset; t->Out = false; t->On = false; }
static bool pcont(bool *m, bool b) { bool e = b && !*m; *m = b; return e; }        /* E7 */
static bool in_range(int v, int lo, int hi) { return v >= lo && v <= hi; }         /* 11 6.7 */
static bool save_error(int s) { return s != -37 && in_range(s, -38, -1); }       /* the NotEqual + InRange pair */

/* struct member ColN (N = 3..22) by column number 0..19, DateTime = -2, OpenedValves = -1 */
static char *col_of(LoggingTableStruct_t *r, int c) {
    char *const m[22] = { r->DateTime, r->OpenedValves, r->Col3, r->Col4, r->Col5, r->Col6, r->Col7, r->Col8,
        r->Col9, r->Col10, r->Col11, r->Col12, r->Col13, r->Col14, r->Col15, r->Col16, r->Col17, r->Col18,
        r->Col19, r->Col20, r->Col21, r->Col22 };
    return m[c + 2];
}
static size_t cap_of(int c) { return c == -1 ? 220 : 18; }

/* ---------------- data table and SD models (platform) ----------------
 * 11d sections 1-7. Queue of asynchronous jobs: the request sets the status to 1 (A4.12);
 * sd_tick() ("between scans") finishes every queued job in order. */
typedef enum { J_CLEAR, J_COUNT, J_LOAD, J_STORE } job_kind_t;
typedef struct { job_kind_t k; int16_t n; } job_t;
static job_t q_[16]; static int nq_;
static void enqueue(job_kind_t k, int16_t n) { if (nq_ < 16) { q_[nq_].k = k; q_[nq_].n = n; nq_++; } }

void sd_tick(void) {
    for (int j = 0; j < nq_; j++) {
        switch (q_[j].k) {
        case J_CLEAR: memset(LoggingTable, 0, sizeof LoggingTable); LoggingTable_Status = 0; break;
        case J_COUNT:                                    /* UdtfCount: no help page (11d s5), modelled */
            if (!Is_SD_Present || !g_reserv.exists) CountStatus = -5;
            else { Rows = g_reserv.rows; Columns = 22; CountStatus = 0; }
            break;
        case J_LOAD:                                     /* SD2DT row 0, file row 0, n rows */
            if (!Is_SD_Present || !g_reserv.exists) LoadWriteTableStatus = -5;
            else if (q_[j].n > DT_ROWS || q_[j].n > g_reserv.rows) LoadWriteTableStatus = -33;   /* ASSUMED */
            else if (q_[j].n <= 0) LoadWriteTableStatus = -31;                                     /* ASSUMED */
            else { memcpy(LoggingTable, g_reserv.data, sizeof(LoggingTableStruct_t) * (size_t)q_[j].n); LoadWriteTableStatus = 0; }
            break;
        case J_STORE:                                    /* DT2SD row 0, n rows, overwrite, .udtf */
            if (!Is_SD_Present) StatusOfSavingReserv = -6;                                         /* ASSUMED */
            else if (q_[j].n > DT_ROWS) StatusOfSavingReserv = -33;                                /* documented code */
            else { memcpy(g_reserv.data, LoggingTable, sizeof(LoggingTableStruct_t) * (size_t)q_[j].n);
                   g_reserv.rows = q_[j].n; g_reserv.exists = true; StatusOfSavingReserv = 0; }
            break;
        }
    }
    nq_ = 0;
}
/* LF: Clear DTI. No status pin; the project waits for Status 1 then 0, so the default is
 * asynchronous (ASSUMPTION A4.18). g_dt_clear_sync: done at once, Status stays as it was. */
static void dt_clear(void) {
    if (g_dt_clear_sync) { memset(LoggingTable, 0, sizeof LoggingTable); return; }
    LoggingTable_Status = 1; enqueue(J_CLEAR, 0);
}
/* LF: Set DTI Column Value, start row 0, 1 row. Default: row 0 only (ASSUMPTION A4.19, 11d s1 reading b).
 * g_setcol_whole: start row 0 means the whole column (reading a). */
static void dt_set_column(int c, const char *v) {
    int n = g_setcol_whole ? DT_ROWS : 1;
    for (int r = 0; r < n; r++) lf_store_string(v, col_of(&LoggingTable[r], c), cap_of(c));
}
static void dt_read_row(int r, LoggingTableStruct_t *dst) { if (in_range(r, 0, DT_ROWS - 1)) *dst = LoggingTable[r]; }
/* LF: Insert Row to DTI, async bit 0 = the scan waits, so synchronous (11d s4). Rows shift down,
 * the last row drops off. Row outside the table: no change, Status -3 (ASSUMPTION A4.20). */
static void dt_insert(const LoggingTableStruct_t *src, int row) {
    if (!in_range(row, 0, DT_ROWS - 1)) { LoggingTable_Status = -3; return; }
    memmove(&LoggingTable[row + 1], &LoggingTable[row], sizeof(LoggingTableStruct_t) * (size_t)(DT_ROWS - 1 - row));
    LoggingTable[row] = *src; LoggingTable_Status = 0;
}
static void udtf_count(void) { CountStatus = 1; enqueue(J_COUNT, 0); }
static void sd2dt(int16_t n) { LoadWriteTableStatus = 1; enqueue(J_LOAD, n); }
static void dt2sd(int16_t n) { StatusOfSavingReserv = 1; enqueue(J_STORE, n); }

/* ======================================================================
 * LoggingAdditionalFunction: index 0..19 -> TempCol = that parameter's name
 * ====================================================================== */
void LoggingAdditionalFunction(void)
{
    /* R1: 20 branches Equal(index, k) -> StoreString(name k, TempCol), joined to an EndPointElement */
    if (in_range(index_, 0, 19)) lf_store_string(ParamColName[index_], TempCol, 18);
}

/* ======================================================================
 * LoggingFindConflict (ST, transliterated line by line)
 * ====================================================================== */
void LoggingFindConflict(void)
{
    bool anyConflict, edgeLogging, isCandidate, selectTrigger;
    int16_t i, j, mainCol, tempNewIdx, tempOldIdx;
    ConflictingIndex = -1; anyConflict = false; tempNewIdx = -1; tempOldIdx = -1;
    for (i = 0; i <= 19; i++) {
        edgeLogging = !prevLogging[i] && ParameterColNumberLoggingArray[i] && !ResetCertainCheckBox;  /* open item 1 */
        selectTrigger = ParameterColNumberSelectedArray[i] && ParameterColNumberLoggingArray[i];
        isCandidate = edgeLogging || selectTrigger;
        if (isCandidate && !anyConflict) {
            mainCol = ParameterColNumberArray[i];
            for (j = 0; j <= 19; j++) {
                if (j != i && ParameterColNumberLoggingArray[j] && ParameterColNumberArray[j] == mainCol) {
                    anyConflict = true; tempNewIdx = i; tempOldIdx = ParameterColNumberArray[j];
                    break;
                }
            }
        }
    }
    for (i = 0; i <= 19; i++) prevLogging[i] = ParameterColNumberLoggingArray[i];
    if (anyConflict) { ColumnIsntVacantArray[tempNewIdx] = true; ConflictingIndex = tempOldIdx; }
}

/* ======================================================================
 * LogOpenedValves: on the logging tick, append "name-" for each open inlet, then "PurgeInlet"
 * ====================================================================== */
void LogOpenedValves(void)
{
    char *ov = InsertTableStruct.OpenedValves;
    if (LoginIntervalsGeneral.Out) {
        /* R1-R5: inlets 0..18 in order (R1 0-3, R2 4-7, R3 8-11, R4 12-15, R5 16-18); a script checked
         * that every step is the same. A closed inlet bypasses its step (NC contact joins the next). */
        for (int k = 0; k <= 18; k++) {
            if (!SingularInletActivate[k]) continue;
            ValvesStringLength = (int16_t)lf_strlen(SingularInlet[k]);
            if (ValvesStringLength != 0) lf_insert_to_string(ov, SingularInlet[k], OpenValveShift, ov, 220);
            /* joined tail (w8/w9): runs for a blank name too */
            OpenValveShift = (int16_t)(OpenValveShift + ValvesStringLength);
            lf_insert_to_string(ov, "-", OpenValveShift, ov, 220);
            OpenValveShift++;
        }
        /* R6: any purge bit */
        if (PurgeInletActivate || PurgeInletActivateDI || PurgeInletActivateModbus)
            lf_insert_to_string(ov, "PurgeInlet", OpenValveShift, ov, 220);
    }
}

/* ======================================================================
 * LoggingColumns: on the logging tick, format each logged average and place it in its column
 * ====================================================================== */
void LoggingColumns(void)
{
    int16_t i = 0, StringLength = 0;                    /* locals (E6) */
    char TempColLocal[19] = "";
    /* Region 1, R1 */
    if (LoginIntervalsGeneral.Out)
        for (int k = 0; k <= 19; k++)
            if (ParameterColNumberLoggingArray[k]) ConvertRealOrIntToString(AverageValues[k], DecimPart, FractPart, Par[k]);
Region2:
    /* Region 2, R1 */
    if (LoginIntervalsGeneral.Out && in_range(i, 0, 19)) {
        TempParameter = ParameterColNumberLoggingArray[i];
        if (TempParameter) {
            TempColumn = ParameterColNumberArray[i];
            lf_store_string(Par[i], TempColLocal, 18);  /* Equal(i, k) for k = 0..19: always one match */
            if (FlowValidationAlarm || (StartPeriodsMUX && DwellSampleStep == 0)) {   /* joined tail w11 */
                StringLength = (int16_t)lf_strlen(TempColLocal);
                lf_insert_to_string(TempColLocal, "*", StringLength, TempColLocal, 18);
            }
        }
    }
    /* Region 2, R2 */
    if (LoginIntervalsGeneral.Out) {
        if (in_range(i, 0, 19)) {
            bool tail = false;
            if (TempParameter && in_range(TempColumn, 0, 19)) {
                lf_store_string(TempColLocal, col_of(&InsertTableStruct, TempColumn), 18);
                tail = true;
            }
            if (!TempParameter) tail = true;            /* branch w50 */
            if (tail) { i++; goto Region2; }            /* Inc i, JumpToRegion Region2 */
            /* enabled parameter with a column outside 0..19: no Inc, no jump, the loop stops */
        }
        if (i > 19) AssignColumn3 = false;              /* branch w51 */
    }
}

/* ======================================================================
 * Logging (30 rungs)
 * ====================================================================== */
static bool m_r1, m_r2[4], m_r3, m_r5[20], m_r9;

/* Test support: a power cycle. RAM tags go back to their power-up values, retained tags keep
 * theirs (data dictionary "retain"), the data table and the job queue are emptied, edge memories 0. */
void h4_power_up(void)
{
    memset(LoggingTable, 0, sizeof LoggingTable); LoggingTable_Status = 0; nq_ = 0;
    m_r1 = m_r3 = m_r9 = false; memset(m_r2, 0, sizeof m_r2); memset(m_r5, 0, sizeof m_r5);
    memset(ParameterColNumberSelectedArray, 0, 20); memset(ColumnIsntVacantArray, 0, 20); memset(ColNumberNameEntered, 0, 20);
    ResetCertainCheckBox = ResetLastLogCheckBox = ColumnIsntVacant = ReadOccupColumnName = false;
    ConflictingIndex = -1; index_ = 0; TempColumn = 0; OpenValveShift = 0; ValvesStringLength = 0;
    TempParameter = AssignColumn1 = AssignColumn2 = AssignColumn3 = false;
    ContactManufacturer1 = ContactManufacturer2 = Ackbit1 = false;
    OccupiedColumnName[0] = TempCol[0] = 0; memset(&ReadTableStruct, 0, sizeof ReadTableStruct); memset(&InsertTableStruct, 0, sizeof InsertTableStruct);
    memset(Values, 0, sizeof Values); memset(Par, 0, sizeof Par);
    EverySecond.Preset = 1000; treset(&EverySecond); treset(&LoginIntervalsGeneral);
    StatusOfSavingReserv = 0; SaveReserv = SaveReserv1 = AcknowledgeCSVReserv = false;
    CountStatus = LoadWriteTableStatus = 0; Rows = Columns = 0;
    CheckCountStatus = CheckCountStatus1 = CheckLoadStatus = CheckLoadStatus1 = AckowledgeLoadError = false;
    Ladder_Initial_Cycle = true;
}

void Logging(void)
{
    int k; bool p;
    /* R1 */
    if (pcont(&m_r1, ResetCertainCheckBox)) ResetCertainCheckbox();
    /* R2: parameters 0..3 only */
    p = false;
    for (k = 0; k <= 3; k++) {
        bool e = pcont(&m_r2[k], ParameterColNumberLoggingArray[k]);
        if ((!ResetCertainCheckBox && e) || ParameterColNumberSelectedArray[k]) p = true;
    }
    if (p) dt_read_row(0, &ReadTableStruct);
    /* R3: positive transition coil */
    LoggingFindConflict();
    p = ConflictingIndex != -1;
    ReadOccupColumnName = p && !m_r3; m_r3 = p;
    /* R4 */
    if (ReadOccupColumnName) { dt_read_row(0, &ReadTableStruct); CheckIfVacant(ConflictingIndex, &ReadTableStruct, OccupiedColumnName); }
    /* R5 */
    for (k = 0; k <= 19; k++) if (pcont(&m_r5[k], ColumnIsntVacantArray[k])) ParameterColNumberLoggingArray[k] = false;
    /* R6 */
    p = false; for (k = 0; k <= 19; k++) p = p || ColumnIsntVacantArray[k];
    ColumnIsntVacant = p;
    /* R7: tail (reset ResetLastLogCheckBox) runs once after all branches */
    p = false;
    for (k = 0; k <= 19; k++) if (ColumnIsntVacantArray[k] && ResetLastLogCheckBox) { ColumnIsntVacantArray[k] = false; p = true; }
    if (p) ResetLastLogCheckBox = false;
    /* R8: Selected for 0..3, but the Logging tick itself for 4..19 */
    p = false;
    for (k = 0; k <= 19; k++) {
        bool *b = k <= 3 ? &ParameterColNumberSelectedArray[k] : &ParameterColNumberLoggingArray[k];
        if (*b) { *b = false; if (!ColumnIsntVacantArray[k]) p = true; }
    }
    if (ResetCertainCheckBox) { ResetCertainCheckBox = false; p = true; }
    if (p) AssignColumn1 = true;
    /* R9 */
    if (pcont(&m_r9, AssignColumn1)) dt_clear();
    /* R10 */
    if (AssignColumn1) {
        p = false;
        if (LoggingTable_Status == 1) { AssignColumn2 = true; p = true; }
        if (LoggingTable_Status < 0) { ContactManufacturer2 = false; p = true; }
        if (p) { AssignColumn1 = false; LoggingIndex = 1; }
    }
    /* R11 */
    if (AssignColumn2) {
        p = false;
        if (LoggingTable_Status == 0) { AssignColumn3 = true; index_ = -2; p = true; }
        if (LoggingTable_Status < 0) { ContactManufacturer2 = false; p = true; }
        if (p) AssignColumn2 = false;
    }
    /* R12 */
    if (Ackbit1) { ContactManufacturer1 = false; Ackbit1 = false; }
    /* R13 */
    p = false;
    for (k = 0; k <= 19; k++)
        if (ColNumberNameEntered[k]) { ColNumberNameEntered[k] = false; if (ParameterColNumberLoggingArray[k]) p = true; }
    if (p) { AssignColumn3 = true; index_ = -2; }
    /* R15 */
    if (AssignColumn3 && index_ < 0) {
        p = false;
        if (index_ == -2) { dt_set_column(-2, DATETIMEColumn); p = true; }
        if (index_ == -1) { dt_set_column(-1, ValvesNames); p = true; }
        if (p) index_++;
    }
    /* R16 */
    if (AssignColumn3) {
        if (in_range(index_, 0, 19)) {
            TempParameter = ParameterColNumberLoggingArray[index_];
            p = false;
            if (TempParameter) {
                TempColumn = ParameterColNumberArray[index_];
                LoggingAdditionalFunction();
                if (in_range(TempColumn, 0, 19)) { dt_set_column(TempColumn, TempCol); p = true; }
            }
            if (!TempParameter) p = true;
            if (p) index_++;
        }
        if (index_ > 19) AssignColumn3 = false;
    }
    /* R18 */
    if (Ladder_Initial_Cycle)
        for (k = 0; k <= 19; k++) if (AverageValues[k] == 0.0f) AverageValues[k] = Values[k];
    /* R19-R21 */
    ton(&EverySecond, true);
    if (EverySecond.Out)
        for (k = 0; k <= 19; k++)
            if (ParameterColNumberLoggingArray[k]) AverageValues[k] = (Values[k] + AverageValues[k]) / 2.0f;   /* Average = (A+B)/2 */
    if (EverySecond.Out) treset(&EverySecond);
    /* R22 */
    ton(&LoginIntervalsGeneral, true);
    /* R23 */
    if (LoginIntervalsGeneral.Out) {
        YearString[0] = MonthString[0] = DayString[0] = HourString[0] = MinuteString[0] = 0;
        for (k = -2; k <= 19; k++) col_of(&InsertTableStruct, k)[0] = 0;
        OpenValveShift = 0;
    }
    /* R24 */
    if (LoginIntervalsGeneral.Out) {
        Num_to_ASCII(RTC_Date.Year, 4, 0, 0, 0, YearString, &YearStatus);
        Num_to_ASCII(RTC_Date.Month, 3, 0, 2, 0, MonthString, &MonthStatus);
        Num_to_ASCII(RTC_Date.Day_In_Month, 3, 0, 2, 0, DayString, &DayStatus);
        Num_to_ASCII(RTC_Time.Hours, 3, 0, 2, 0, HourString, &HourStatus);
        Num_to_ASCII(RTC_Time.Minutes, 3, 0, 2, 0, MinuteString, &MinuteStatus);
        lf_remove_from_string(MonthString, " ", 0, 1, MonthString, 4);
        lf_remove_from_string(DayString, " ", 0, 1, DayString, 4);
        lf_remove_from_string(HourString, " ", 0, 1, HourString, 4);
        lf_remove_from_string(MinuteString, " ", 0, 1, MinuteString, 4);
        if (YearStatus == 0 && MonthStatus == 0 && DayStatus == 0 && HourStatus == 0 && MinuteStatus == 0) {
            char *d = InsertTableStruct.DateTime;
            lf_insert_to_string(d, YearString, 0, d, 18);
            lf_insert_to_string(d, "-", 4, d, 18);
            lf_insert_to_string(d, MonthString, 5, d, 18);
            lf_insert_to_string(d, "-", 7, d, 18);
            lf_insert_to_string(d, DayString, 8, d, 18);
            lf_insert_to_string(d, " ", 10, d, 18);
            lf_insert_to_string(d, HourString, 11, d, 18);
            lf_insert_to_string(d, "-", 13, d, 18);
            lf_insert_to_string(d, MinuteString, 14, d, 18);
        }
    }
    /* R25 */
    LogOpenedValves();
    LoggingColumns();
    /* R26 */
    if (LoginIntervalsGeneral.Out) {
        dt_insert(&InsertTableStruct, LoggingIndex);
        treset(&LoginIntervalsGeneral);
        LoggingIndex++;
        if (Is_SD_Present && !Is_SD_Locked) { dt2sd(LoggingIndex); SaveReserv = true; }
    }
    /* R27 */
    if (AcknowledgeCSVReserv) {
        StatusOfSavingReserv = 0; SaveDataFailedReserv = false; AcknowledgeCSVReserv = false;
        if (LoggingIndex > 0) { dt2sd(LoggingIndex); SaveReserv = true; }
    }
    /* R28 */
    if (SaveReserv) {
        p = false;
        if (StatusOfSavingReserv == 1) { SaveReserv1 = true; p = true; }
        if (save_error(StatusOfSavingReserv)) { SaveDataFailedReserv = true; p = true; }
        if (p) SaveReserv = false;
    }
    /* R29 */
    if (SaveReserv1) {
        p = false;
        if (StatusOfSavingReserv == 0) p = true;
        if (save_error(StatusOfSavingReserv)) { SaveDataFailedReserv = true; p = true; }
        if (p) SaveReserv1 = false;
    }
}

/* ======================================================================
 * LoadTable: at power-up, reload LoggingTable from the SD file "Reserv"
 * ====================================================================== */
void LoadTable(void)
{
    bool p;
    /* R1 */
    if (Ladder_Initial_Cycle && LoggingIndex != 0) { udtf_count(); CheckCountStatus = true; }
    /* R2 */
    if (CheckCountStatus) {
        p = false;
        if (CountStatus == 1 || CountStatus == 2) { CheckCountStatus1 = true; p = true; }
        if (CountStatus < 0) p = true;                  /* joins w6: gives up silently */
        if (p) CheckCountStatus = false;
    }
    /* R3: a failed count (negative) still loads */
    if (CheckCountStatus1 && (CountStatus == 0 || CountStatus < 0)) {
        sd2dt(Rows); CheckLoadStatus = true; CheckCountStatus1 = false;
    }
    /* R4 */
    if (CheckLoadStatus) {
        p = false;
        if (LoadWriteTableStatus == 1 || LoadWriteTableStatus == 2) { CheckLoadStatus1 = true; p = true; }
        if (save_error(LoadWriteTableStatus)) { AlarmLoadingTable = true; p = true; }
        if (p) CheckLoadStatus = false;
    }
    /* R5 */
    if (CheckLoadStatus1) {
        p = false;
        if (LoadWriteTableStatus == 0) p = true;
        if (save_error(LoadWriteTableStatus)) { AlarmLoadingTable = true; p = true; }
        if (p) CheckLoadStatus1 = false;
    }
    /* R6 */
    if (AckowledgeLoadError) { AlarmLoadingTable = false; AckowledgeLoadError = false; }
}
