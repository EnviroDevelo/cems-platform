/* test_log.c - event log, data logger, CSV export, power-cut and fault handling. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "tst.h"
#include "plat_log.h"
#include "log_csv.h"
#include "param.h"
#include "param_store.h"
#include "http_api.h"
#include "ul_system.h"
#include "bsp_host.h"

#ifndef FUZZ_SEED
#define FUZZ_SEED 12345u
#endif

extern uint16_t plat_log_interval_s;

#define NOR_SIZE (64u * 1024u)       /* 16 sectors: 3 for events, 13 for data */
static uint8_t   nor_mem[NOR_SIZE];
static nor_dev_t nor;
static nor_sim_t sim;
static uint8_t   fr_mem[1024];
static nvm_dev_t fr;
static nvm_ram_t fr_ram;
static uint32_t  tms = 1000;

static const plat_log_chan_t chans[] = {
    { "SO2", "mg/m3", 1 }, { "NOx", "ppm", 2 }, { "flow", NULL, 0 },
};

static void set_clock(uint32_t t) {
    unsigned y, mo, d, h, mi, s;
    plat_civil_from_unix(t, &y, &mo, &d, &h, &mi, &s);
    RTC_Date.Year = (uint16_t)y; RTC_Date.Month = (uint8_t)mo; RTC_Date.Day_In_Month = (uint8_t)d;
    RTC_Time.Hours = (uint8_t)h; RTC_Time.Minutes = (uint8_t)mi; RTC_Time.Seconds = (uint8_t)s;
}
static void clock_off(void) { memset(&RTC_Date, 0, sizeof RTC_Date); memset(&RTC_Time, 0, sizeof RTC_Time); }

/* (re)boot the logger on the same flash and FRAM */
static bool boot(const plat_log_chan_t *c, unsigned n, uint32_t unix_now) {
    plat_log_reset_for_test();
    param_on_change = NULL;
    plat_log_cfg_t cfg = { .nor = &nor, .fram = &fr, .fram_addr = 0, .chans = c, .n_chans = n,
                           .fw_version = "1.2.3", .reset_cause = BSP_RESET_POWER_ON, .unix_now = unix_now };
    return plat_log_init(&cfg, bsp_host.ms);
}
static void fresh(void) {
    nor_sim_init(&nor, &sim, nor_mem, NOR_SIZE, true, true);
    memset(fr_mem, 0, sizeof fr_mem);
    nvm_ram_init(&fr, &fr_ram, fr_mem, sizeof fr_mem, true);
}
static void poll(void) { for (int k = 0; k < 4; ++k) plat_log_poll(bsp_host.ms); }

/* read a whole ring into buf (slots, oldest first) */
static uint8_t rd[NOR_SIZE];
static uint32_t read_ring(uint8_t ring) {
    log_ring_info_t inf;
    if (!plat_log_info(ring, &inf)) return 0;
    if (inf.used == 0) return 0;
    CHECK_EQ(plat_log_read_start(ring, inf.oldest, inf.used, rd), 0);
    CHECK_EQ(plat_log_read_start(ring, inf.oldest, inf.used, rd), -1);   /* one job at a time */
    int32_t res = 0;
    CHECK(!plat_log_read_done(&res));
    plat_log_poll(bsp_host.ms);
    CHECK(plat_log_read_done(&res));
    CHECK_EQ(res, (int32_t)inf.used);
    return inf.used;
}

