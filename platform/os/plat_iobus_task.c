/* plat_iobus_task.c - the I/O bus task (iobus.h): one bus cycle every PLAT_IOB_PERIOD_MS.
 * The cycle sleeps on the UART while it waits for replies, so it takes little CPU.
 * In the NODE role (plat_role.h) the same task is the I/O board instead: the node core and its
 * Modbus TCP and discovery sockets (plat_node.h), a pass every millisecond. */
#include "FreeRTOS.h"
#include "task.h"
#include "plat_tasks.h"
#include "plat_config.h"
#include "iobus.h"
#include "bsp.h"
#include "plat_role.h"
#include "plat_node.h"

bool plat_net_ready(void);

static void node_body(void) {
    static plat_node_t node;
    plat_node_init(&node, bsp_ms());
    for (;;) {
        if (plat_net_ready()) plat_node_poll(&node, bsp_ms(), 1);   /* waits up to 1 ms for traffic */
        else { node_poll(&node.node, bsp_ms()); vTaskDelay(1); }   /* no network yet: the core alone (all off) */
        plat_checkin(PLAT_TASK_IOBUS);
    }
}

void plat_iobus_task_body(void) {
    if (plat_role() == PLAT_ROLE_NODE) node_body();
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        (void)xTaskDelayUntil(&last, pdMS_TO_TICKS(PLAT_IOB_PERIOD_MS));
        iob_cycle(bsp_ms());
        plat_checkin(PLAT_TASK_IOBUS);
    }
}
