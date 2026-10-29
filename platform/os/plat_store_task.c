/* plat_store_task.c - the store task: sole owner of the parameter FRAM.
 * Replaces the weak default body in plat_tasks_freertos.c. Also runs the event
 * log / data logger (the FRAM write pointer and the NOR flash have one owner). */
#include "FreeRTOS.h"
#include "task.h"
#include "plat_tasks.h"
#include "param_store.h"
#include "plat_log.h"
#include "fw_update.h"
#include "bsp.h"
#include "plat_power.h"
#include "plat_selftest.h"
#include "plat_boardtest.h"

__attribute__((weak)) bool plat_ui_healthy(void) { return true; }

/* before a firmware swap: give the store and the log ~1.2 s to save what is pending */
static void flush_before_swap(void) {
    for (int k = 0; k < 60; ++k) {
        param_store_poll(bsp_ms());
        plat_log_poll(bsp_ms());
        plat_checkin(PLAT_TASK_STORE);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void plat_store_task_body(void) {
    uint32_t fw_t = bsp_ms(), fw_scans = 0, fail_t = 0;
    bool was_failing = false;
    fw_before_swap = flush_before_swap;
    for (;;) {
        uint32_t now = bsp_ms();
        bool failing = plat_power_failing();              /* CIP-07.5: nothing half-written at the brown-out */
        if (failing && !was_failing) fail_t = now;
        was_failing = failing;
        if (!failing) param_store_poll(now);
        if (!failing || now - fail_t < 60u) plat_log_poll(now);   /* long enough to write the power_fail event */
        plat_selftest_poll(now);                          /* CIP-11.1: the start-up checks, once */
        if (!failing) plat_boardtest_poll(now);           /* CIP-23.3: the factory board test, when asked */
        if (now - fw_t >= 1000u) {          /* activation, and confirming a trial image */
            uint32_t sc = plat_scan.stats.scans;
            fw_service(now, sc - fw_scans >= 50u && plat_ui_healthy());   /* healthy: scan running, display working */
            fw_scans = sc;
            fw_t = now;
        }
        plat_checkin(PLAT_TASK_STORE);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
