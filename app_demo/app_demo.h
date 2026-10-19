/* app_demo.h - minimal example application on the platform scan.
 * Shows the contract the real VM/MUX application follows; not product logic.
 *   DI0 held on for StartDelay.Preset ms -> DO0 on      (UniLogic TimerOnDelay)
 *   parameter 1 (via the command queue) sets StartDelay.Preset
 *   first scan sets PowerUps++ (General.Ladder_Initial_Cycle) */
#ifndef APP_DEMO_H
#define APP_DEMO_H
#include "scan_engine.h"
#include "ul_system.h"

#define DEMO_PARAM_START_DELAY 1u

typedef struct {               /* what UI / web / Modbus may display */
    uint32_t delay_preset, delay_current;
    bool     start_in, valve_out;
    uint32_t power_ups;
    rtc_time_t time;
} demo_snapshot_t;

extern const scan_app_t app_demo;
extern demo_snapshot_t demo_snapshot;   /* guarded by the scan's snapshot seqlock */

#endif
