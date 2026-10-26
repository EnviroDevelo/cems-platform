/* plat_tasks_freertos.c - FreeRTOS binding of the platform (target only).
 * All tasks are statically allocated: no heap use at start-up, and the memory
 * map shows every stack. Requires configSUPPORT_STATIC_ALLOCATION = 1. */
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include "plat_tasks.h"
#include "plat_health.h"
#include "plat_config.h"
#include "plat_diag.h"
#include "bsp.h"
#include "plat_sys.h"
#include "fw_update.h"
#include "tls.h"
#include <math.h>
#include "web_auth.h"
#include "modbus.h"
#include "iobus.h"
#include "iob_tcp.h"
#include "iob_w5500.h"
#include "plat_power.h"

scan_engine_t  plat_scan;
plat_cmdq_t    plat_cmdq;
plat_seqlock_t plat_snap_lock;
task_monitor_t plat_monitor;

static int tm_ids[PLAT_TASK_COUNT];
static const plat_app_t *the_app;
static iob_tcp_t iob_eth;                        /* the I/O boards on Ethernet (iobus task only) */
static iob_w5500_t iob_io;                       /* the I/O boards on the I/O port (iobus task only) */
const plat_app_t *plat_app(void) { return the_app; }

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
__attribute__((weak)) void plat_net_task_body(void)    { idle_body(PLAT_TASK_NET); }
__attribute__((weak)) void plat_iobus_task_body(void)  { idle_body(PLAT_TASK_IOBUS); }

/* ---------------- fixed platform tasks ---------------- */
static void ctrl_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        (void)xTaskDelayUntil(&last, pdMS_TO_TICKS(PLAT_CTRL_PERIOD_MS));
        plat_power_sample();                     /* the 24 V supply, for the power-fail shutdown */
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
    plat_io_safe_state();               /* the main board's pins, and all-off to the I/O boards */
    bsp_system_reset();
    for (;;) { }   /* IWDG catches it if the reset request fails */
}

/* heap and every task's stack margin (lwIP's and the timer task too), every 5 s */
static void health_sample(void) {
    static TaskStatus_t ts[PLAT_HEALTH_TASKS];
    UBaseType_t n = uxTaskGetSystemState(ts, PLAT_HEALTH_TASKS, NULL);
    plat_health_t h = { .valid = true, .heap_size = configTOTAL_HEAP_SIZE,
                        .heap_free = (uint32_t)xPortGetFreeHeapSize(), .heap_min = (uint32_t)xPortGetMinimumEverFreeHeapSize() };
    for (UBaseType_t i = 0; i < n && h.n_tasks < PLAT_HEALTH_TASKS; ++i) {
        plat_task_health_t *t = &h.task[h.n_tasks++];
        strncpy(t->name, ts[i].pcTaskName, sizeof t->name - 1);
        t->free_words = (uint16_t)(ts[i].usStackHighWaterMark > 0xFFFFu ? 0xFFFFu : ts[i].usStackHighWaterMark);
    }
    h.sampled_ms = bsp_ms();
    plat_health = h;
}

static void supervisor_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t n = 0;
    for (;;) {
        (void)xTaskDelayUntil(&last, pdMS_TO_TICKS(PLAT_SUPERVISOR_PERIOD_MS));
        if (n++ % (5000u / PLAT_SUPERVISOR_PERIOD_MS) == 0) health_sample();
        plat_diag_tick(bsp_ms());                /* how long this boot ran, for the reboot-loop check */
        int bad = tm_check(&plat_monitor, bsp_ms());
        if (bad == TM_ALL_OK) { bsp_wdg_kick(); continue; }
        fatal(PLAT_FAULT_TASK_SILENT, bad, tm_name(&plat_monitor, bad), 0);
    }
    /* If this task itself is starved (a higher-priority task stuck), nothing kicks
     * the IWDG and the hardware resets the unit. */
}