static char out[256 * 1024]; static size_t outn;
static int cap(void *ctx, const void *p, size_t n) {
    (void)ctx; if (outn + n >= sizeof out) return -1;
    memcpy(out + outn, p, n); outn += n; out[outn] = 0; return 0;
}
static const char *csv_of(uint8_t ring, uint32_t from, uint32_t to) {
    static log_csv_t c;
    http_out_t o; out_init(&o, cap, NULL); outn = 0; out[0] = 0;
    uint32_t n = read_ring(ring);
    log_csv_begin(&c, ring, chans, 3, plat_log_interval_s, from, to, &o);
    log_csv_feed(&c, rd, n, &o);
    out_flush(&o);
    return out;
}
static int count(const char *s, const char *needle) {
    int n = 0; for (const char *p = s; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

/* ---------------------------------------------------------------- */
static void t_calendar(void) {
    CHECK_EQ(plat_unix_from_civil(1970, 1, 1, 0, 0, 0), 0);
    CHECK_EQ(plat_unix_from_civil(2026, 10, 19, 12, 0, 0), 1792411200u);
    CHECK_EQ(plat_unix_from_civil(2000, 2, 29, 23, 59, 59), 951868799u);
    unsigned y, mo, d, h, mi, s;
    for (uint32_t t = 0; t < 4000000000u; t += 86399u * 7u + 3601u) {
        plat_civil_from_unix(t, &y, &mo, &d, &h, &mi, &s);
        if (plat_unix_from_civil(y, mo, d, h, mi, s) != t) { CHECK(0); break; }
    }
    char b[40];
    log_fmt_time(b, sizeof b, 1792411200u + 3723u, true); CHECK(!strcmp(b, "2026-10-19 13:02:03"));
    log_fmt_time(b, sizeof b, 75u, false); CHECK(!strcmp(b, "uptime 75s"));
    log_fmt_fixed(b, sizeof b, 12.345, 2); CHECK(!strcmp(b, "12.35") || !strcmp(b, "12.34"));
    log_fmt_fixed(b, sizeof b, -0.5, 0); CHECK(!strcmp(b, "-1") || !strcmp(b, "-0") || !strcmp(b, "0"));
    log_fmt_fixed(b, sizeof b, -3.05, 1); CHECK(!strcmp(b, "-3.1") || !strcmp(b, "-3.0"));
    log_fmt_fixed(b, sizeof b, 7.0, 3); CHECK(!strcmp(b, "7.000"));
    log_fmt_fixed(b, sizeof b, NAN, 3); CHECK(!strcmp(b, ""));
    log_fmt_fixed(b, sizeof b, 0.04, 1); CHECK(!strcmp(b, "0.0"));
}

static void t_events_basic(void) {
    fresh();
    CHECK(boot(chans, 3, 1792411200u));
    CHECK_EQ(plat_log_status() & ~(uint32_t)LSTAT_NO_CLOCK, 0);   /* blank flash and FRAM: a clean start */
    plat_log_event(LOG_EV_LOGIN, PLAT_SRC_WEB, PLAT_LVL_ADMIN, 0, NAN, NAN, NULL);
    plat_log_event(LOG_EV_APP + 5, PLAT_SRC_INTERNAL, 0, 77, 1.5, 2.5, "valve, \"3\"");
    poll();
    log_ring_info_t inf;
    CHECK(plat_log_info(LOG_RING_EVENTS, &inf));
    CHECK_EQ(inf.used, 3); CHECK_EQ(inf.next_seq, 4); CHECK_EQ(inf.nslots, 3u * 64u);
    CHECK_EQ(read_ring(LOG_RING_EVENTS), 3);
    log_event_t e;
    CHECK(plat_log_decode_event(rd, &e));
    CHECK_EQ(e.code, LOG_EV_POWER_UP); CHECK_EQ(e.seq, 1); CHECK_EQ(e.arg, BSP_RESET_POWER_ON);
    CHECK(!strcmp(e.text, "fw 1.2.3")); CHECK(e.clock_set); CHECK_EQ(e.time, 1792411200u);
    CHECK(plat_log_decode_event(rd + 128, &e));
    CHECK_EQ(e.code, LOG_EV_APP + 5); CHECK_EQ(e.arg, 77); CHECK(e.old_v == 1.5 && e.new_v == 2.5);
    rd[128 + 20] ^= 1;                                    /* corrupt one byte: CRC catches it */
    CHECK(!plat_log_decode_event(rd + 128, &e));

    const char *c = csv_of(LOG_RING_EVENTS, 0, 0);
    CHECK(!strncmp(c, "seq,time,event,source,level,id,detail,old,new\r\n", 47));
    CHECK(strstr(c, "1,2026-10-19 12:00:00.000,power_up,unit,,1,fw 1.2.3,,\r\n") != NULL);
    CHECK(strstr(c, ",login,web,admin,,,,\r\n") != NULL);
    CHECK(strstr(c, ",app 105,unit,,77,\"valve, \"\"3\"\"\",1.5,2.5\r\n") != NULL);
    CHECK_EQ(sim.page_violations, 0);
}

static void t_param_events(void) {
    fresh();
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    param_set_defaults(0);
    CHECK(boot(chans, 3, 1792411200u));
    CHECK(param_on_change != NULL);
    plat_cmd_t c = { .id = PARAM_ID_COMMS_TIMEOUT_S, .src = PLAT_SRC_WEB, .level = PLAT_LVL_ADMIN,
                     .kind = PLAT_VAL_U32, .v.u = 25 };
    CHECK_EQ(param_apply(&c), PERR_OK);
    CHECK_EQ(param_apply(&c), PERR_OK);                   /* same value again: not logged */
    plat_cmd_t m = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR,
                     .kind = PLAT_VAL_U32, .v.u = PARAM_MODE_REMOTE };
    CHECK_EQ(param_apply(&m), PERR_OK);
    plat_cmd_t i = c; i.src = PLAT_SRC_INTERNAL; i.v.u = 30;
    CHECK_EQ(param_apply(&i), PERR_OK);                   /* the unit itself: not an audit event */
    poll();
    const char *s = csv_of(LOG_RING_EVENTS, 0, 0);
    CHECK(strstr(s, ",param,web,admin,3,comms_timeout,10,25\r\n") != NULL);
    CHECK(strstr(s, ",mode,touchscreen,operator,1,ctrl_mode,LOCAL,REMOTE\r\n") != NULL);
    CHECK_EQ(count(s, ",param,"), 1);
    param_on_change = NULL;
    m.v.u = PARAM_MODE_LOCAL; (void)param_apply(&m);
}

static void t_data(void) {
    fresh();
    uint32_t t0 = 1792411200u;             /* on a minute boundary */
    plat_log_interval_s = 10;
    set_clock(t0 + 3);
    CHECK(boot(chans, 3, t0 + 3));
    /* 10 ms scans for 25 s: SO2 ramps, NOx flagged once, flow has no valid sample in the 2nd interval */
    uint32_t ms = bsp_host.ms;
    for (uint32_t k = 0; k < 2500u; ++k) {
        uint32_t t = t0 + 3 + k / 100u;
        set_clock(t);
        plat_log_scan(ms);
        plat_log_put(0, (float)(t - t0), LQ_OK);
        plat_log_put(1, 4.0f, (t == t0 + 15) ? LQ_FLAG : LQ_OK);
        if (t < t0 + 10 || t >= t0 + 20) plat_log_put(2, 100.0f, LQ_OK);
        else plat_log_put(2, 0.0f, LQ_NA);
        ms += 10;
        if (k % 50u == 0) plat_log_poll(ms);
    }
    poll();
    const char *s = csv_of(LOG_RING_DATA, 0, 0);
    CHECK(!strncmp(s, "time,SO2 [mg/m3],SO2 q,NOx [ppm],NOx q,flow,flow q\r\n", 51));
    /* first interval (partial: 3..9 s) mean of 3..9 = 6; second 10..19 = 14.5 */
    CHECK(strstr(s, "2026-10-19 12:00:10,6.0,OK,4.00,OK,100,OK\r\n") != NULL);
    CHECK(strstr(s, "2026-10-19 12:00:20,14.5,OK,4.00,FLAG,,N/A\r\n") != NULL);
    CHECK_EQ(count(s, "\r\n"), 3);          /* header + 2 rows: the 3rd interval is still open */
    /* time filter */
    s = csv_of(LOG_RING_DATA, t0 + 15, 0);
    CHECK(strstr(s, "12:00:10") == NULL); CHECK(strstr(s, "12:00:20") != NULL);

    /* interval change: the open interval is closed, a new schema and a new header follow */
    plat_log_interval_s = 5;
    for (uint32_t k = 0; k < 1200u; ++k) {
        set_clock(t0 + 28 + k / 100u);
        plat_log_scan(ms);
        plat_log_put(0, 1.0f, LQ_OK);
        ms += 10;
    }
    poll();
    s = csv_of(LOG_RING_DATA, 0, 0);
    CHECK_EQ(count(s, "time,SO2"), 2);
    CHECK(strstr(s, "\r\n\r\ntime,SO2") != NULL);
    CHECK(strstr(s, "2026-10-19 12:00:35,1.0,OK,,N/A,,N/A\r\n") != NULL);

    /* clock step: logged, and the interval is closed at the old time */
    set_clock(t0 + 3600); plat_log_scan(ms); ms += 10;
    poll();
    s = csv_of(LOG_RING_EVENTS, 0, 0);
    CHECK(strstr(s, ",time_set,unit,,,,2026-10-19 12:00:39,2026-10-19 13:00:00\r\n") != NULL);

    /* a restart writes the schema again, but the CSV doesn't repeat an identical header */
    CHECK(boot(chans, 3, t0 + 3600));
    for (uint32_t k = 0; k < 1100u; ++k) { set_clock(t0 + 3600 + k / 100u); plat_log_scan(ms); plat_log_put(0, 2.0f, LQ_BAD); ms += 10; }
    poll();
    s = csv_of(LOG_RING_DATA, 0, 0);
    CHECK_EQ(count(s, "time,SO2"), 2);
    CHECK(strstr(s, "2026-10-19 13:00:05,2.0,BAD") != NULL);
    plat_log_interval_s = 60;
}

static void t_schema_lost(void) {
    /* data slots whose schema record is gone: names from the running table if it matches, else ch1.. */
    fresh();
    plat_log_interval_s = 10;
    set_clock(1792411200u);
    CHECK(boot(chans, 3, 1792411200u));
    uint32_t ms = bsp_host.ms;
    for (uint32_t k = 0; k < 2100u; ++k) { set_clock(1792411200u + k / 100u); plat_log_scan(ms); plat_log_put(1, 3.0f, LQ_OK); ms += 10; }
    poll();
    uint32_t n = read_ring(LOG_RING_DATA);
    CHECK_EQ(n, 3u + 1u + 1u);                    /* schema (3 slots) + 2 one-slot data records */
    static log_csv_t c;
    http_out_t o; out_init(&o, cap, NULL); outn = 0;
    log_csv_begin(&c, LOG_RING_DATA, chans, 3, 10, 0, 0, &o);
    log_csv_feed(&c, rd + 3 * LOG_SLOT, n - 3, &o);           /* without the schema */
    out_flush(&o);
    CHECK(strstr(out, "time,SO2 [mg/m3],SO2 q,NOx [ppm]") != NULL);
    out_init(&o, cap, NULL); outn = 0;
    log_csv_begin(&c, LOG_RING_DATA, chans, 2, 10, 0, 0, &o);  /* table changed since */
    log_csv_feed(&c, rd + 3 * LOG_SLOT, n - 3, &o);
    out_flush(&o);
    CHECK(strstr(out, "time,ch1,ch1 q,ch2,ch2 q,ch3,ch3 q\r\n") != NULL);
    CHECK(strstr(out, ",3.000,OK,") != NULL);
    plat_log_interval_s = 60;
}

static void t_wrap(void) {
    fresh();
    CHECK(boot(NULL, 0, 1792411200u));
    /* 3 sectors x 64 slots; write 1000 events: the ring wraps 5 times */
    for (uint32_t k = 0; k < 1000u; ++k) {
        plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, k, NAN, NAN, NULL);
        if (k % 20u == 0) plat_log_poll(bsp_host.ms);
    }
    poll();
    log_ring_info_t inf;
    CHECK(plat_log_info(LOG_RING_EVENTS, &inf));
    CHECK_EQ(inf.next_seq, 1002);
    CHECK(inf.used >= 64u && inf.used <= 128u);           /* one sector erased ahead, one being filled */
    uint32_t n = read_ring(LOG_RING_EVENTS);
    log_event_t e, prev = { 0 };
    uint32_t good = 0;
    for (uint32_t k = 0; k < n; ++k) {
        if (!plat_log_decode_event(rd + k * LOG_SLOT, &e)) continue;
        if (good) CHECK_EQ(e.seq, prev.seq + 1);           /* contiguous, oldest first */
        prev = e; good++;
    }
    CHECK_EQ(good, n);
    CHECK_EQ(prev.seq, 1001); CHECK_EQ(prev.arg, 999);
    CHECK_EQ(sim.page_violations, 0);
    /* wear is even: every event sector erased about the same number of times */
    CHECK(sim.erases >= 15u && sim.erases <= 20u);
    /* data ring untouched by events */
    CHECK(plat_log_info(LOG_RING_DATA, &inf)); CHECK_EQ(inf.used, 0);
}

