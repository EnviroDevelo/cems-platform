/* vm_records.c - the six sequence / schedule records (core-thread design, 20 Oct): each is one
 * parameter holding the whole C struct, written whole, checked here before the scan applies it.
 *
 *   ID 3164 Sequence1      VM interconnection 1 sequence     Sequences_t
 *   ID 3165 Sequence2      VM interconnection 2 sequence     Sequences_t
 *   ID 3166 SequenceMUX    MUX channel list (Q8 loop)        ScheduleMUX_t
 *   ID 3123 Schedule1      VM interconnection 1 calendar     Schedule_t
 *   ID 3124 Schedule2      VM interconnection 2 calendar     Schedule_t
 *   ID 3125 ScheduleMUX    MUX calendar (not used while VM_MUX_CONTINUOUS)   Schedule_t
 *
 * vm_check_record() returns 0 to accept, or non-zero with the reason in words. Rules come from
 * what the logic needs to run the record as intended (46d, 45d, 45c), against the current
 * configuration; a record the original would have stalled on or misrun is refused, never
 * silently changed:
 *   sequences  NumberOfPeriods 0..20 (0 = no sequence); rows 1..n numbered 1..n and rows past n
 *              numbered 0 (the period chains end on the first 0, S10/V-chain); VM inlet 1..inlet
 *              count and outlet 1..outlet count of its interconnection on rows 1..n, 0 after
 *              (rung 1 checks all 20 rows); MUX valve = a shown inlet (SingularInletVisib) on
 *              rows 1..n, 0 after; names printable, 12 characters; booleans 0/1.
 *   schedules  years both 0 (any) or From <= To; hours 0..23, minutes and seconds 0..59;
 *              To after From (the window is From <= now < To); booleans 0/1.
 * The field descriptions the platform's editors are drawn from are at the end (vm_records[]). */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "vm_app.h"
#include "vm_tags.h"
#include "vm_params.h"
#include "vm_records.h"

#define FAIL(...) do { snprintf(why, cap, __VA_ARGS__); return 1; } while (0)

static bool bools_ok(const bool *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)b;   /* any byte other than 0 / 1 is not a valid bool */
    for (size_t i = 0; i < n; i++) if (p[i] > 1u) return false;
    return true;
}

static bool text_ok(const char *s, size_t cap)
{
    const char *z = memchr(s, 0, cap);
    if (!z) return false;                                         /* not terminated */
    size_t n = (size_t)(z - s);
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20u || (unsigned char)s[i] > 0x7Eu) return false;
    return true;
}

static int check_numbering(const uint8_t *num, unsigned n, char *why, size_t cap)
{
    if (n > 20u) FAIL("Number of periods %u: at most 20", n);
    for (unsigned k = 0; k < 20u; k++) {
        if (k < n && num[k] != k + 1u) FAIL("Period %u is numbered %u: rows must be numbered 1..%u", k + 1u, num[k], n);
        if (k >= n && num[k] != 0u) FAIL("Row %u is past the last period: its number must be 0", k + 1u);
    }
    return 0;
}

static int check_vm_sequence(const Sequences_t *s, int16_t inlets, int16_t outlets, int tube, char *why, size_t cap)
{
    unsigned n = s->NumberOfPeriods;
    if (check_numbering(s->NumberArray, n, why, cap)) return 1;
    if (n > 0u && (inlets < 1 || outlets < 1)) FAIL("Interconnection %d has no inlets or outlets configured", tube);
    for (unsigned k = 0; k < 20u; k++) {
        if (k < n) {
            if (s->Inlets[k] < 1u || (int)s->Inlets[k] > inlets)
                FAIL("Period %u: inlet %u is not fitted (interconnection %d has %d)", k + 1u, s->Inlets[k], tube, inlets);
            if (s->Outlets[k] < 1u || (int)s->Outlets[k] > outlets)
                FAIL("Period %u: outlet %u is not fitted (interconnection %d has %d)", k + 1u, s->Outlets[k], tube, outlets);
        } else if (s->Inlets[k] != 0u || s->Outlets[k] != 0u) {
            FAIL("Row %u is past the last period: its inlet and outlet must be 0", k + 1u);
        }
        if (!text_ok(s->Name[k], sizeof s->Name[k])) FAIL("Period %u: the name must be printable text of up to 12 characters", k + 1u);
    }
    return 0;
}

