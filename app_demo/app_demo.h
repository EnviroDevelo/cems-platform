/* app_demo.h - minimal example application on the platform.
 * Shows the contract the real VM/MUX application follows; not product logic.
 *   DI0 held on for StartDelay.Preset ms -> DO0 on      (UniLogic TimerOnDelay)
 *   StartDelay.Preset is parameter 1001: range-checked, persisted in FRAM
 *   ValveName is a retained block (a string kept across power-off)
 *   first scan sets PowerUps++ (General.Ladder_Initial_Cycle) */
#ifndef APP_DEMO_H
#define APP_DEMO_H
#include "scan_engine.h"
#include "param.h"
#include "param_store.h"
#include "modbus.h"
#include "ul_system.h"
#include "plat_alarm.h"

#define DEMO_PARAM_START_DELAY 1001u
#define DEMO_PARAM_RUN_HOURS   1002u   /* written by the logic, persisted, read-only outside */
#define DEMO_PARAM_VALVE_OUT   1003u
#define DEMO_PARAM_START_IN    1004u
#define DEMO_PARAM_VALVE_NAME  1005u
#define DEMO_PARAM_ACK         1006u
#define DEMO_PARAM_FAULT       1007u
#define DEMO_PARAM_LEVEL       1008u
#define DEMO_PARAM_LEVEL_AL_EN 1009u
#define DEMO_PARAM_LEVEL_TEXT  1010u
#define DEMO_PARAM_RUNNING     1011u
#define DEMO_PARAM_STEPS       1012u   /* a record: the demo's step list (PARAM_RECORD) */
#define DEMO_BLOCK_CYCLES      1100u
#define DEMO_ALARM_FAULT       100u
#define DEMO_ALARM_LEVEL       101u

typedef struct {               /* what UI / web / Modbus may display */
    uint32_t delay_preset, delay_current;
    bool     start_in, valve_out;
    uint32_t power_ups;
    rtc_time_t time;
} demo_snapshot_t;

extern const scan_app_t   app_demo;
struct plat_app_s;
extern const struct plat_app_s app_demo_plat;   /* plat_start(&app_demo_plat) */

extern const param_def_t  app_demo_params[];
extern const size_t       app_demo_n_params;
extern const retain_def_t app_demo_retain[];
extern const size_t       app_demo_n_retain;
extern demo_snapshot_t    demo_snapshot;   /* guarded by the scan's snapshot seqlock */
extern const mb_map_t     app_demo_modbus[];
extern char               ValveName[11];
extern uint32_t           RunSeconds;

#endif
