/* plat_power.h - the 24 V supply: measured, alarmed, and a predictable shutdown when it fails
 * (Common Instrument Platform Specification CIP-07.3, CIP-07.5; CORE_PCB.md 12).
 *
 * The ctrl task takes one reading a millisecond (bsp_supply_mv) into a 16-reading average.
 * Below PLAT_SUPPLY_FAIL_MV the supply is failing: the store task finishes the write in progress
 * and starts no new FRAM or log-flash write (the event log gets the "power_fail" event first),
 * so the brown-out finds nothing half-written. Back above PLAT_SUPPLY_RESTORE_MV for a second
 * (a dip, not a loss) and writing resumes ("power_restored"). Outside PLAT_SUPPLY_LOW_MV ..
 * PLAT_SUPPLY_HIGH_MV for 2 s raises platform alarm 20 SUPPLY. Parameter 305 holds the average
 * for the System screen and the web page (0 = not measured on this board). */
#ifndef PLAT_POWER_H
#define PLAT_POWER_H
#include <stdint.h>
#include <stdbool.h>

void     plat_power_sample(void);          /* ctrl task, every ms */
void     plat_power_scan(uint32_t now_ms); /* scan task, every scan: parameter 305, events */
bool     plat_power_failing(void);         /* any task: the store task holds its writes while true */
uint32_t plat_supply_mv(void);             /* the average, mV; 0 = not measured */
void     plat_power_reset_for_test(void);

#endif