enum { BODY_STORE, BODY_MODBUS, BODY_HTTP, BODY_UI, BODY_NET, BODY_IOBUS };
static void (*const service_bodies[])(void) = {
    [BODY_STORE] = plat_store_task_body, [BODY_MODBUS] = plat_modbus_task_body,
    [BODY_HTTP] = plat_http_task_body,   [BODY_UI] = plat_ui_task_body,
    [BODY_NET] = plat_net_task_body,     [BODY_IOBUS] = plat_iobus_task_body,
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
DEF_TASK(net,    PLAT_STACK_NET);
DEF_TASK(iobus,  PLAT_STACK_IOBUS);

#define MAKE(fn, name, stk, prio, arg, nm) \
    do { if (xTaskCreateStatic((fn), (name), (stk), (arg), (prio), nm##_stack, &nm##_tcb) == NULL) \
             fatal(PLAT_FAULT_ASSERT, -1, name, __LINE__); } while (0)

/* HTTPS key and certificate (D11): the key is made at the first start (~0.3 s). */
static void tls_start(nvm_dev_t *fram) {
    const param_def_t *ds = param_find(PARAM_ID_SERIAL), *di = param_find(PARAM_ID_NET_IP);
    bsp_wdg_kick();
    int rc = tls_init(fram, STORE_META_ADDR + 256u, ds ? param_tag_get(ds).u : 0, di ? param_tag_get(di).u : 0);
    bsp_wdg_kick();
    if (rc != 0) plat_diag_fault(PLAT_FAULT_ASSERT, -1, "tls", (uint32_t)rc, bsp_ms());   /* web stays closed; the rest runs */
    else if (tls_key_was_created()) plat_log_event(LOG_EV_TLS_KEY, PLAT_SRC_INTERNAL, 0, 0, NAN, NAN, NULL);
}

/* Event log and data logger: before the tasks start, so the power-up event comes first. */
static void log_start(const plat_app_t *app, nvm_dev_t *fram) {
    bsp_rtc_t t;
    uint32_t unix_now = 0;
    if (bsp_rtc_read(&t) && t.year >= 2000u) unix_now = plat_unix_from_civil(t.year, t.month, t.day, t.hours, t.minutes, t.seconds);
    char note[sizeof plat_diag.fault_name + 1] = { 0 };
    if (plat_diag.last_reset_cause == BSP_RESET_IWDG || plat_diag.last_reset_cause == BSP_RESET_SOFTWARE)
        memcpy(note, plat_diag.fault_name, sizeof plat_diag.fault_name);
    plat_log_cfg_t c = { .nor = bsp_log_nor(), .fram = fram, .fram_addr = STORE_META_ADDR,
                         .chans = app->log_chans, .n_chans = (unsigned)app->n_log_chans,
                         .fw_version = app->ident.revision, .reset_cause = (uint8_t)plat_diag.last_reset_cause,
                         .reset_note = note, .unix_now = unix_now };
    (void)plat_log_init(&c, bsp_ms());     /* without flash it reports LSTAT_* and stays off */
}

void plat_start(const plat_app_t *app) {
    the_app = app;
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
    tm_ids[PLAT_TASK_NET]    = tm_register(&plat_monitor, "net",    PLAT_SILENCE_NET_MS);
    tm_ids[PLAT_TASK_IOBUS]  = tm_register(&plat_monitor, "iobus",  PLAT_SILENCE_IOBUS_MS);

    plat_cmdq_init(&plat_cmdq);
    plat_snap_lock.seq = 0;
    /* CIP-08.5: after a reboot loop, no application code at all (not even its init); outputs stay
     * off, and the network, the touchscreen and firmware update run so it can be serviced */
    static const scan_app_t safe_scan = { 0 };
    bool safe = plat_diag_reboot_loop();
    scan_init(&plat_scan, safe ? &safe_scan : app->scan, &scan_hw_bsp, &plat_cmdq, &plat_snap_lock, PLAT_SCAN_PERIOD_MS);

    /* A table error is a programming error: stop here with outputs safe, the
     * watchdog fed, and the offending ID in the fault record (debugger or next boot). */
    if (param_init(app->params, app->n_params) != PERR_OK) {
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "param table", param_table_error(), bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    if (ui_app_check(app->ui) != 0) {                          /* a screen or record names a missing ID */
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "ui table", ui_app_check(app->ui), bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    ui_app_set(app->ui);
    if (plat_cal_init(app->cal, app->n_cal) != 0) {             /* a calibrated channel names a missing or wrong coefficient */
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "cal table", (uint32_t)plat_cal_init(app->cal, app->n_cal), bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    nvm_dev_t *fram = bsp_param_nvm();
    if (!param_store_init(fram, NULL, app->retain, app->n_retain, app->layout_version, app->migrate)) {
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "store table", param_store_status(), bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    param_store_boot();
    log_start(app, fram);
    if (plat_selftest_init(app->selftests, app->n_selftests, bsp_ms()) != 0) {   /* more than PLAT_STEST_APP_MAX */
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "selftest table", (uint32_t)app->n_selftests, bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    fw_init(bsp_fw_flash(), fram, STORE_META_ADDR + 128u, app->ident.revision);
    fw_boot((uint8_t)plat_diag.last_reset_cause, bsp_ms());   /* a failed trial image rolls back here */
    fw_safe_check = app->fw_safe;
    tls_start(fram);
    if (alarm_init(app->alarms, app->n_alarms, (uint8_t)plat_diag.last_reset_cause) != 0) {
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "alarm table", 0, bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    if (plat_state_init(app->meas, app->n_meas) != 0) {        /* a measurement names a missing or non-numeric ID */
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "meas table", (uint32_t)plat_state_init(app->meas, app->n_meas), bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    plat_state_safe_mode(safe);
    param_bind(&plat_cmdq, &plat_snap_lock);
    auth_init();
    if (mb_init(app->modbus_map, app->n_modbus_map, &app->ident) != 0) {
        plat_diag_fault(PLAT_FAULT_ASSERT, -1, "modbus map", 0, bsp_ms());
        for (;;) { bsp_io_safe_state(); bsp_wdg_kick(); }
    }
    scan_set_sys(&plat_scan, &plat_sys);
    iob_init(bsp_rs485());
    iob_tcp_init(&iob_eth);                      /* the sockets open on first use, once the network is up */
    iob_init_eth(&iob_eth.port, &iob_eth.disc);
    if (bsp_w5500()) {                           /* the I/O port: the chip is set up on first use */
        uint8_t uid[12], mac[6];
        bsp_chip_uid(uid);
        iob_w5500_mac(uid, mac);
        iob_w5500_init(&iob_io, bsp_w5500(), mac);
        iob_init_ioport(&iob_io.port, &iob_io.disc);
    }

    MAKE(ctrl_task,       "ctrl",   PLAT_STACK_CTRL,       PLAT_PRIO_CTRL,       NULL, ctrl);
    MAKE(scan_task,       "scan",   PLAT_STACK_SCAN,       PLAT_PRIO_SCAN,       NULL, scan);
    MAKE(supervisor_task, "sup",    PLAT_STACK_SUPERVISOR, PLAT_PRIO_SUPERVISOR, NULL, sup);
    MAKE(service_task,    "store",  PLAT_STACK_STORE,      PLAT_PRIO_STORE,      (void *)(uintptr_t)BODY_STORE,  store);
    MAKE(service_task,    "modbus", PLAT_STACK_MODBUS,     PLAT_PRIO_MODBUS,     (void *)(uintptr_t)BODY_MODBUS, modbus);
    MAKE(service_task,    "http",   PLAT_STACK_HTTP,       PLAT_PRIO_HTTP,       (void *)(uintptr_t)BODY_HTTP,   http);
    MAKE(service_task,    "ui",     PLAT_STACK_UI,         PLAT_PRIO_UI,         (void *)(uintptr_t)BODY_UI,     ui);
    MAKE(service_task,    "net",    PLAT_STACK_NET,        PLAT_PRIO_NET,        (void *)(uintptr_t)BODY_NET,    net);
    MAKE(service_task,    "iobus",  PLAT_STACK_IOBUS,      PLAT_PRIO_IOBUS,      (void *)(uintptr_t)BODY_IOBUS,  iobus);

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
