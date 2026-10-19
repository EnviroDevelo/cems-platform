/* test_scan.c - scan engine contract (scan_engine.h steps 1-8). */
#include <string.h>
#include "tst.h"
#include "scan_engine.h"
#include "ul_system.h"
#include "bsp_host.h"

/* ---- a recording fake application ---- */
static struct {
    int inits, scans;
    char order[64]; int n;                  /* call order within one scan */
    bool first_seen[8];
    uint32_t dt_seen[8];
    bool di0_at_logic;
    int32_t applied_ids[40]; int napplied;
    bool reject_odd;
    bool do3;
    int publishes; uint32_t seq_during_publish;
} A;
static plat_seqlock_t lock;

static void mark(char c) { if (A.n < 63) A.order[A.n++] = c; }
static void a_init(void) { A.inits++; }
static bool di0_tag;
static void a_in(const plat_io_in_t *in) { mark('I'); di0_tag = plat_di(in, 0); }
static int  a_cmd(const plat_cmd_t *c) {
    mark('C'); A.applied_ids[A.napplied++] = c->id;
    return (A.reject_odd && (c->id & 1)) ? -1 : 0;
}
static void a_logic(void) {
    mark('L');
    if (A.scans < 8) { A.first_seen[A.scans] = Ladder_Initial_Cycle; A.dt_seen[A.scans] = g_dt_ms; }
    A.di0_at_logic = di0_tag;
    A.scans++;
}
static void a_out(plat_io_out_t *o) { mark('O'); plat_do_set(o, 3, A.do3); }
static void a_pub(void) { mark('P'); A.publishes++; A.seq_during_publish = lock.seq; }

static const scan_app_t app = { a_init, a_in, a_cmd, a_logic, a_out, a_pub };
static const scan_app_t app_min = { .logic = a_logic };

static scan_engine_t S;
static plat_cmdq_t Q;

static void setup(void) {
    memset(&A, 0, sizeof A);
    bsp_host_reset();
    plat_cmdq_init(&Q);
    lock.seq = 0;
    scan_init(&S, &app, &scan_hw_bsp, &Q, &lock, 10);
}

static void t_init_called_once(void) {
    setup();
    CHECK_EQ(A.inits, 1);
    scan_step(&S); scan_step(&S);
    CHECK_EQ(A.inits, 1);
}

static void t_first_scan_bit(void) {
    setup();
    for (int k = 0; k < 4; ++k) { scan_step(&S); bsp_host_advance_ms(10); }
    CHECK(A.first_seen[0]);
    CHECK(!A.first_seen[1]); CHECK(!A.first_seen[2]); CHECK(!A.first_seen[3]);
    CHECK(!Ladder_Initial_Cycle);          /* never left set outside the logic */
}

static void t_dt(void) {
    setup();
    scan_step(&S); bsp_host_advance_ms(10);
    scan_step(&S); bsp_host_advance_ms(37);   /* a late scan */
    scan_step(&S);
    CHECK_EQ(A.dt_seen[0], 0);
    CHECK_EQ(A.dt_seen[1], 10);
    CHECK_EQ(A.dt_seen[2], 37);
    CHECK_EQ(S.stats.max_dt_ms, 37);
    CHECK_EQ(S.stats.late_starts, 1);
}

static void t_dt_wrap(void) {
    setup();
    bsp_host.ms = 0xFFFFFFF8u;
    scan_step(&S);
    bsp_host.ms += 10;                         /* wraps through zero */
    scan_step(&S);
    CHECK_EQ(A.dt_seen[1], 10);
}

static void t_dt_total_preserved(void) {
    /* a TON-style accumulator fed by g_dt_ms must equal real elapsed time */
    setup();
    uint32_t sum = 0, steps[] = { 10, 10, 25, 9, 11, 100, 10 };
    scan_step(&S);
    for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; ++k) {
        bsp_host_advance_ms(steps[k]); scan_step(&S); sum += g_dt_ms;
    }
    CHECK_EQ(sum, 175);
}

static void t_order(void) {
    setup();
    plat_cmd_t c = { .id = 7 };
    plat_cmdq_post(&Q, &c);
    scan_step(&S);
    A.order[A.n] = 0;
    CHECK(strcmp(A.order, "ICLOP") == 0);
    if (strcmp(A.order, "ICLOP") != 0) printf("    order was %s\n", A.order);
}

static void t_rtc(void) {
    setup();
    bsp_host.rtc = (bsp_rtc_t){ 2026, 10, 18, 23, 59, 58, 7 };   /* Sunday */
    scan_step(&S);
    CHECK_EQ(RTC_Date.Year, 2026); CHECK_EQ(RTC_Date.Month, 10); CHECK_EQ(RTC_Date.Day_In_Month, 18);
    CHECK_EQ(RTC_Date.Day_Of_Week, 1);                          /* UniLogic Sunday = 1 */
    CHECK_EQ(RTC_Time.Hours, 23); CHECK_EQ(RTC_Time.Minutes, 59); CHECK_EQ(RTC_Time.Seconds, 58);
    /* invalid RTC: previous snapshot kept, counted */
    bsp_host.rtc_valid = false;
    bsp_host.rtc.seconds = 0;
    scan_step(&S);
    CHECK_EQ(RTC_Time.Seconds, 58);
    CHECK_EQ(S.stats.rtc_invalid, 1);
}