static void restart_check_seq(uint32_t expect_next, bool expect_rebuilt) {
    CHECK(boot(NULL, 0, 1792411200u));
    log_ring_info_t inf;
    CHECK(plat_log_info(LOG_RING_EVENTS, &inf));
    if (expect_next) CHECK_EQ(inf.next_seq, expect_next);
    CHECK_EQ(!!(plat_log_status() & LSTAT_REBUILT), expect_rebuilt);
}

static void t_restart_and_recovery(void) {
    fresh();
    CHECK(boot(NULL, 0, 1792411200u));
    for (uint32_t k = 0; k < 150u; ++k) { plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, k, NAN, NAN, NULL); if (k % 30u == 0) poll(); }
    poll();                                               /* 1 power-up + 150 = seq 1..151 */
    restart_check_seq(152, false);                         /* FRAM pointer used */
    poll();                                               /* power-up #2 = 152 */

    memset(fr_mem, 0, sizeof fr_mem);                     /* FRAM pointer lost: rebuilt from the flash */
    restart_check_seq(153, true);
    poll();                                               /* 153 */

    uint8_t saved[sizeof fr_mem]; memcpy(saved, fr_mem, sizeof saved);
    for (uint32_t k = 0; k < 10u; ++k) plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, k, NAN, NAN, NULL);
    poll();                                               /* 154..163 */
    memcpy(fr_mem, saved, sizeof saved);                  /* stale pointer (power lost before it was saved) */
    restart_check_seq(164, false);                         /* rolled forward over the newer slots */
    poll();

    /* power cut in the middle of writing a slot */
    plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, 1, NAN, NAN, NULL);
    sim.prog_fail_after = 30;
    poll();                                               /* the torn slot fails verification */
    sim.prog_fail_after = -1;
    CHECK(plat_log_status() & LSTAT_NOR_FAULT);           /* (a real cut would not get this far) */
    memset(fr_mem, 0, sizeof fr_mem);
    restart_check_seq(0, true);                            /* (sequence checked below) */
    log_ring_info_t inf; CHECK(plat_log_info(LOG_RING_EVENTS, &inf));
    uint32_t n = read_ring(LOG_RING_EVENTS);
    uint32_t good = 0, last = 0; log_event_t e;
    for (uint32_t k = 0; k < n; ++k) if (plat_log_decode_event(rd + k * LOG_SLOT, &e)) { CHECK(e.seq > last); last = e.seq; good++; }
    CHECK(good > 60u);
    CHECK_EQ(inf.next_seq, last + 1);
}

