/* scan_engine.h - PLC-style scan cycle that runs the translated VM/MUX program.
 *
 * One call to scan_step() is one UniLogic scan (11_UniLogic_Semantics E1, E10, E11):
 *
 *   1. g_dt_ms   = ms since the previous scan started (0 on the first scan)
 *   2. RTC_Date / RTC_Time = one clock snapshot for the whole scan
 *   3. inputs    : bsp_io_read -> app->io_in   (inputs frozen for the scan)
 *   4. commands  : up to PLAT_SCAN_MAX_CMDS queued external writes -> parameter table
 *                  (sys->apply), IDs not in the table -> app->apply_cmd; then sys->pre_logic
 *   5. logic     : Ladder_Initial_Cycle = first scan; app->logic()  (= Function1)
 *   6. outputs   : app->io_out -> bsp_io_write
 *   7. publish   : app->publish() inside the snapshot seqlock
 *   8. statistics: exec time, dt, overruns
 *
 * The engine is pure C with no OS calls, so it runs unchanged in host tests.
 * The FreeRTOS scan task (plat_tasks.c) just calls scan_step() every period. */
#ifndef SCAN_ENGINE_H
#define SCAN_ENGINE_H
#include <stdint.h>
#include <stdbool.h>
#include "plat_io.h"
#include "plat_cmdq.h"
#include "plat_seqlock.h"
#include "bsp.h"

/* What the application provides. Only logic is mandatory. */
typedef struct {
    void (*init)(void);                          /* once, before the first scan (power-up values, retained-tag load) */
    void (*io_in)(const plat_io_in_t *in);       /* hardware image -> application input tags */
    int  (*apply_cmd)(const plat_cmd_t *cmd);    /* external write; return 0 if accepted */
    void (*logic)(void);                         /* Function1: one full scan of the program */
    void (*io_out)(plat_io_out_t *out);          /* application output tags -> hardware image */
    void (*publish)(void);                       /* copy display values into the snapshot */
} scan_app_t;

/* Hardware access, normally the BSP; replaced by fakes in tests. */
typedef struct {
    uint32_t (*ms)(void);
    uint32_t (*cycles)(void);          /* may be NULL: no exec-time statistics */
    uint32_t (*cycles_per_us)(void);
    bool     (*rtc_read)(bsp_rtc_t *t);
    void     (*io_read)(plat_io_in_t *in);
    void     (*io_write)(const plat_io_out_t *out);
} scan_hw_t;

/* Platform services run by the scan (parameter table, store). Filled in by
 * plat_sys.c; NULL in tests that exercise the engine alone. */
#define SCAN_CMD_UNKNOWN 1     /* sys->apply: not a table ID, offer it to app->apply_cmd */
typedef struct {
    int  (*apply)(const plat_cmd_t *cmd);   /* 0 accepted, SCAN_CMD_UNKNOWN, or another code = rejected */
    void (*pre_logic)(uint32_t now_ms);     /* after commands, before the logic */
    void (*publish)(void);                  /* inside the seqlock, before app->publish */
    void (*io_out)(plat_io_out_t *out);     /* after app->io_out: platform-owned outputs (alarm relays) */
} scan_sys_t;

typedef struct {
    uint32_t scans;            /* completed scans */
    uint32_t overruns;         /* scans whose execution took longer than the period */
    uint32_t late_starts;      /* scans that started more than one period after the previous one */
    uint32_t last_dt_ms, max_dt_ms;
    uint32_t last_exec_us, max_exec_us;
    uint32_t cmds_applied, cmds_rejected;
    uint32_t rtc_invalid;      /* scans run with an invalid RTC (previous snapshot kept) */
} scan_stats_t;

typedef struct {
    const scan_app_t *app;
    const scan_hw_t  *hw;
    const scan_sys_t *sys;
    plat_cmdq_t      *cmdq;
    plat_seqlock_t   *snap_lock;
    uint32_t          period_ms;
    uint32_t          last_start_ms;
    bool              started;
    plat_io_in_t      in;
    plat_io_out_t     out;
    scan_stats_t      stats;
} scan_engine_t;

extern const scan_hw_t scan_hw_bsp;     /* scan_hw_t wired to bsp.h */

void scan_init(scan_engine_t *s, const scan_app_t *app, const scan_hw_t *hw,
               plat_cmdq_t *cmdq, plat_seqlock_t *snap_lock, uint32_t period_ms);
void scan_set_sys(scan_engine_t *s, const scan_sys_t *sys);
void scan_step(scan_engine_t *s);

/* UniLogic day of week (1 = Sunday) from ISO (1 = Monday). 0 if out of range. */
uint8_t scan_dow_iso_to_ul(uint8_t dow_iso);

/* Copy of the statistics for display (call from any task; may be torn by one
 * scan, acceptable for diagnostics). */
scan_stats_t scan_stats(const scan_engine_t *s);

#endif