static void t_dow_table(void) {
    /* ISO Mon..Sun -> UniLogic Sun=1..Sat=7 */
    CHECK_EQ(scan_dow_iso_to_ul(1), 2);
    CHECK_EQ(scan_dow_iso_to_ul(2), 3);
    CHECK_EQ(scan_dow_iso_to_ul(5), 6);
    CHECK_EQ(scan_dow_iso_to_ul(6), 7);
    CHECK_EQ(scan_dow_iso_to_ul(7), 1);
    CHECK_EQ(scan_dow_iso_to_ul(0), 0);
    CHECK_EQ(scan_dow_iso_to_ul(8), 0);
}

static void t_inputs_frozen(void) {
    setup();
    bsp_host.in.di = 1u;
    scan_step(&S);
    CHECK(A.di0_at_logic);
    bsp_host.in.di = 0u;
    scan_step(&S);
    CHECK(!A.di0_at_logic);
}

static void t_outputs(void) {
    setup();
    A.do3 = true; scan_step(&S);
    CHECK_EQ(bsp_host.out.do_bits, 1u << 3);
    A.do3 = false; scan_step(&S);
    CHECK_EQ(bsp_host.out.do_bits, 0u);
    CHECK_EQ(bsp_host.io_writes, 2);
}

static void t_outputs_default_off(void) {
    /* an app without io_out still writes an all-off image */
    memset(&A, 0, sizeof A);
    bsp_host_reset();
    bsp_host.out.do_bits = 0xFFFFFFFFu;
    scan_init(&S, &app_min, &scan_hw_bsp, NULL, NULL, 10);
    scan_step(&S);
    CHECK_EQ(bsp_host.out.do_bits, 0u);
    CHECK_EQ(A.scans, 1);
}

static void t_cmds_bounded_and_ordered(void) {
    setup();
    for (int k = 0; k < 20; ++k) { plat_cmd_t c = { .id = (uint16_t)k }; CHECK(plat_cmdq_post(&Q, &c)); }
    scan_step(&S);
    CHECK_EQ(A.napplied, PLAT_SCAN_MAX_CMDS);
    scan_step(&S);
    CHECK_EQ(A.napplied, 20);
    int ordered = 1; for (int k = 0; k < 20; ++k) if (A.applied_ids[k] != k) ordered = 0;
    CHECK(ordered);
}

static void t_cmd_reject_counted(void) {
    setup();
    A.reject_odd = true;
    for (int k = 0; k < 4; ++k) { plat_cmd_t c = { .id = (uint16_t)k }; plat_cmdq_post(&Q, &c); }
    scan_step(&S);
    CHECK_EQ(S.stats.cmds_applied, 2);
    CHECK_EQ(S.stats.cmds_rejected, 2);
}

static void t_publish_in_lock(void) {
    setup();
    scan_step(&S); scan_step(&S);
    CHECK_EQ(A.publishes, 2);
    CHECK(A.seq_during_publish & 1u);     /* odd while writing */
    CHECK_EQ(lock.seq, 4);                /* even after, 2 per scan */
}

/* exec-time measurement: a fake cycle counter that advances inside the logic */
static uint32_t fake_cyc;
static uint32_t f_ms(void) { return bsp_host.ms; }
static uint32_t f_cyc(void) { return fake_cyc; }
static uint32_t f_cpm(void) { return 250; }
static uint32_t logic_cost_us;
static void slow_logic(void) { fake_cyc += logic_cost_us * 250u; }
static void t_exec_time(void) {
    bsp_host_reset();
    scan_hw_t hw = { .ms = f_ms, .cycles = f_cyc, .cycles_per_us = f_cpm };
    scan_app_t sl = { .logic = slow_logic };
    fake_cyc = 0xFFFFFF00u;                         /* wraps during the first scan */
    scan_init(&S, &sl, &hw, NULL, NULL, 10);
    logic_cost_us = 1200; scan_step(&S);
    CHECK_EQ(S.stats.last_exec_us, 1200);
    CHECK_EQ(S.stats.overruns, 0);
    logic_cost_us = 12000; scan_step(&S);            /* longer than the 10 ms period */
    CHECK_EQ(S.stats.max_exec_us, 12000);
    CHECK_EQ(S.stats.overruns, 1);
}

void test_scan_all(void) {
    RUN(t_init_called_once);
    RUN(t_first_scan_bit);
    RUN(t_dt);
    RUN(t_dt_wrap);
    RUN(t_dt_total_preserved);
    RUN(t_order);
    RUN(t_rtc);
    RUN(t_dow_table);
    RUN(t_inputs_frozen);
    RUN(t_outputs);
    RUN(t_outputs_default_off);
    RUN(t_cmds_bounded_and_ordered);
    RUN(t_cmd_reject_counted);
    RUN(t_publish_in_lock);
    RUN(t_exec_time);
}
