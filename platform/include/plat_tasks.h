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
 * iobus          11        PLAT_IOB_PERIOD_MS (RS-485 I/O boards) PLAT_SILENCE_IOBUS_MS
 * store          10        every 20 ms (param store + logger) PLAT_SILENCE_STORE_MS
 * (lwIP tcpip)   8         created by tcpip_init    (not monitored directly)
 * modbus         7         socket events            PLAT_SILENCE_MODBUS_MS
 * http           6         socket events            PLAT_SILENCE_HTTP_MS
 * net            4         link poll every 250 ms   PLAT_SILENCE_NET_MS
 * ui             3         touch / redraw           PLAT_SILENCE_UI_MS
 * (eth_rx        9         Ethernet receive, created by the Ethernet driver)
 */
#ifndef PLAT_TASKS_H
#define PLAT_TASKS_H
#include "scan_engine.h"
#include "task_monitor.h"
#include "param.h"
#include "param_store.h"
#include "modbus.h"
#include "plat_log.h"
#include "plat_alarm.h"
#include "ui_app.h"
#include "plat_state.h"

/* Everything an application hands to the platform. */
typedef struct plat_app_s {
    const scan_app_t   *scan;            /* the program (Function1 and its I/O mapping) */
    const param_def_t  *params;          /* its parameter-table entries (IDs >= 1000) */
    size_t              n_params;
    const retain_def_t *retain;          /* its retained blocks (strings, arrays, structs) */
    size_t              n_retain;
    uint16_t            layout_version;  /* bump when a stored record changes meaning */
    param_migrate_fn    migrate;         /* may be NULL */
    const mb_map_t     *modbus_map;      /* where table entries appear on Modbus (may be NULL) */
    size_t              n_modbus_map;
    mb_ident_t          ident;           /* vendor / product / revision for Modbus 43/14 and the web page */
    const plat_log_chan_t *log_chans;    /* data-log channels; the logic calls plat_log_put(i, ...) (may be NULL) */
    size_t              n_log_chans;
    const uint16_t     *ui_home_ids;     /* parameters shown as tiles on the touchscreen's home screen (up to 6) */
    size_t              n_ui_home;
    const alarm_def_t  *alarms;          /* its alarms (IDs 100..255; may be NULL); the logic calls alarm_set() */
    bool (*fw_safe)(char *why, size_t cap);   /* safe to restart for a firmware update? false + reason refuses it (NULL = always) */
    size_t              n_alarms;
    const ui_app_t     *ui;              /* its own screens and record editors (ui_app.h; may be NULL) */
    const plat_meas_def_t *meas;         /* its measured values: each carries a quality (plat_state.h; may be NULL) */
    size_t              n_meas;
} plat_app_t;

typedef enum {
    PLAT_TASK_CTRL = 0,
    PLAT_TASK_SCAN,
    PLAT_TASK_STORE,
    PLAT_TASK_MODBUS,
    PLAT_TASK_HTTP,
    PLAT_TASK_UI,
    PLAT_TASK_NET,
    PLAT_TASK_IOBUS,
    PLAT_TASK_COUNT
} plat_task_id_t;

/* Start-up order: diag record, BSP, safe outputs, app->init, parameter table,
 * load FRAM (defaults first, then stored values), tasks, scheduler. */
void plat_start(const plat_app_t *app);   /* never returns */
/* The application this build runs: every application defines it (app_demo.c,
 * app_vmmux ...), so main(), the networked simulation and the target check take
 * whichever application is linked in (request R1). */
const plat_app_t *app_select(void);
const plat_app_t *plat_app(void);          /* the application passed to plat_start */

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
 *   plat_iobus_task_body()  - RS-485 I/O bus master (plat_iobus_task.c)
 * A body must call plat_checkin() at least once per its silence limit and never return. */
void plat_ctrl_tick(void);
void plat_store_task_body(void);
void plat_modbus_task_body(void);
void plat_http_task_body(void);
void plat_ui_task_body(void);
void plat_net_task_body(void);           /* network bring-up and upkeep (port) */
void plat_iobus_task_body(void);         /* RS-485 I/O boards (iobus.h) */
bool plat_ui_healthy(void);              /* display working or not fitted (plat_ui_task.c; weak default true) */

#endif
