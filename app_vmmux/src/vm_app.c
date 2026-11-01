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
#include "vm_hmi.h"
#include "vm_ui.h"
#include "vm_params_modbus.h"
#include "plat_state.h"

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
    vm_ui_pre_logic();         /* Touched_Bit from the panel's last touch (R3) */
    vm_hmi_pre_logic();        /* the old panel's button presses and EntryOK bits, between scans */
    remote_link_pre_logic();   /* Q1: may end a VM Modbus run before Function1 sees it */
    Function1();
    vm_hmi_post_logic();          /* VM top level; calls MUX at rung 36 */
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

/* Operating state and measurement quality (plat_state.h, CIP-09/10). The VM/MUX switches sample
 * routes for an analyser and watches the sample system (flow, heated line) all the time; it has
 * no warm-up. So it reports MEASURING from the first scan: in READY the platform marks every
 * reading unavailable, which would hide the live flow and temperatures between runs. Whether a
 * run is on is shown by the screens and Modbus as before; the firmware-restart gate (vm_busy)
 * still refuses a restart during a run. Quality of the four measured values (vm_meas): unavailable when
 * the channel is disabled, fault when the input reports a fault; the platform adds stale,
 * maintenance, calibration and fault states on top. */
const plat_meas_def_t vm_meas[] = {
    { .id = VMMB_ANALOGSENSOR,      .index = 0, .max_age_ms = 1000 },
    { .id = VMMB_ANALOGSENSOR,      .index = 1, .max_age_ms = 1000 },
    { .id = VMMB_TEMPERATURESENSOR, .index = 0, .max_age_ms = 1000 },   /* 0.1 degree */
    { .id = VMMB_TEMPERATURESENSOR, .index = 1, .max_age_ms = 1000 },
};
const size_t vm_n_meas = sizeof vm_meas / sizeof vm_meas[0];

static uint8_t meas_quality(uint8_t plat_status, bool enabled)
{
    if (!enabled) return PLAT_Q_UNAVAILABLE;
    return plat_status == PLAT_AIN_OK ? PLAT_Q_VALID : PLAT_Q_FAULT;
}

static void report_state(void)
{
    plat_app_state(PLAT_ST_MEASURING);
    plat_meas_update(0, meas_quality(vm_chan_ai_status[0], AnalogEnable[0]));
    plat_meas_update(1, meas_quality(vm_chan_ai_status[1], AnalogEnable[1]));
    plat_meas_update(2, meas_quality(vm_chan_rtd_status[0], TemperatureEnable[0]));
    plat_meas_update(3, meas_quality(vm_chan_rtd_status[1], TemperatureEnable[1]));
}

void vm_after_logic(void)
{
    vm_alarms_scan();
    vm_compat_scan();
    vm_ui_scan();
    log_channels();
    report_state();
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
        /* the generated table plus the screen-action entries (vm_hmi.c), one array */
        static param_def_t all[VM_APP_PARAMS_CAP];   /* not PARAM_MAX: that counts the platform's too */
        size_t n = 0;
        for (size_t k = 0; k < vm_n_params && n < VM_APP_PARAMS_CAP; k++) all[n++] = vm_params[k];
        for (size_t k = 0; k < vm_n_hmi_params && n < VM_APP_PARAMS_CAP; k++) all[n++] = vm_hmi_params[k];
        for (size_t k = 0; k < vm_n_ui_params && n < VM_APP_PARAMS_CAP; k++) all[n++] = vm_ui_params[k];
        if (n != vm_n_params + vm_n_hmi_params + vm_n_ui_params) n = PARAM_MAX + 1u;  /* param_init refuses it */
        a.params = all;                a.n_params = n;
        a.retain = vm_retain;          a.n_retain = vm_n_retain;
        a.layout_version = 1;          a.migrate = NULL;
        a.modbus_map = vm_modbus;      a.n_modbus_map = vm_n_modbus;
        a.ident = (mb_ident_t){ .vendor = "CEMS", .product = "CEMS Supervisor Valve Manager / Multiplexor",
                                .revision = VM_APP_REVISION, .write_level = PLAT_LVL_OPERATOR };
        a.log_chans = vm_log_chans;    a.n_log_chans = vm_n_log_chans;
        a.ui_home_ids = vm_home_ids;   a.n_ui_home = vm_n_home;
        a.alarms = vm_alarm_defs;      a.n_alarms = vm_n_alarms;
        a.fw_safe = vm_fw_safe;
        a.ui = &vm_ui;                 /* screens and record editors (vm_ui.c) */
        a.meas = vm_meas;              a.n_meas = vm_n_meas;   /* AI1, AI2, RTD1, RTD2 with quality */
    }
    return &a;
}