static void t_faults(void) {
    fresh();
    CHECK(boot(NULL, 0, 1792411200u));
    /* a worn cell in the 3rd slot: verification fails, the slot is skipped, the record lands in the next one */
    sim.stuck_addr = 2u * LOG_SLOT + 20u; sim.stuck_bits = 0x01;
    for (uint32_t k = 0; k < 5u; ++k) plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, 0x10u + k, NAN, NAN, NULL);
    poll();
    CHECK(plat_log_status() & LSTAT_NOR_FAULT);
    uint32_t n = read_ring(LOG_RING_EVENTS);
    uint32_t args = 0; log_event_t e;
    for (uint32_t k = 0; k < n; ++k) if (plat_log_decode_event(rd + k * LOG_SLOT, &e) && e.code == LOG_EV_APP) args++;
    CHECK_EQ(args, 5);
    const char *s = csv_of(LOG_RING_EVENTS, 0, 0);
    CHECK(strstr(s, ",log_fault,unit,,2,,,\r\n") != NULL);
    sim.stuck_bits = 0;

    /* an erase that fails part way: that sector is skipped */
    fresh();
    CHECK(boot(NULL, 0, 1792411200u));
    sim.erase_fail_after = 2;                             /* sectors 0 and 1 erase, sector 2 fails */
    for (uint32_t k = 0; k < 200u; ++k) { plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, k, NAN, NAN, NULL); if (k % 20u == 0) poll(); }
    sim.erase_fail_after = -1;
    poll();
    CHECK(plat_log_status() & LSTAT_NOR_FAULT);
    log_ring_info_t inf; CHECK(plat_log_info(LOG_RING_EVENTS, &inf));
    CHECK(inf.next_seq > 150u);                           /* logging carried on */

    /* RAM queue overflow: records lost are counted and reported */
    fresh();
    CHECK(boot(NULL, 0, 1792411200u));
    for (uint32_t k = 0; k < PLAT_LOG_QSLOTS + 10u; ++k) plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, k, NAN, NAN, NULL);
    poll();
    CHECK(plat_log_status() & LSTAT_LOST);
    s = csv_of(LOG_RING_EVENTS, 0, 0);
    CHECK(strstr(s, ",events_lost,unit,,11,,,\r\n") != NULL);    /* 10 + the power-up event already queued */

    /* no flash: off, and says so */
    plat_log_reset_for_test();
    plat_log_cfg_t cfg = { 0 };
    CHECK(!plat_log_init(&cfg, 0));
    CHECK_EQ(plat_log_read_start(0, 0, 1, rd), -1);
    log_ring_info_t i2; CHECK(!plat_log_info(0, &i2));
    /* flash too small for two rings */
    nor_sim_init(&nor, &sim, nor_mem, 16u * 1024u, true, true);
    plat_log_reset_for_test();
    cfg.nor = &nor;
    CHECK(!plat_log_init(&cfg, 0));
    CHECK(plat_log_status() & LSTAT_TOO_SMALL);
    /* RAM stand-in and no clock */
    nor_sim_init(&nor, &sim, nor_mem, NOR_SIZE, false, true);
    plat_log_reset_for_test();
    CHECK(plat_log_init(&cfg, 5000));
    clock_off(); plat_log_scan(5000);
    CHECK(plat_log_status() & LSTAT_VOLATILE);
    CHECK(plat_log_status() & LSTAT_NO_CLOCK);
    poll();
    s = csv_of(LOG_RING_EVENTS, 0, 0);
    CHECK(strstr(s, "1,uptime 5s,power_up") != NULL);
}


