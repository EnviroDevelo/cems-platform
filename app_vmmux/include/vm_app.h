/* vm_app.h - the Valve Manager / Multiplexor application on the CEMS core platform.
 *
 *   main():  plat_start(vmmux_app());
 *
 * Everything the platform needs is in vmmux_app() (plat_tasks.h). The program itself
 * is the integrated Stage 4 code: logic() is Function1, the VM top level, which calls
 * MUX once per scan (30_Call_Graph section 1). See app_vmmux/INTEGRATION.md. */
#ifndef VM_APP_H
#define VM_APP_H
#include <stdint.h>
#include <stdbool.h>
#include "plat_tasks.h"
#include "plat_io.h"

#define VM_APP_REVISION "0.1.0-dev"

/* everything the platform needs: plat_start(vmmux_app()). Filled on the first call
 * (the tables live in separate files, so their sizes are not constant expressions here). */
const plat_app_t *vmmux_app(void);
extern const scan_app_t vmmux_scan;

/* ---- tables (vm_params.c, vm_alarms.c, vm_modbus.c, vm_log.c) ---- */
extern const param_def_t     *const vm_params;      extern const size_t vm_n_params;
extern const retain_def_t    *const vm_retain;      extern const size_t vm_n_retain;
extern const alarm_def_t     *const vm_alarm_defs;  extern const size_t vm_n_alarms;
extern const mb_map_t        *const vm_modbus;      extern const size_t vm_n_modbus;
extern const plat_log_chan_t *const vm_log_chans;   extern const size_t vm_n_log_chans;
extern const uint16_t        *const vm_home_ids;    extern const size_t vm_n_home;

/* ---- I/O (vm_io.c) ---- */
#define VM_AI_STATUS_FAULT 0x7F   /* AI status for any platform fault except OVER (ignored by the logic) */
int16_t vm_ai_to_ta30(int32_t raw);
int16_t vm_ai_status_to_ta30(uint8_t plat_status);
int16_t vm_rtd_to_ta30(int32_t cdeg, uint8_t plat_status);
void    vm_io_in(const plat_io_in_t *in);
void    vm_io_out(plat_io_out_t *out);
extern uint8_t vm_chan_ai_status[2], vm_chan_rtd_status[2];   /* plat_ain_status_t, this scan */

/* ---- application events (plat_log_event codes >= LOG_EV_APP) ---- */
enum {
    VM_EV_REMOTE_LINK_LOST = 100,   /* remote mode and no remote traffic for the timeout */
    VM_EV_REMOTE_LINK_BACK = 101,
    VM_EV_LINK_VM_STOPPED  = 102,   /* Q1: VM Modbus run ended after vm_link_close_min of link loss */
};

/* ---- after-logic hooks (vm_app.c) ---- */
void vm_after_logic(void);        /* alarms, data log, link watchdog: once per scan after Function1 */
bool vm_fw_safe(char *why, size_t cap);
/* The application's own parameter entries (422 on 1 Nov 2026), held in one array for plat_app_t.
 * The build's PARAM_MAX (app_vars.mk, 1024) must cover these plus the platform's (389). */
#define VM_APP_PARAMS_CAP 512u
bool vm_busy(char *why, size_t cap);   /* a sequence, manual run or purge is active */
void vm_compat_scan(void);       /* old Modbus coils and buffers kept meaningful (vm_compat.c) */

#endif
