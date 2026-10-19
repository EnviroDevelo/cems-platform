/* rtos_main.c - run the platform task set under FreeRTOS (POSIX port).
 *   ./rtos_sim normal  : 3 s of normal running, then report and exit 0
 *   ./rtos_sim stuck   : the UI task stops checking in at 2 s; the supervisor
 *                        must reset within PLAT_SILENCE_UI_MS (exit code 42) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "plat_tasks.h"
#include "app_demo.h"

extern volatile uint32_t sim_kicks, sim_di;
extern plat_io_out_t sim_out;
static int mode_stuck;
static volatile uint32_t ctrl_ticks;

void plat_ctrl_tick(void) { ctrl_ticks++; }          /* overrides the weak default */

void plat_ui_task_body(void) {                       /* overrides: a UI that later hangs */
    for (;;) {
        if (!(mode_stuck && bsp_ms() > 2000)) plat_checkin(PLAT_TASK_UI);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* lowest-priority observer: drives DI0 and reports */
static void observer(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));  sim_di = 1;
    /* a "web" write via the queue after switching to remote on the "touchscreen" */
    plat_cmd_t m = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR, .kind = PLAT_VAL_U32, .v.u = 1 };
    plat_cmdq_post(&plat_cmdq, &m);
    vTaskDelay(pdMS_TO_TICKS(50));
    plat_cmd_t w = { .id = DEMO_PARAM_START_DELAY, .src = PLAT_SRC_WEB, .level = PLAT_LVL_OPERATOR, .kind = PLAT_VAL_U32, .v.u = 1500 };
    plat_cmdq_post(&plat_cmdq, &w);
    vTaskDelay(pdMS_TO_TICKS(2250));
    demo_snapshot_t v = { 0 }; bool ok = plat_seq_read(&plat_snap_lock, &v, &demo_snapshot, sizeof v, 4);
    scan_stats_t s = scan_stats(&plat_scan);
    printf("t=%u ms scans=%u late=%u max_dt=%u ctrl_ticks=%u kicks=%u DO0=%u snap_ok=%d valve=%d current=%u\n",
           (unsigned)bsp_ms(), (unsigned)s.scans, (unsigned)s.late_starts, (unsigned)s.max_dt_ms,
           (unsigned)ctrl_ticks, (unsigned)sim_kicks, (unsigned)(sim_out.do_bits & 1u), ok, v.valve_out,
           (unsigned)v.delay_current);
    plat_val_t pv; param_read(DEMO_PARAM_START_DELAY, &pv);
    store_stats_t ss = param_store_stats();
    printf("store: start_delay=%u saves_usr=%u saves_fac=%u status=0x%04x cmds_applied=%u\n",
           (unsigned)pv.u, (unsigned)ss.saves_usr, (unsigned)ss.saves_fac, (unsigned)param_store_status(),
           (unsigned)s.cmds_applied);
    if (!mode_stuck) exit(pv.u == 1500 && ss.saves_usr >= 2 ? 0 : 3);
    vTaskDelay(pdMS_TO_TICKS(10000));
    printf("ERROR: no reset after the UI task hung\n");
    exit(2);
}

int main(int argc, char **argv) {
    mode_stuck = argc > 1 && strcmp(argv[1], "stuck") == 0;
    static StaticTask_t tcb; static StackType_t stk[configMINIMAL_STACK_SIZE];
    xTaskCreateStatic(observer, "obs", configMINIMAL_STACK_SIZE, NULL, 1, stk, &tcb);
    plat_start(&app_demo_plat);
}
