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
#include "vm_alarms.h"

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

static void remote_link_pre_logic(void);

static void vm_logic(void)
{
    remote_link_pre_logic();   /* Q1: may end a VM Modbus run before Function1 sees it */
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

/* Remote-link watchdog (APP_INTEGRATION.md section 4; decision Q1, Gary 20 Oct).
 * The platform reports the link lost when, in REMOTE mode, no Modbus or web request has
 * arrived for comms_timeout (parameter 3). Then:
 *   - Valve Manager: a run started over Modbus (VM ModbusMode, StartModbus) is a one-off
 *     calibration-type run. If the link stays lost for vm_link_close_min minutes (default 30,
 *     0 = never) the run is ended and every valve it opened closes.
 *   - Multiplexor: carries on. It is the automated loop.
 * Ending the run uses the original's own path: StartModbus going FALSE is ModbusMode rung 6's
 * falling edge, which clears the four selections and StartModbusCoil, so Route / DO_Assign
 * drop the valves in the same scan (as when the master or the timer ends a run). */
uint16_t vm_link_close_min = 30;
static uint32_t lost_ms;

static void remote_link_pre_logic(void)
{
    bool lost = param_remote_link_lost();
    static bool was_lost;
    if (lost != was_lost) {
        plat_log_event(lost ? VM_EV_REMOTE_LINK_LOST : VM_EV_REMOTE_LINK_BACK, PLAT_SRC_INTERNAL,
                       PLAT_LVL_VIEW, 0, 0.0, 0.0, lost ? "remote link lost" : "remote link back");
        was_lost = lost;
    }
    if (!lost) { lost_ms = 0; return; }
    if (lost_ms < 0xF0000000u) lost_ms += g_dt_ms;
    if (vm_link_close_min != 0 && StartModbus && lost_ms >= (uint32_t)vm_link_close_min * 60000u) {
        StartModbus = false;
        ModbusModeState = 0;
        plat_log_event(VM_EV_LINK_VM_STOPPED, PLAT_SRC_INTERNAL, PLAT_LVL_VIEW, vm_link_close_min, 0.0, 0.0,
                       "remote link lost: Valve Manager Modbus run ended, valves closed");
    }
}

void vm_after_logic(void)
{
    vm_alarms_scan();
    vm_compat_scan();
    log_channels();
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