static int check_mux_sequence(const ScheduleMUX_t *s, char *why, size_t cap)
{
    unsigned n = s->NumberOfPeriods;
    if (check_numbering(s->NumberArray, n, why, cap)) return 1;
    if (!bools_ok(s->FastLoopPump, 20)) FAIL("Fast-loop pump flags must be 0 or 1");
    for (unsigned k = 0; k < 20u; k++) {
        unsigned v = s->SingulValves[k];
        if (k < n) {
            if (v >= 19u || !SingularInletVisib[v])
                FAIL("Channel %u: inlet %u is not fitted (%d valves configured)", k + 1u, v + 1u, NumberOfMUXValves);
        } else if (v != 0u) {
            FAIL("Row %u is past the last channel: its inlet must be 0", k + 1u);
        }
    }
    return 0;
}

static int check_schedule(const Schedule_t *s, char *why, size_t cap)
{
    if (!bools_ok(s->Month, 12) || !bools_ok(s->Day_Of_Month, 31) || !bools_ok(s->Day_Of_Week, 7))
        FAIL("Month, day and weekday ticks must be 0 or 1");
    if (!(s->From_Year == 0 && s->To_Year == 0) && s->From_Year > s->To_Year)
        FAIL("Years: from %u is after to %u", s->From_Year, s->To_Year);
    /* [0] seconds, [1] minutes, [2] hours */
    if (s->From_Hour[2] > 23u || s->From_Hour[1] > 59u || s->From_Hour[0] > 59u) FAIL("Start time is not a valid time of day");
    if (s->To_Hour[2] > 23u || s->To_Hour[1] > 59u || s->To_Hour[0] > 59u) FAIL("End time is not a valid time of day");
    long from = s->From_Hour[2] * 3600L + s->From_Hour[1] * 60L + s->From_Hour[0];
    long to   = s->To_Hour[2] * 3600L + s->To_Hour[1] * 60L + s->To_Hour[0];
    if (to <= from) FAIL("End time must be after the start time (the window is start <= time < end)");
    return 0;
}

int vm_check_record(uint16_t id, const void *img, char *why, size_t cap)
{
    if (cap) why[0] = 0;
    switch (id) {
    case VMP_SEQUENCE1:   return check_vm_sequence((const Sequences_t *)img, FirstInlets, FirstOutlets, 1, why, cap);
    case VMP_SEQUENCE2:   return check_vm_sequence((const Sequences_t *)img, SecondInlets, SecondOutlets, 2, why, cap);
    case VMP_SEQUENCEMUX: return check_mux_sequence((const ScheduleMUX_t *)img, why, cap);
    case VMP_SCHEDULE1:
    case VMP_SCHEDULE2:
    case VMP_SCHEDULEMUX: return check_schedule((const Schedule_t *)img, why, cap);
    default: FAIL("Not a record of this application");
    }
}

/* ---------------- field descriptions for the editors ----------------
 * Written against the core thread's description of ui_record_t (20 Oct); converted to that
 * type when it lands in platform/include. Table records: one row per period, row count from
 * the record's NumberOfPeriods; form records: one field each. "max_param" = a live parameter
 * that limits the column (the editor shows only what is fitted). */
#define F(NAME, KIND, TYPE, MEMBER, MAXP, MAXV) \
    { NAME, KIND, (uint16_t)offsetof(TYPE, MEMBER), (uint16_t)sizeof(((TYPE *)0)->MEMBER), MAXP, MAXV }