/* Random power cuts while events and data records are being written, with the FRAM
 * pointer sometimes current and sometimes stale. After every restart: everything in
 * the flash reads back in sequence order, every record that was fully written before
 * the cut is still there, and new records continue after the old ones. */
static void t_powercut_fuzz(void) {
    static const plat_log_chan_t many[20] = {
        {"c1",0,1},{"c2",0,1},{"c3",0,1},{"c4",0,1},{"c5",0,1},{"c6",0,1},{"c7",0,1},{"c8",0,1},{"c9",0,1},{"c10",0,1},
        {"c11",0,1},{"c12",0,1},{"c13",0,1},{"c14",0,1},{"c15",0,1},{"c16",0,1},{"c17",0,1},{"c18",0,1},{"c19",0,1},{"c20",0,1} };
    fresh();
    srand(FUZZ_SEED);
    plat_log_interval_s = 1;
    uint32_t t = 1792411200u, ms = 1000;
    uint32_t bad = 0, cycles = 0, min_kept_ev = 0, min_kept_da = 0;
    uint8_t saved_fr[sizeof fr_mem];
    for (int round = 0; round < 400; ++round) {
        set_clock(t);
        CHECK(boot(many, 20, t));
        /* a previous record that was complete must have survived */
        for (uint8_t ring = 0; ring < 2; ++ring) {
            log_ring_info_t inf; CHECK(plat_log_info(ring, &inf));
            uint32_t n = read_ring(ring), last = 0, top = 0;
            bool order = true;
            for (uint32_t k = 0; k < n; ++k) {
                const uint8_t *sl = rd + k * LOG_SLOT;
                if (!plat_log_slot_ok(sl)) continue;
                uint32_t q = (uint32_t)sl[4] | ((uint32_t)sl[5] << 8) | ((uint32_t)sl[6] << 16) | ((uint32_t)sl[7] << 24);
                if (q < last) order = false;
                last = q; if (q > top) top = q;
            }
            uint32_t need = ring ? min_kept_da : min_kept_ev;
            if ((!order || inf.next_seq <= top || (need && top < need)) && bad < 8)
                printf("  round %d ring %u: order %d next %u top %u need %u used %u status %x wr %u oldest %u\n", round, ring, order, inf.next_seq, top, need, inf.used, plat_log_status(), inf.wr, inf.oldest);
            if (!order) bad++;
            if (inf.next_seq <= top) bad++;
            if (need && top < need) bad++;
        }
        memcpy(saved_fr, fr_mem, sizeof saved_fr);
        /* run for a while, then cut the power at a random point */
        int steps = 20 + rand() % 200;
        if (rand() % 2) sim.cut_after_bytes = rand() % 4000; else sim.cut_after_erases = rand() % 3;
        for (int k = 0; k < steps && !sim.dead; ++k) {
            for (int j = 0; j < 10; ++j) {             /* 100 ms of scans */
                set_clock(t + (uint32_t)(ms / 1000u) % 2u);
                plat_log_scan(ms);
                for (unsigned ch = 0; ch < 20; ++ch) plat_log_put(ch, (float)k, LQ_OK);
                ms += 10;
            }
            if (rand() % 3 == 0) plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, (uint32_t)k, NAN, NAN, NULL);
            plat_log_poll(ms);
            if (!sim.dead) {                           /* what is in the flash now must survive */
                log_ring_info_t a, b2;
                plat_log_info(LOG_RING_EVENTS, &a); plat_log_info(LOG_RING_DATA, &b2);
                min_kept_ev = a.next_seq > 1 ? a.next_seq - 1 : 0;
                min_kept_da = b2.next_seq > 1 ? b2.next_seq - 1 : 0;
            }
        }
        if (!sim.dead) { cycles++; }
        sim.dead = false; sim.cut_after_bytes = -1; sim.cut_after_erases = -1;
        if (rand() % 3 == 0) memcpy(fr_mem, saved_fr, sizeof fr_mem);    /* pointer from the last boot */
        if (rand() % 10 == 0) memset(fr_mem, 0, sizeof fr_mem);          /* pointer gone */
        t += 7;
        ms += 5000;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(sim.page_violations, 0);
    (void)cycles;
    plat_log_interval_s = 60;
}

