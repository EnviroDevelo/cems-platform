/* vm_app.c - the scan definition and plat_app_t of the VM/MUX application.
 *
 * Scan order (PLATFORM.md section 3, fixed by the platform):
 *   io_in   -> vm_io_in        platform image -> TA30_DI / UID_DI / TA30_AI / TA30_TEMP
 *   logic   -> Function1, then vm_after_logic (alarms, data log, remote-link watchdog)
 *   io_out  -> vm_io_out       TA30_DO / UID_DO -> platform image
 * The faithful code never calls the platform; everything platform-facing is here. */
#include <stdio.h>
#include <string.h>
#include "vm_app.h"
#include "vm_tags.h"
#include "param_store.h"
#include "vm_params.h"

uint8_t vm_chan_ai_status[2], vm_chan_rtd_status[2];

/* ---------------- the scan ---------------- */

static void vm_init(void)
{
    /* Power-up values are the static initialisers of src/vm_tags.c (generated from the
     * data dictionary); retained tags are then loaded by the parameter store before the
     * first scan. The table's text and block defaults are copies of those power-up values,
     * taken here, before param_init and the FRAM load (scan_init calls this first). */
    vm_params_snapshot();
}

static void vm_logic(void)
{
    Function1();          /* VM top level; calls MUX at rung 36 */
    vm_after_logic();
}

const scan_app_t vmmux_scan = {
    .init = vm_init, .io_in = vm_io_in, .logic = vm_logic, .io_out = vm_io_out, .publish = NULL,
};

/* ---------------- after the logic ---------------- */

/* Data-log quality of an analog or RTD channel (APP_INTEGRATION.md section 8). */
static uint8_t chan_quality(uint8_t plat_status, bool enabled)
{
    if (!enabled) return LQ_NA;
    if (plat_status != PLAT_AIN_OK) return LQ_BAD;
    if (!param_store_calibration_valid()) return LQ_BAD;     /* block measurement (spec: fail safe) */
    if (PurgeInletActivate || FlowValidationAlarm) return LQ_FLAG;
    return LQ_OK;
}

static void log_channels(void)
{
    /* Channels as vm_log.c: AI1, AI2 (scaled, AnalogSensor), RTD1, RTD2 (TemperatureSensor, 0.1 degree) */
    plat_log_put(0, AnalogSensor[0], chan_quality(vm_chan_ai_status[0], AnalogEnable[0]));
    plat_log_put(1, AnalogSensor[1], chan_quality(vm_chan_ai_status[1], AnalogEnable[1]));
    plat_log_put(2, (float)TemperatureSensor[0] / 10.0f, chan_quality(vm_chan_rtd_status[0], TemperatureEnable[0]));
    plat_log_put(3, (float)TemperatureSensor[1] / 10.0f, chan_quality(vm_chan_rtd_status[1], TemperatureEnable[1]));
}

/* Remote-link watchdog (APP_INTEGRATION.md section 4, PLATFORM.md section 4): the
 * application must go to its safe state. WHAT that safe state is for the valve
 * manager / multiplexor is not in the design spec or the analysis: open question Q1 in
 * INTEGRATION.md (Rule 0). Until it is answered this only records the event. */
static void remote_link_watch(void)
{
    static bool was_lost;
    bool lost = param_remote_link_lost();
    if (lost != was_lost) {
        plat_log_event(lost ? VM_EV_REMOTE_LINK_LOST : VM_EV_REMOTE_LINK_BACK, PLAT_SRC_INTERNAL,
                       PLAT_LVL_VIEW, 0, 0.0, 0.0, lost ? "remote link lost" : "remote link back");
        was_lost = lost;
    }
}

void vm_after_logic(void)
{
    log_channels();
    remote_link_watch();
}

/* ---------------- firmware restart gate ---------------- */

bool vm_busy(char *why, size_t cap)
{
    static const char *const mode_name[4] = { "", "Modbus", "schedule", "manual" };
    if (CurrentMode >= 1 && CurrentMode <= 3) {
        snprintf(why, cap, "Valve Manager %s mode is running", mode_name[CurrentMode]);
        return true;
    }
    if (CurrentModeMUX >= 1 && CurrentModeMUX <= 3) {
        snprintf(why, cap, "Multiplexor %s mode is running", mode_name[CurrentModeMUX]);
        return true;
    }
    if (PurgeInletActivate) { snprintf(why, cap, "a purge is running"); return true; }
    return false;
}

bool vm_fw_safe(char *why, size_t cap) { return !vm_busy(why, cap); }

/* ---------------- everything for plat_start ---------------- */

const plat_app_t *vmmux_app(void)
{
    static plat_app_t a;
    if (a.scan == NULL) {
        a.scan = &vmmux_scan;
        a.params = vm_params;          a.n_params = vm_n_params;
        a.retain = vm_retain;          a.n_retain = vm_n_retain;
        a.layout_version = 1;          a.migrate = NULL;
        a.modbus_map = vm_modbus;      a.n_modbus_map = vm_n_modbus;
        a.ident = (mb_ident_t){ .vendor = "CEMS", .product = "CEMS Supervisor Valve Manager / Multiplexor",
                                .revision = VM_APP_REVISION, .write_level = PLAT_LVL_OPERATOR };
        a.log_chans = vm_log_chans;    a.n_log_chans = vm_n_log_chans;
        a.ui_home_ids = vm_home_ids;   a.n_ui_home = vm_n_home;
        a.alarms = vm_alarm_defs;      a.n_alarms = vm_n_alarms;
        a.fw_safe = vm_fw_safe;
    }
    return &a;
}
