/* plat_net_tasks.c - the Modbus TCP and web server tasks (target).
 * Replace the weak default bodies in plat_tasks_freertos.c. They wait for the
 * network (plat_net_ready, from the net task), then serve for ever, checking in
 * with the watchdog supervisor between requests and while waiting or sending. */
#include "FreeRTOS.h"
#include "task.h"
#include "plat_tasks.h"
#include "mbtcp.h"
#include "http_api.h"
#include "http_server.h"
#include "param_store.h"

bool plat_net_ready(void);

static void wait_net(plat_task_id_t id) {
    while (!plat_net_ready()) { plat_checkin(id); vTaskDelay(pdMS_TO_TICKS(100)); }
}

void plat_modbus_task_body(void) {
    static mbtcp_server_t s;
    wait_net(PLAT_TASK_MODBUS);
    while (mbtcp_open(&s, PLAT_MODBUS_PORT) != 0) { plat_checkin(PLAT_TASK_MODBUS); vTaskDelay(pdMS_TO_TICKS(1000)); }
    for (;;) {
        mbtcp_poll(&s, 100);
        plat_checkin(PLAT_TASK_MODBUS);
    }
}

static uint8_t http_buf[4096u + STORE_FAC_SLOT + STORE_USR_SLOT + 64u];   /* largest upload: a backup file */

static void http_alive(void) { plat_checkin(PLAT_TASK_HTTP); }
static void http_wait(void)  { plat_checkin(PLAT_TASK_HTTP); vTaskDelay(pdMS_TO_TICKS(10)); }

void plat_http_task_body(void) {
    static http_server_t s;
    const plat_app_t *app = plat_app();
    http_cfg_t cfg = { .product = app->ident.product, .fw_version = app->ident.revision, .scan = &plat_scan,
                       .scratch = http_buf, .scratch_size = sizeof http_buf, .wait = http_wait };
    http_api_init(&cfg);
    wait_net(PLAT_TASK_HTTP);
    while (http_open(&s, PLAT_HTTP_PORT, http_buf, sizeof http_buf) != 0) { plat_checkin(PLAT_TASK_HTTP); vTaskDelay(pdMS_TO_TICKS(1000)); }
    s.alive = http_alive;
    for (;;) {
        http_poll(&s, 100);
        plat_checkin(PLAT_TASK_HTTP);
    }
}
