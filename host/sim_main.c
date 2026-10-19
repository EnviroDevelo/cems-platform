/* sim_main.c - PC simulation: the demo application on the real scan engine,
 * with simulated time, inputs, a "web" write and a "UI" reader. Run: make sim */
#include <stdio.h>
#include "scan_engine.h"
#include "bsp_host.h"
#include "app_demo.h"

int main(void) {
    static scan_engine_t s; static plat_cmdq_t q; static plat_seqlock_t lock;
    bsp_host_reset();
    plat_cmdq_init(&q);
    scan_init(&s, &app_demo, &scan_hw_bsp, &q, &lock, 10);

    bool last_out = false;
    for (uint32_t t = 0, step = 10; t <= 8000; t += step) {
        if (t == 500)  { bsp_host.in.di |= 1u;  printf("%5u ms  DI0 on\n", t); }
        if (t == 1500) { bsp_host.in.di &= ~1u; printf("%5u ms  DI0 off (before delay expired)\n", t); }
        if (t == 2000) { bsp_host.in.di |= 1u;  printf("%5u ms  DI0 on\n", t); }
        if (t == 4500) { bsp_host.in.di &= ~1u; printf("%5u ms  DI0 off\n", t); }
        if (t == 5000) {
            plat_cmd_t c = { .id = DEMO_PARAM_START_DELAY, .src = PLAT_SRC_WEB, .kind = PLAT_VAL_U32, .v.u = 500 };
            plat_cmdq_post(&q, &c);
            printf("%5u ms  web writes StartDelay.Preset = 500\n", t);
        }
        if (t == 6000) { bsp_host.in.di |= 1u;  printf("%5u ms  DI0 on\n", t); }

        scan_step(&s);

        bool out = (bsp_host.out.do_bits & 1u) != 0;
        if (out != last_out) { printf("%5u ms  DO0 %s\n", t, out ? "ON" : "off"); last_out = out; }

        if (t % 2000 == 0) {                    /* a lower-priority reader, e.g. the UI */
            demo_snapshot_t v;
            if (plat_seq_read(&lock, &v, &demo_snapshot, sizeof v, 4))
                printf("%5u ms  [ui] preset=%u current=%u in=%d out=%d power_ups=%u\n", t,
                       v.delay_preset, v.delay_current, v.start_in, v.valve_out, v.power_ups);
        }
        step = (t == 3000) ? 30 : 10;               /* one late scan at 3 s (30 ms gap) */
        bsp_host_advance_ms(step);
    }
    scan_stats_t st = scan_stats(&s);
    printf("\nscans=%u late_starts=%u max_dt=%u ms cmds_applied=%u rejected=%u\n",
           st.scans, st.late_starts, st.max_dt_ms, st.cmds_applied, st.cmds_rejected);
    return 0;
}
