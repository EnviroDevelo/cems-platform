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
    uint32_t fw_t = bsp_ms(), fw_scans = 0;
    fw_before_swap = flush_before_swap;
    for (;;) {
        param_store_poll(bsp_ms());
        plat_log_poll(bsp_ms());
        uint32_t now = bsp_ms();
        if (now - fw_t >= 1000u) {          /* activation, and confirming a trial image */
            uint32_t sc = plat_scan.stats.scans;
            fw_service(now, sc - fw_scans >= 50u);   /* healthy: the scan is running */
            fw_scans = sc;
            fw_t = now;
        }
        plat_checkin(PLAT_TASK_STORE);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