static const vm_rec_field_t f_vmseq[] = {
    F("No.",      VRF_ROWNUM,  Sequences_t, NumberArray, 0, 20),
    F("Name",     VRF_TEXT,    Sequences_t, Name,        0, 12),
    F("Time",     VRF_MS_AS_S, Sequences_t, Timers,      0, 0),
    F("Inlet",    VRF_U8,      Sequences_t, Inlets,      VMP_FIRSTINLETS, 19),
    F("Outlet",   VRF_U8,      Sequences_t, Outlets,     VMP_FIRSTOUTLETS, 19),
};
static const vm_rec_field_t f_vmseq2[] = {
    F("No.",      VRF_ROWNUM,  Sequences_t, NumberArray, 0, 20),
    F("Name",     VRF_TEXT,    Sequences_t, Name,        0, 12),
    F("Time",     VRF_MS_AS_S, Sequences_t, Timers,      0, 0),
    F("Inlet",    VRF_U8,      Sequences_t, Inlets,      VMP_SECONDINLETS, 19),
    F("Outlet",   VRF_U8,      Sequences_t, Outlets,     VMP_SECONDOUTLETS, 19),
};
static const vm_rec_field_t f_muxseq[] = {
    F("No.",       VRF_ROWNUM,   ScheduleMUX_t, NumberArray,       0, 20),
    F("Inlet",     VRF_INLET0,   ScheduleMUX_t, SingulValves,      VMP_NUMBEROFMUXVALVES, 19),
    F("Dwell",     VRF_MS_AS_S,  ScheduleMUX_t, DwellTimer,        0, 0),
    F("Sample",    VRF_MS_AS_S,  ScheduleMUX_t, SampleTimer,       0, 0),
    F("Pre-pump",  VRF_BOOL,     ScheduleMUX_t, FastLoopPump,      0, 1),
    F("Pump lead", VRF_MS_AS_S,  ScheduleMUX_t, FastLoopPumpTimer, 0, 0),
};
static const vm_rec_field_t f_sched[] = {
    F("From year",    VRF_U16,    Schedule_t, From_Year,    0, 2099),
    F("To year",      VRF_U16,    Schedule_t, To_Year,      0, 2099),
    F("Months",       VRF_TICKS,  Schedule_t, Month,        0, 12),
    F("Days",         VRF_TICKS,  Schedule_t, Day_Of_Month, 0, 31),
    F("Weekdays",     VRF_TICKS,  Schedule_t, Day_Of_Week,  0, 7),   /* [0] = Sunday (UniLogic) */
    F("Start",        VRF_HMS,    Schedule_t, From_Hour,    0, 0),   /* [0] s, [1] min, [2] h */
    F("End",          VRF_HMS,    Schedule_t, To_Hour,      0, 0),
};

#define REC(ID, NAME, TYPE, KIND, FIELDS, NOFF) { ID, NAME, KIND, (uint16_t)sizeof(TYPE), NOFF, \
    FIELDS, (uint8_t)(sizeof FIELDS / sizeof FIELDS[0]) }
const vm_record_t vm_records[] = {
    REC(VMP_SEQUENCE1,   "VM sequence, interconnection 1", Sequences_t,   VRK_TABLE, f_vmseq,  (int16_t)offsetof(Sequences_t, NumberOfPeriods)),
    REC(VMP_SEQUENCE2,   "VM sequence, interconnection 2", Sequences_t,   VRK_TABLE, f_vmseq2, (int16_t)offsetof(Sequences_t, NumberOfPeriods)),
    REC(VMP_SEQUENCEMUX, "MUX channels",                   ScheduleMUX_t, VRK_TABLE, f_muxseq, (int16_t)offsetof(ScheduleMUX_t, NumberOfPeriods)),
    REC(VMP_SCHEDULE1,   "VM schedule, interconnection 1", Schedule_t,    VRK_FORM,  f_sched,  -1),
    REC(VMP_SCHEDULE2,   "VM schedule, interconnection 2", Schedule_t,    VRK_FORM,  f_sched,  -1),
    REC(VMP_SCHEDULEMUX, "MUX schedule",                   Schedule_t,    VRK_FORM,  f_sched,  -1),
};
const size_t vm_n_records = sizeof vm_records / sizeof vm_records[0];
