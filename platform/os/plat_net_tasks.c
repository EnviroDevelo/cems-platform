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
#include "mail.h"
#include "param.h"
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

static uint8_t http_buf[PLAT_HTTP_BUF];   /* headers and form bodies; backups use the store's buffer, uploads stream */

static void http_alive(void) { plat_checkin(PLAT_TASK_HTTP); }
static void http_wait(void)  { plat_checkin(PLAT_TASK_HTTP); vTaskDelay(pdMS_TO_TICKS(10)); }

/* HTTPS on 443 (TLS; two kept connections, requests one at a time); port 80 only redirects to https://,
 * and is closed when parameter 6 (http_redirect) is off. */
static http_server_t s, r;
static void release_web(void) { http_drop_kept(&s); }

void plat_http_task_body(void) {
    static uint8_t rbuf[2048];
    const plat_app_t *app = plat_app();
    http_cfg_t cfg = { .product = app->ident.product, .fw_version = app->ident.revision, .scan = &plat_scan,
                       .scratch = http_buf, .scratch_size = sizeof http_buf, .wait = http_wait,
                       .log_chans = app->log_chans, .n_log_chans = (unsigned)app->n_log_chans };
    http_api_init(&cfg);
    mail_init(app->ident.product);
    wait_net(PLAT_TASK_HTTP);
    while (http_open(&s, PLAT_HTTPS_PORT, http_buf, sizeof http_buf) != 0) { plat_checkin(PLAT_TASK_HTTP); vTaskDelay(pdMS_TO_TICKS(1000)); }
    s.alive = http_alive;
    s.tls = true;
    r.lfd = -1;
    for (;;) {
        plat_val_t v;
        bool want80 = param_read(PARAM_ID_HTTP_REDIRECT, &v) ? v.b : true;
        if (want80 && r.lfd < 0 && http_open(&r, PLAT_HTTP_PORT, rbuf, sizeof rbuf) == 0) { r.alive = http_alive; r.redirect_only = true; }
        if (!want80 && r.lfd >= 0) http_close(&r);
        http_poll(&s, 50);
        if (r.lfd >= 0) http_poll(&r, 0);                 /* no wait: the HTTPS poll above does the waiting */
        mail_service(bsp_ms(), http_alive, release_web);   /* alarm email, between web requests (shares the TLS context) */
        plat_checkin(PLAT_TASK_HTTP);
    }
}
