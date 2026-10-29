/* plat_iobus_task.c - the I/O bus task (iobus.h): one bus cycle every PLAT_IOB_PERIOD_MS.
 * The cycle sleeps on the UART while it waits for replies, so it takes little CPU. */
#include "FreeRTOS.h"
#include "task.h"
#include "plat_tasks.h"
#include "plat_config.h"
#include "iobus.h"
#include "inst.h"
#include "bsp.h"

void plat_iobus_task_body(void) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        (void)xTaskDelayUntil(&last, pdMS_TO_TICKS(PLAT_IOB_PERIOD_MS));
        iob_cycle(bsp_ms());
        inst_cycle(bsp_ms());         /* the instruments on the I/O port: never waits (inst.h) */
        plat_checkin(PLAT_TASK_IOBUS);
    }
}
