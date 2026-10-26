/* plat_power.c - see plat_power.h */
#include <math.h>
#include "plat_power.h"
#include "plat_config.h"
#include "plat_log.h"
#include "plat_cmdq.h"
#include "bsp.h"

#define AVG_N 16u

uint32_t plat_supply_mv_tag;                 /* parameter 305 */

static uint32_t ring[AVG_N], sum;
static unsigned head, have;
static volatile uint32_t avg_mv;
static volatile bool failing;
static uint32_t above;                       /* ms in a row above the restore level while failing */
static unsigned low_raw;                     /* raw readings in a row below the failing level */
static bool     told;                        /* the event for the current failing state was logged */

void plat_power_sample(void) {
    uint32_t mv;
    if (!bsp_supply_mv(&mv)) return;
    sum = sum - ring[head] + mv;
    ring[head] = mv;
    head = (head + 1u) % AVG_N;
    low_raw = mv < PLAT_SUPPLY_FAIL_MV ? low_raw + 1u : 0u;
    if (have < AVG_N && ++have < AVG_N) return;   /* a full average before acting on it */
    uint32_t a = sum / AVG_N;
    avg_mv = a;
    if (!failing) {
        /* two raw readings in a row, not the average: the hold-up time from 16 V is about 20 ms
         * at full load (CORE_PCB.md 5a), so every ms of detection counts */
        if (low_raw >= 2u) { failing = true; above = 0; }
    } else if (a > PLAT_SUPPLY_RESTORE_MV) {
        if (++above >= 1000u) failing = false;
    } else {
        above = 0;
    }
}

bool     plat_power_failing(void) { return failing; }
uint32_t plat_supply_mv(void)     { return avg_mv; }

void plat_power_scan(uint32_t now_ms) {
    (void)now_ms;
    plat_supply_mv_tag = avg_mv;
    bool f = failing;
    if (f != told) {
        told = f;
        plat_log_event(f ? LOG_EV_POWER_FAIL : LOG_EV_POWER_RESTORED, PLAT_SRC_INTERNAL, 0, avg_mv, NAN, NAN, NULL);
    }
}

void plat_power_reset_for_test(void) {
    for (unsigned k = 0; k < AVG_N; ++k) ring[k] = 0;
    sum = 0; head = 0; have = 0; avg_mv = 0; failing = false; above = 0; told = false; low_raw = 0;
    plat_supply_mv_tag = 0;
}
