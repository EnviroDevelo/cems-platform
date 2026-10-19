/* plat_tasks_freertos.c - FreeRTOS binding of the platform (target only).
 * All tasks are statically allocated: no heap use at start-up, and the memory
 * map shows every stack. Requires configSUPPORT_STATIC_ALLOCATION = 1. */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "plat_tasks.h"
#include "plat_config.h"
#include "plat_diag.h"
#include "bsp.h"
#include "plat_sys.h"

scan_engine_t  plat_scan;
plat_cmdq_t    plat_cmdq;
plat_seqlock_t plat_snap_lock;
task_monitor_t plat_monitor;

static int tm_ids[PLAT_TASK_COUNT];

void plat_checkin(plat_task_id_t id) {
    if ((unsigned)id < PLAT_TASK_COUNT) tm_checkin(&plat_monitor, tm_ids[id], bsp_ms());
}

/* ---------------- default (weak) bodies, replaced by the service modules ---------------- */
__attribute__((weak)) void plat_ctrl_tick(void) { }

static void idle_body(plat_task_id_t id) {
    for (;;) { plat_checkin(id); vTaskDelay(pdMS_TO_TICKS(100)); }
}
__attribute__((weak)) void plat_store_task_body(void)  { idle_body(PLAT_TASK_STORE); }
__attribute__((weak)) void plat_modbus_task_body(void) { idle_body(PLAT_TASK_MODBUS); }
__attribute__((weak)) void plat_http_task_body(void)   { idle_body(PLAT_TASK_HTTP); }
__attribute__((weak)) void plat_ui_task_body(void)     { idle_body(PLAT_TASK_UI); }

/* ---------------- fixed platform tasks ---------------- */
static void ctrl_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        (void)xTaskDelayUntil(&last, pdMS_TO_TICKS(PLAT_CTRL_PERIOD_MS));
        plat_ctrl_tick();
        plat_checkin(PLAT_TASK_CTRL);
    }
}

static void scan_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        /* xTaskDelayUntil returns pdFALSE when the wake time has already passed
         * (previous scan overran); the engine counts that as a late start. */
        (void)xTaskDelayUntil(&last, pdMS_TO_TICKS(plat_scan.period_ms));
        scan_step(&plat_scan);
        plat_checkin(PLAT_TASK_SCAN);
    }
}

static void fatal(plat_fault_t f, int32_t task, const char *name, uint32_t arg) {
    plat_diag_fault(f, task, name, arg, bsp_ms());
    bsp_io_safe_state();
    bsp_system_reset();
    for (;;) { }   /* IWDG catches it if the reset request fails */
}

static void supervisor_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        (void)xTaskDelayUntil(&last, pdMS_TO_TICKS(PLAT_SUPERVISOR_PERIOD_MS));
        int bad = tm_check(&plat_monitor, bsp_ms());
        if (bad == TM_ALL_OK) { bsp_wdg_kick(); continue; }
        fatal(PLAT_FAULT_TASK_SILENT, bad, tm_name(&plat_monitor, bad), 0);
    }
    /* If this task itself is starved (a higher-priority task stuck), nothing kicks
     * the IWDG and the hardware resets the unit. */
}

enum { BODY_STORE, BODY_MODBUS, BODY_HTTP, BODY_UI };
static void (*const service_bodies[])(void) = {
    [BODY_STORE] = plat_store_task_body, [BODY_MODBUS] = plat_modbus_task_body,
    [BODY_HTTP] = plat_http_task_body,   [BODY_UI] = plat_ui_task_body,
};

static void service_task(void *arg) {
    service_bodies[(uintptr_t)arg]();           /* must not return */
    fatal(PLAT_FAULT_ASSERT, -1, "body returned", (uint32_t)(uintptr_t)arg);
}

/* ---------------- task table ---------------- */
#define DEF_TASK(nm, stack) static StaticTask_t nm##_tcb; static StackType_t nm##_stack[stack]
DEF_TASK(ctrl,   PLAT_STACK_CTRL);
DEF_TASK(scan,   PLAT_STACK_SCAN);
DEF_TASK(sup,    PLAT_STACK_SUPERVISOR);
DEF_TASK(store,  PLAT_STACK_STORE);
DEF_TASK(modbus, PLAT_STACK_MODBUS);
DEF_TASK(http,   PLAT_STACK_HTTP);
DEF_TASK(ui,     PLAT_STACK_UI);

