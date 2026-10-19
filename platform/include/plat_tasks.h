/* plat_tasks.h - start the platform: create every task and run the scheduler.
 *
 * Call plat_start() from main() after the CubeMX MX_*_Init() calls. It does not
 * return. The application passes its scan definition (scan_app_t); every other
 * service task runs a default body until its module replaces it.
 *
 * Task           Priority  Period / trigger         Monitor silence
 * ctrl           14        PLAT_CTRL_PERIOD_MS      PLAT_SILENCE_CTRL_MS
 * scan           13        PLAT_SCAN_PERIOD_MS      PLAT_SILENCE_SCAN_MS
 * supervisor     12        PLAT_SUPERVISOR_PERIOD_MS (kicks the IWDG)
 * store          10        event / 100 ms           PLAT_SILENCE_STORE_MS
 * (lwIP tcpip)   8         created by tcpip_init    (not monitored directly)
 * modbus         7         socket events            PLAT_SILENCE_MODBUS_MS
 * http           6         socket events            PLAT_SILENCE_HTTP_MS
 * ui             3         touch / redraw           PLAT_SILENCE_UI_MS
 */
#ifndef PLAT_TASKS_H
#define PLAT_TASKS_H
#include "scan_engine.h"
#include "task_monitor.h"

typedef enum {
    PLAT_TASK_CTRL = 0,
    PLAT_TASK_SCAN,
    PLAT_TASK_STORE,
    PLAT_TASK_MODBUS,
    PLAT_TASK_HTTP,
    PLAT_TASK_UI,
    PLAT_TASK_COUNT
} plat_task_id_t;

void plat_start(const scan_app_t *app);   /* never returns */

/* Shared platform objects, for the service modules. */
extern scan_engine_t  plat_scan;
extern plat_cmdq_t    plat_cmdq;          /* external writes into the scan */
extern plat_seqlock_t plat_snap_lock;     /* guards the application's display snapshot */
extern task_monitor_t plat_monitor;

/* Liveness check-in, to be called from the main loop of each service task. */
void plat_checkin(plat_task_id_t id);

/* Task bodies. Each is a weak default (loops and checks in) that the service
 * module overrides with a strong definition of the same name:
 *   plat_ctrl_tick()        - fast control loops, one call per ctrl period
 *   plat_store_task_body()  - parameter store + logger owner
 *   plat_modbus_task_body() - Modbus TCP server
 *   plat_http_task_body()   - HTTP server
 *   plat_ui_task_body()     - EVE touch UI
 * A body must call plat_checkin() at least once per its silence limit and never return. */
void plat_ctrl_tick(void);
void plat_store_task_body(void);
void plat_modbus_task_body(void);
void plat_http_task_body(void);
void plat_ui_task_body(void);

#endif
