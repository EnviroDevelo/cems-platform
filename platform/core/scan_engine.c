/* scan_engine.c - see scan_engine.h */
#include <string.h>
#include "scan_engine.h"
#include "ul_system.h"
#include "plat_config.h"
#include "iobus.h"

/* The single definitions of the UniLogic system tags (ul_system.h). */
uint32_t   g_dt_ms;
bool       Ladder_Initial_Cycle;
rtc_date_t RTC_Date;
rtc_time_t RTC_Time;

const scan_hw_t scan_hw_bsp = {
    .ms = bsp_ms, .cycles = bsp_cycles, .cycles_per_us = bsp_cycles_per_us, .rtc_read = bsp_rtc_read,
    .io_read = plat_hw_io_read, .io_write = plat_hw_io_write,   /* main-board pins + I/O boards (iobus.h) */
};

uint8_t scan_dow_iso_to_ul(uint8_t dow_iso) {
    if (dow_iso < 1 || dow_iso > 7) return 0;
    return (uint8_t)((dow_iso % 7u) + 1u);      /* Mon(1)->2 ... Sat(6)->7, Sun(7)->1 */
}

void scan_init(scan_engine_t *s, const scan_app_t *app, const scan_hw_t *hw,
               plat_cmdq_t *cmdq, plat_seqlock_t *snap_lock, uint32_t period_ms) {
    memset(s, 0, sizeof *s);
    s->app = app;
    s->hw = hw;
    s->cmdq = cmdq;
    s->snap_lock = snap_lock;
    s->period_ms = period_ms ? period_ms : PLAT_SCAN_PERIOD_MS;
    g_dt_ms = 0;
    Ladder_Initial_Cycle = false;
    if (app && app->init) app->init();
}

static void take_rtc(scan_engine_t *s) {
    bsp_rtc_t t;
    if (!s->hw->rtc_read || !s->hw->rtc_read(&t)) { s->stats.rtc_invalid++; return; }
    uint8_t dow = scan_dow_iso_to_ul(t.dow_iso);
    if (dow == 0) { s->stats.rtc_invalid++; return; }
    RTC_Date.Year = t.year;
    RTC_Date.Month = t.month;
    RTC_Date.Day_In_Month = t.day;
    RTC_Date.Day_Of_Week = dow;
    RTC_Time.Hours = t.hours;
    RTC_Time.Minutes = t.minutes;
    RTC_Time.Seconds = t.seconds;
}

static void apply_cmds(scan_engine_t *s) {
    if (!s->cmdq) return;
    plat_cmd_t c;
    for (uint32_t n = 0; n < PLAT_SCAN_MAX_CMDS && plat_cmdq_pop(s->cmdq, &c); ++n) {
        int rc = SCAN_CMD_UNKNOWN;
        if (s->sys && s->sys->apply) rc = s->sys->apply(&c);
        if (rc == SCAN_CMD_UNKNOWN) rc = s->app->apply_cmd ? s->app->apply_cmd(&c) : -1;
        if (rc == 0) s->stats.cmds_applied++; else s->stats.cmds_rejected++;
    }
}

void scan_set_sys(scan_engine_t *s, const scan_sys_t *sys) { s->sys = sys; }

void scan_step(scan_engine_t *s) {
    const scan_app_t *app = s->app;
    uint32_t now = s->hw->ms();
    uint32_t c0 = s->hw->cycles ? s->hw->cycles() : 0;

    /* 1. elapsed time (unsigned subtraction is wrap-safe) */
    uint32_t dt = s->started ? (uint32_t)(now - s->last_start_ms) : 0u;
    s->last_start_ms = now;
    g_dt_ms = dt;
    s->stats.last_dt_ms = dt;
    if (dt > s->stats.max_dt_ms) s->stats.max_dt_ms = dt;
    if (s->started && dt > s->period_ms) s->stats.late_starts++;

    /* 2. clock snapshot */
    take_rtc(s);

    /* 3. inputs */
    memset(&s->in, 0, sizeof s->in);
    if (s->hw->io_read) s->hw->io_read(&s->in);
    if (app->io_in) app->io_in(&s->in);

    /* 4. external writes, between scans as far as the logic is concerned */
    apply_cmds(s);
    if (s->sys && s->sys->pre_logic) s->sys->pre_logic(now);

    /* 5. logic */
    Ladder_Initial_Cycle = !s->started;
    app->logic();
    Ladder_Initial_Cycle = false;
    s->started = true;

    /* 6. outputs: start from all-off so an unmapped channel can never stay on */
    memset(&s->out, 0, sizeof s->out);
    if (app->io_out) app->io_out(&s->out);
    if (s->sys && s->sys->io_out) s->sys->io_out(&s->out);
    if (s->hw->io_write) s->hw->io_write(&s->out);

    /* 7. publish */
    if (app->publish || (s->sys && s->sys->publish)) {
        if (s->snap_lock) plat_seq_write_begin(s->snap_lock);
        if (s->sys && s->sys->publish) s->sys->publish();
        if (app->publish) app->publish();
        if (s->snap_lock) plat_seq_write_end(s->snap_lock);
    }

    /* 8. statistics */
    s->stats.scans++;
    if (s->hw->cycles) {
        uint32_t cpu = s->hw->cycles_per_us ? s->hw->cycles_per_us() : 1u;
        uint32_t exec = (uint32_t)(s->hw->cycles() - c0) / (cpu ? cpu : 1u);
        s->stats.last_exec_us = exec;
        if (exec > s->stats.max_exec_us) s->stats.max_exec_us = exec;
        if (exec > s->period_ms * 1000u) s->stats.overruns++;
    }
}

scan_stats_t scan_stats(const scan_engine_t *s) { return s->stats; }