/* ---------------- web endpoints ---------------- */
static uint8_t scratch[8 * 1024];       /* 128 slots per read: exercises chunking */
static void web_wait(void) { plat_log_poll(bsp_host.ms); }
static int get(const char *path) {
    static char raw[512];
    snprintf(raw, sizeof raw, "GET %s HTTP/1.1\r\nHost: unit\r\n\r\n", path);
    http_req_t r;
    if (http_parse_head(raw, (uint32_t)strlen(raw), &r) <= 0) return -1;
    http_out_t o; out_init(&o, cap, NULL); outn = 0;
    http_handle(&r, &o, 1000);
    int st = 0; sscanf(out, "HTTP/1.1 %d", &st);
    return st;
}

static void t_web(void) {
    fresh();
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    CHECK(boot(chans, 3, 1792411200u));
    http_cfg_t c = { .product = "CEMS", .fw_version = "t", .scratch = scratch, .scratch_size = sizeof scratch,
                     .wait = web_wait, .log_chans = chans, .n_log_chans = 3 };
    http_api_init(&c);
    for (uint32_t k = 0; k < 150u; ++k) { plat_log_event(LOG_EV_APP, PLAT_SRC_INTERNAL, 0, k, NAN, NAN, NULL); if (k % 30u == 0) poll(); }
    poll();
    CHECK_EQ(get("/api/log/info"), 200);
    CHECK(strstr(out, "\"events\":{\"slots\":192,\"used\":87,\"next_seq\":152}") != NULL);
    CHECK_EQ(get("/api/log/events?n=3"), 200);
    CHECK(strstr(out, "{\"events\":[{\"seq\":151,") != NULL);
    CHECK_EQ(count(out, "\"seq\":"), 3);
    CHECK(strstr(out, "\"seq\":149") != NULL);
    CHECK_EQ(get("/api/log/events.csv"), 200);
    CHECK(strstr(out, "Content-Type: text/csv") != NULL);
    CHECK(strstr(out, "Content-Disposition: attachment; filename=\"cems-0-events.csv\"") != NULL);
    CHECK_EQ(count(out, ",app 100,unit,"), 87);          /* seq 65..151: the first sector was erased ahead */
    CHECK_EQ(get("/api/log/data.csv"), 200);
    CHECK_EQ(get("/api/info"), 200);
    CHECK(strstr(out, "\"log_status\":") != NULL);
    plat_log_reset_for_test();
    CHECK_EQ(get("/api/log/events.csv"), 503);
    param_on_change = NULL;
}

void test_log_all(void) {
    bsp_host_reset();
    bsp_host.ms = 1000;
    (void)tms;
    RUN(t_calendar);
    RUN(t_events_basic);
    RUN(t_param_events);
    RUN(t_data);
    RUN(t_schema_lost);
    RUN(t_wrap);
    RUN(t_restart_and_recovery);
    RUN(t_faults);
    RUN(t_powercut_fuzz);
    RUN(t_web);
    clock_off();
    plat_log_reset_for_test();
    param_on_change = NULL;
}
