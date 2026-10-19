/* sim_main.c - PC simulation: the demo application on the real scan engine,
 * parameter table and store, with simulated time, inputs, writes from the
 * touchscreen / web / Modbus, and a power cycle. Run: make sim */
#include <stdio.h>
#include <string.h>
#include "scan_engine.h"
#include "plat_sys.h"
#include "bsp_host.h"
#include "app_demo.h"
#include "plat_tasks.h"

static scan_engine_t s; static plat_cmdq_t q; static plat_seqlock_t lock;

static void power_up(void) {
    bsp_host_reset();
    plat_cmdq_init(&q);
    scan_init(&s, &app_demo, &scan_hw_bsp, &q, &lock, 10);
    param_init(app_demo_params, app_demo_n_params);
    param_store_init(bsp_param_nvm(), NULL, app_demo_retain, app_demo_n_retain, 1, NULL);
    param_store_boot();
    alarm_init(app_demo_plat.alarms, app_demo_plat.n_alarms, BSP_RESET_POWER_ON);
    scan_set_sys(&s, &plat_sys);
    plat_val_t v; param_read(DEMO_PARAM_START_DELAY, &v);
    printf("  power up: start_delay=%u ms, valve_name=\"%s\", run_seconds=%u, store status=0x%04x%s\n",
           v.u, ValveName, RunSeconds, param_store_status(),
           param_store_calibration_valid() ? "" : "  [CALIBRATION INVALID alarm]");
}

static void post(const char *who, uint16_t id, uint8_t src, uint8_t lvl, uint32_t val) {
    plat_cmd_t c = { .id = id, .src = src, .level = lvl, .kind = PLAT_VAL_U32, .v.u = val };
    param_err_t pre = param_check(id, c.kind, c.v, src, lvl);       /* what a server would answer at once */
    printf("  %-12s writes id %u = %u -> %s\n", who, id, val, pre == PERR_OK ? "accepted" :
           pre == PERR_MODE ? "refused (mode)" : pre == PERR_RANGE ? "refused (range)" :
           pre == PERR_ACCESS ? "refused (access)" : "refused");
    if (pre == PERR_OK) plat_cmdq_post(&q, &c);
}

static void run_ms(uint32_t from, uint32_t to) {
    static bool last_out;
    for (uint32_t t = from; t < to; t += 10) {
        scan_step(&s);
        param_store_poll(bsp_ms());           /* the store task, every scan here */
        bool out = (bsp_host.out.do_bits & 1u) != 0;
        if (out != last_out) { printf("%5u ms  DO0 %s\n", t, out ? "ON" : "off"); last_out = out; }
        bsp_host_advance_ms(10);
    }
}

int main(void) {
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);   /* brand-new unit: blank FRAM */
    printf("== first power-up\n");
    power_up();
    run_ms(0, 500);
    printf("  500 ms  DI0 on\n"); bsp_host.in.di |= 1u;
    run_ms(500, 3000);
    post("modbus", DEMO_PARAM_START_DELAY, PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR, 500);   /* local mode */
    post("touchscreen", PARAM_ID_CTRL_MODE, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, PARAM_MODE_REMOTE);
    run_ms(3000, 3010);
    post("web", DEMO_PARAM_START_DELAY, PLAT_SRC_WEB, PLAT_LVL_OPERATOR, 99);          /* below min */
    post("web", DEMO_PARAM_START_DELAY, PLAT_SRC_WEB, PLAT_LVL_OPERATOR, 500);
    post("modbus", PARAM_ID_NET_IP, PLAT_SRC_MODBUS, PLAT_LVL_FACTORY, 0x0A000001u);   /* never via Modbus */
    strcpy(ValveName, "Stack A");                                                       /* logic edits a retained string */
    printf("  3010 ms  DI0 off\n"); bsp_host.in.di &= ~1u;
    run_ms(3010, 3500);
    printf("  3500 ms  DI0 on\n"); bsp_host.in.di |= 1u;
    run_ms(3500, 6000);
    store_stats_t st = param_store_stats();
    printf("  store: %u user saves, %u factory saves, user image %u bytes\n", st.saves_usr, st.saves_fac, st.payload_usr);

    printf("\n== power cycle\n");
    power_up();
    printf("  mode after power-up: %s\n", param_ctrl_mode() == PARAM_MODE_LOCAL ? "LOCAL" : "REMOTE");
    return 0;
}
