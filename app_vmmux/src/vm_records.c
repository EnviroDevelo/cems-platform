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
 *   sequences  NumberOfPeriods 0..20 (0 = no sequence). The row numbers are derived, not
 *              entered: the check writes 1..n and 0 after (the period chains end on the first
 *              0, S10/V-chain) and zeroes the valves of rows past n, which the editor leaves as
 *              they were when rows are removed (the core thread's answer: "your check can
 *              renumber it before accepting"; the platform passes its own copy). VM inlet 1..inlet
 *              count and outlet 1..outlet count of its interconnection on rows 1..n, 0 after
 *              (rung 1 checks all 20 rows); MUX valve = a shown inlet (SingularInletVisib) on
 *              rows 1..n, 0 after; names printable, 12 characters; booleans 0/1.
 *   schedules  years both 0 (any) or From <= To; hours 0..23, minutes and seconds 0..59;
 *              To after From (the window is From <= now < To); booleans 0/1.
 * The editors are drawn from the field descriptions in vm_ui.c. */
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

/* rows 1..n numbered 1..n, rows past n numbered 0; the valves of rows past n cleared */
static int normalise(uint8_t *num, uint8_t *v1, uint8_t *v2, unsigned n, char *why, size_t cap)
{
    if (n > 20u) FAIL("Number of periods %u: at most 20", n);
    for (unsigned k = 0; k < 20u; k++) {
        num[k] = k < n ? (uint8_t)(k + 1u) : 0u;
        if (k >= n) { v1[k] = 0u; if (v2) v2[k] = 0u; }
    }
    return 0;
}

static int check_vm_sequence(Sequences_t *s, int16_t inlets, int16_t outlets, int tube, char *why, size_t cap)
{
    unsigned n = s->NumberOfPeriods;
    if (normalise(s->NumberArray, s->Inlets, s->Outlets, n, why, cap)) return 1;
    if (n > 0u && (inlets < 1 || outlets < 1)) FAIL("Interconnection %d has no inlets or outlets configured", tube);
    for (unsigned k = 0; k < 20u; k++) {
        if (k < n) {
            if (s->Inlets[k] < 1u || (int)s->Inlets[k] > inlets)
                FAIL("Period %u: inlet %u is not fitted (interconnection %d has %d)", k + 1u, s->Inlets[k], tube, inlets);
            if (s->Outlets[k] < 1u || (int)s->Outlets[k] > outlets)
                FAIL("Period %u: outlet %u is not fitted (interconnection %d has %d)", k + 1u, s->Outlets[k], tube, outlets);
        }
        if (!text_ok(s->Name[k], sizeof s->Name[k])) FAIL("Period %u: the name must be printable text of up to 12 characters", k + 1u);
    }
    return 0;
}

static int check_mux_sequence(ScheduleMUX_t *s, char *why, size_t cap)
{
    unsigned n = s->NumberOfPeriods;
    if (normalise(s->NumberArray, s->SingulValves, NULL, n, why, cap)) return 1;
    if (!bools_ok(s->FastLoopPump, 20)) FAIL("Fast-loop pump flags must be 0 or 1");
    for (unsigned k = 0; k < 20u; k++) {
        unsigned v = s->SingulValves[k];
        if (k < n) {
            if (v >= 19u || !SingularInletVisib[v])
                FAIL("Channel %u: inlet %u is not fitted (%d valves configured)", k + 1u, v + 1u, NumberOfMUXValves);
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

int vm_check_record(uint16_t id, const void *image, char *why, size_t cap)
{
    void *img = (void *)(uintptr_t)image;   /* the platform's private copy of the new image */
    if (cap) why[0] = 0;
    switch (id) {
    case VMP_SEQUENCE1:   return check_vm_sequence((Sequences_t *)img, FirstInlets, FirstOutlets, 1, why, cap);
    case VMP_SEQUENCE2:   return check_vm_sequence((Sequences_t *)img, SecondInlets, SecondOutlets, 2, why, cap);
    case VMP_SEQUENCEMUX: return check_mux_sequence((ScheduleMUX_t *)img, why, cap);
    case VMP_SCHEDULE1:
    case VMP_SCHEDULE2:
    case VMP_SCHEDULEMUX: return check_schedule((const Schedule_t *)img, why, cap);
    default: FAIL("Not a record of this application");
    }
}
/* The editors' field descriptions (ui_record_t) are in vm_ui.c. */
