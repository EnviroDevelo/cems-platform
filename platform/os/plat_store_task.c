/* plat_store_task.c - the store task: sole owner of the parameter FRAM.
 * Replaces the weak default body in plat_tasks_freertos.c. Also runs the event
 * log / data logger (the FRAM write pointer and the NOR flash have one owner). */
#include "FreeRTOS.h"
#include "task.h"
#include "plat_tasks.h"
#include "param_store.h"
#include "plat_log.h"
#include "bsp.h"

void plat_store_task_body(void) {
    for (;;) {
        param_store_poll(bsp_ms());
        plat_log_poll(bsp_ms());
        plat_checkin(PLAT_TASK_STORE);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