#define MAKE(fn, name, stk, prio, arg, nm) \
    do { if (xTaskCreateStatic((fn), (name), (stk), (arg), (prio), nm##_stack, &nm##_tcb) == NULL) \
             fatal(PLAT_FAULT_ASSERT, -1, name, __LINE__); } while (0)

void plat_start(const plat_app_t *app) {
    plat_diag_boot(bsp_reset_cause());
    bsp_init();
    bsp_io_safe_state();

    uint32_t now = bsp_ms();
    tm_init(&plat_monitor, now, PLAT_STARTUP_GRACE_MS);
    tm_ids[PLAT_TASK_CTRL]   = tm_register(&plat_monitor, "ctrl",   PLAT_SILENCE_CTRL_MS);
    tm_ids[PLAT_TASK_SCAN]   = tm_register(&plat_monitor, "scan",   PLAT_SILENCE_SCAN_MS);
    tm_ids[PLAT_TASK_STORE]  = tm_register(&plat_monitor, "store",  PLAT_SILENCE_STORE_MS);
    tm_ids[PLAT_TASK_MODBUS] = tm_register(&plat_monitor, "modbus", PLAT_SILENCE_MODBUS_MS);
    tm_ids[PLAT_TASK_HTTP]   = tm_register(&plat_monitor, "http",   PLAT_SILENCE_HTTP_MS);
    tm_ids[PLAT_TASK_UI]     = tm_register(&plat_monitor, "ui",     PLAT_SILENCE_UI_MS);

    plat_cmdq_init(&plat_cmdq);
    plat_snap_lock.seq = 0;
    scan_init(&plat_scan, app->scan, &scan_hw_bsp, &plat_cmdq, &plat_snap_lock, PLAT_SCAN_PERIOD_MS);

    /* A table error is a programming error: stop here with outputs safe, the
     * watchdog fed, and the offending ID in the fault record (debugger or next boot). */
    if (param_init(app->params, app->n_params) != PERR_OK) {
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "param table", param_table_error(), bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    if (!param_store_init(bsp_param_nvm(), NULL, app->retain, app->n_retain, app->layout_version, app->migrate)) {
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "store table", param_store_status(), bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    param_store_boot();
    scan_set_sys(&plat_scan, &plat_sys);

    MAKE(ctrl_task,       "ctrl",   PLAT_STACK_CTRL,       PLAT_PRIO_CTRL,       NULL, ctrl);
    MAKE(scan_task,       "scan",   PLAT_STACK_SCAN,       PLAT_PRIO_SCAN,       NULL, scan);
    MAKE(supervisor_task, "sup",    PLAT_STACK_SUPERVISOR, PLAT_PRIO_SUPERVISOR, NULL, sup);
    MAKE(service_task,    "store",  PLAT_STACK_STORE,      PLAT_PRIO_STORE,      (void *)(uintptr_t)BODY_STORE,  store);
    MAKE(service_task,    "modbus", PLAT_STACK_MODBUS,     PLAT_PRIO_MODBUS,     (void *)(uintptr_t)BODY_MODBUS, modbus);
    MAKE(service_task,    "http",   PLAT_STACK_HTTP,       PLAT_PRIO_HTTP,       (void *)(uintptr_t)BODY_HTTP,   http);
    MAKE(service_task,    "ui",     PLAT_STACK_UI,         PLAT_PRIO_UI,         (void *)(uintptr_t)BODY_UI,     ui);

    vTaskStartScheduler();
    fatal(PLAT_FAULT_MALLOC_FAILED, -1, "scheduler", 0);   /* only if idle/timer task creation failed */
}

/* ---------------- FreeRTOS hooks ---------------- */
void vApplicationStackOverflowHook(TaskHandle_t t, char *name) {
    (void)t;
    fatal(PLAT_FAULT_STACK_OVERFLOW, -1, name, 0);
}

void vApplicationMallocFailedHook(void) {
    fatal(PLAT_FAULT_MALLOC_FAILED, -1, "malloc", 0);
}

/* Called by configASSERT (see port/stm32h5/FreeRTOSConfig.h). */
void plat_assert_failed(const char *file, int line) {
    const char *base = file;
    for (const char *p = file; p && *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    fatal(PLAT_FAULT_ASSERT, -1, base, (uint32_t)line);
}

/* Static memory for the idle and timer tasks. Weak in case the CubeMX/X-CUBE-FREERTOS
 * generated code already provides them. */
__attribute__((weak)) void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                                          configSTACK_DEPTH_TYPE *size) {
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
    *tcb = &idle_tcb; *stack = idle_stack; *size = configMINIMAL_STACK_SIZE;
}

#if (configUSE_TIMERS == 1)
__attribute__((weak)) void vApplicationGetTimerTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                                           configSTACK_DEPTH_TYPE *size) {
    static StaticTask_t tmr_tcb;
    static StackType_t tmr_stack[configTIMER_TASK_STACK_DEPTH];
    *tcb = &tmr_tcb; *stack = tmr_stack; *size = configTIMER_TASK_STACK_DEPTH;
}
#endif
