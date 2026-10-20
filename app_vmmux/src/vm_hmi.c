/* vm_hmi.c - what the old panel's screens did to the logic, as parameter-table entries, so the
 * new touch UI, the web page and the tests drive the program exactly as the old buttons did.
 *
 * The old screens were not just displays (50_HMI_Map): buttons set, reset and toggled tags, and
 * number boxes set an "...Entered" bit when a value was typed (EntryOK actions, hmi_elements.csv).
 * The logic depends on both. This file reproduces them:
 *
 * 1. Commands (IDs 5000+). Writing one = pressing that button once. The press is checked at
 *    apply time against the button's own enable / visibility condition (refused with
 *    PERR_INTERLOCK when the old button was disabled or hidden) and carried out at the start of
 *    the next logic pass, i.e. between scans, as the panel did. Each command lists the old
 *    element and its actions; they are copied from hmi_elements.csv (screens Main_1_19 or 2,
 *    ManualMode, SequencesAbsent, IncorrectSequences, MUX_Main1_10 and their MUX popups).
 *    Commands that start valve movement are PF_CONFIRM (the touchscreen asks first).
 * 2. Entry emulation. When a setting with an EntryOK action changes from outside (touch, web,
 *    Modbus), its "...Entered" bit is set before the logic runs, as if typed on the panel.
 * 3. Status (IDs 5100+): the read-only tags the screens showed (button states, popups, enables,
 *    valve states), for the UI and the web page.
 *
 * Not reproduced (screen navigation only, replaced by the new UI): LoadScreen actions, Settings
 * and password popups, "Switch to VM / MUX". The sequence and schedule editors come with the
 * application-screen API (INTEGRATION.md R2). */
#include <string.h>
#include "vm_app.h"
#include "vm_tags.h"
#include "vm_params.h"
#include "vm_hmi.h"

/* HMI-only tags of the old project (data dictionary role HMI-ONLY): the state of the mode
 * buttons and the manual-mode popups. No logic reads them; the panel did. */
uint8_t ManualMode, ManualModeMUX;
bool    ResetManualAttemptMUX, StartButtonVisibMUX;

/* one byte per command: the value written (1 = press; for the valve commands, the valve 1..19) */
static uint8_t cmd[VM_CMD_COUNT];

static bool idx_ok(plat_val_t v) { return v.u >= 1u && v.u <= 19u; }

/* Can the press happen now? (the old element's enable / visible condition) */
static bool allowed(unsigned c, plat_val_t v)
{
    switch (c) {
    case VMC_VM_MODBUS:      return ModbusModeEnable;
    case VMC_VM_SCHEDULE:    return SchedulerModeEnable;
    case VMC_VM_MANUAL:      return ManualModeEnable;
    case VMC_VM_MANUAL_YES:
    case VMC_VM_MANUAL_NO:   return ResetManualAttempt;                 /* popup shown */
    case VMC_VM_MANUAL_START: return StartButtonVisib;                  /* Start button visible */
    case VMC_VM_INLET:       return idx_ok(v) && EnableInletValve[v.u - 1u];
    case VMC_VM_OUTLET:      return idx_ok(v) && EnableOutletValve[v.u - 1u];
    case VMC_VM_SEQ_ABSENT_OK: return SequenceAbsent;
    case VMC_VM_INCORRECT_OK:  return YouHaveIncorrectInletOutletConfig;
    case VMC_MUX_MODBUS:     return ModbusModeEnableMUX;
    case VMC_MUX_SCHEDULE:   return SchedulerModeEnableMUX;
    case VMC_MUX_MANUAL:     return ManualModeEnableMUX;
    case VMC_MUX_MANUAL_YES:
    case VMC_MUX_MANUAL_NO:  return ResetManualAttemptMUX;
    case VMC_MUX_MANUAL_START: return StartButtonVisibMUX;
    case VMC_MUX_INLET:      return idx_ok(v) && EnableSingulManualSelection && SingularInletVisib[v.u - 1u];
    case VMC_MUX_PURGE:      return EnableSingulManualSelection;
    case VMC_MUX_SEQ_ABSENT_OK: return SequenceAbsentMUX;
    case VMC_MUX_INCORRECT_OK:  return YouHaveIncorrectInletOutletConfigMUX;
    default:                 return false;
    }
}

static int interlock(uint16_t id, plat_val_t v)
{
    unsigned c = (unsigned)(id - VM_CMD_FIRST);
    if (c >= VM_CMD_COUNT || v.u == 0u) return 1;
    return allowed(c, v) ? 0 : 1;
}

/* The old element's actions (hmi_elements.csv "actions" column), in their order. */
static void press(unsigned c, uint8_t v)
{
    unsigned i = (unsigned)v - 1u;
    switch (c) {
    case VMC_VM_MODBUS:      /* StateMachineButton "Modbus Mode", state ModbusModeState */
        if (ModbusModeState == 0) { StartModbus = true;  ModbusModeState = 1; }
        else                      { StartModbus = false; ModbusModeState = 0; }
        break;
    case VMC_VM_SCHEDULE:    /* "Scheduler", state ScheduleButtonState */
        if (ScheduleButtonState == 0) { StartSchedule = true;  ScheduleButtonState = 1; }
        else                          { StartSchedule = false; ScheduleButtonState = 0; }
        break;
    case VMC_VM_MANUAL:      /* "Manual Mode", state ManualMode */
        if (ManualMode == 0) ResetManualAttempt = true;                   /* opens the ManualMode popup */
        else { ManualModeStart = false; ManualMode = 0; }
        break;
    case VMC_VM_MANUAL_YES:  StartButtonVisib = true; ResetManualAttempt = false; break;
    case VMC_VM_MANUAL_NO:   ResetManualAttempt = false; break;
    case VMC_VM_MANUAL_START: ManualModeStart = true; ManualMode = 1; StartButtonVisib = false; break;
    case VMC_VM_INLET:       InletValveTouch[i] = true; break;            /* SetBit; FB_ValveSelectionInter consumes it */
    case VMC_VM_OUTLET:      OutletValveTouch[i] = true; break;
    case VMC_VM_SEQ_ABSENT_OK: SequenceAbsent = false; break;             /* "Cancel" */
    case VMC_VM_INCORRECT_OK:  YouHaveIncorrectInletOutletConfig = false; break;
    case VMC_MUX_MODBUS:     /* "Modbus mode", state ModbusModeStateMUX */
        if (ModbusModeStateMUX == 0) { StartModbusMUX = true;  ModbusModeStateMUX = 1; }
        else                         { StartModbusMUX = false; ModbusModeStateMUX = 0; }
        break;
    case VMC_MUX_SCHEDULE:   /* "Scheduler", state ScheduleButtonStateMUX */
        if (ScheduleButtonStateMUX == 0) { StartScheduleMUX = true;  ScheduleButtonStateMUX = 1; }
        else                             { StartScheduleMUX = false; ScheduleButtonStateMUX = 0; }
        break;
    case VMC_MUX_MANUAL:     /* "Manual mode", state ManualModeMUX */
        if (ManualModeMUX == 0) ResetManualAttemptMUX = true;
        else { ManualModeStartMUX = false; ManualModeMUX = 0; }
        break;
    case VMC_MUX_MANUAL_YES: StartButtonVisibMUX = true; ResetManualAttemptMUX = false; EnableSingulManualSelection = true; break;
    case VMC_MUX_MANUAL_NO:  ResetManualAttemptMUX = false; break;
    case VMC_MUX_MANUAL_START: ManualModeStartMUX = true; ManualModeMUX = 1; StartButtonVisibMUX = false; break;
    case VMC_MUX_INLET:      SingularInletActivate[i] = !SingularInletActivate[i]; break;   /* ToggleBit */
    case VMC_MUX_PURGE:      PurgeInletActivate = !PurgeInletActivate; break;
    case VMC_MUX_SEQ_ABSENT_OK: SequenceAbsentMUX = false; break;
    case VMC_MUX_INCORRECT_OK:  YouHaveIncorrectInletOutletConfigMUX = false; break;
    default: break;
    }
}

/* ---- entry emulation: settings whose number box set an "...Entered" bit on EntryOK ---- */
typedef struct { int16_t *v; bool *flag; int16_t last; } entry_t;
static entry_t entries[] = {
    { &MaxNumberOfValves,   &MaxNumberEntered, 0 },
    { &NumInterconnections, &NumInterconnectionsEntried, 0 },
    { &FirstInlets,         &Tube1InletEntered, 0 },
    { &FirstOutlets,        &Tube1OutletEntered, 0 },
    { &SecondInlets,        &Tube2InletEntered, 0 },
    { &SecondOutlets,       &Tube2OutletEntered, 0 },
    { &NumberOfMUXValves,   &NumberOfMUXVavlesEntered, 0 },
};
#define N_ENTRIES (sizeof entries / sizeof entries[0])
static char last_in[19][11], last_out[19][11];     /* InletName / OutletName -> ChangeModbusNamesVM */
static bool primed;

static void remember(void)
{
    for (size_t k = 0; k < N_ENTRIES; k++) entries[k].last = *entries[k].v;
    memcpy(last_in, InletName, sizeof last_in); memcpy(last_out, OutletName, sizeof last_out);
    primed = true;
}

void vm_hmi_pre_logic(void)
{
    if (!primed) remember();
    for (size_t k = 0; k < N_ENTRIES; k++)
        if (*entries[k].v != entries[k].last) *entries[k].flag = true;
    if (memcmp(last_in, InletName, sizeof last_in) || memcmp(last_out, OutletName, sizeof last_out))
        ChangeModbusNamesVM = true;
    for (unsigned c = 0; c < VM_CMD_COUNT; c++)
        if (cmd[c] != 0) { uint8_t v = cmd[c]; cmd[c] = 0; press(c, v); }
}

void vm_hmi_post_logic(void) { remember(); }   /* the logic's own changes are not "typed" */

/* ---- parameter entries ---- */
#define CMD(ID, NAME, MAX, FL) { .id = (ID), .ctype = CT_U8, .ptype = PT_UINT, .flags = (FL), \
    .level = PLAT_LVL_OPERATOR, .ptr = &cmd[(ID) - VM_CMD_FIRST], .vmin.u = 0, .vmax.u = (MAX), \
    .name = (NAME), .interlock = interlock }
#define STB(ID, NAME, VAR)  PARAM_BIT(ID, NAME, VAR, PLAT_LVL_VIEW, PF_RO)
#define STU(ID, NAME, VAR, CTY) PARAM_UINT(ID, NAME, VAR, CTY, 0u, 255u, 0u, PLAT_LVL_VIEW, PF_RO, NULL)

static const param_def_t defs[] = {
    CMD(VM_CMD_FIRST + VMC_VM_MODBUS,        "vm_btn_modbus",       1u,  PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_VM_SCHEDULE,      "vm_btn_schedule",     1u,  PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_VM_MANUAL,        "vm_btn_manual",       1u,  0),
    CMD(VM_CMD_FIRST + VMC_VM_MANUAL_YES,    "vm_manual_yes",       1u,  0),
    CMD(VM_CMD_FIRST + VMC_VM_MANUAL_NO,     "vm_manual_no",        1u,  0),
    CMD(VM_CMD_FIRST + VMC_VM_MANUAL_START,  "vm_manual_start",     1u,  PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_VM_INLET,         "vm_inlet_touch",      19u, PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_VM_OUTLET,        "vm_outlet_touch",     19u, PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_VM_SEQ_ABSENT_OK, "vm_seq_absent_ok",    1u,  0),
    CMD(VM_CMD_FIRST + VMC_VM_INCORRECT_OK,  "vm_seq_incorrect_ok", 1u,  0),
    CMD(VM_CMD_FIRST + VMC_MUX_MODBUS,       "mux_btn_modbus",      1u,  PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_MUX_SCHEDULE,     "mux_btn_schedule",    1u,  PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_MUX_MANUAL,       "mux_btn_manual",      1u,  0),
    CMD(VM_CMD_FIRST + VMC_MUX_MANUAL_YES,   "mux_manual_yes",      1u,  0),
    CMD(VM_CMD_FIRST + VMC_MUX_MANUAL_NO,    "mux_manual_no",       1u,  0),
    CMD(VM_CMD_FIRST + VMC_MUX_MANUAL_START, "mux_manual_start",    1u,  PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_MUX_INLET,        "mux_inlet_toggle",    19u, PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_MUX_PURGE,        "mux_purge_toggle",    1u,  PF_CONFIRM),
    CMD(VM_CMD_FIRST + VMC_MUX_SEQ_ABSENT_OK, "mux_seq_absent_ok",  1u,  0),
    CMD(VM_CMD_FIRST + VMC_MUX_INCORRECT_OK, "mux_seq_incorrect_ok", 1u, 0),

    /* status the old screens showed */
    STU(VMS_VM_MODBUS_STATE,    "vm_btn_modbus_state",    ModbusModeState, CT_U8),
    STU(VMS_VM_SCHEDULE_STATE,  "vm_btn_schedule_state",  ScheduleButtonState, CT_U8),
    STU(VMS_VM_MANUAL_STATE,    "vm_btn_manual_state",    ManualMode, CT_U8),
    STB(VMS_VM_MODBUS_EN,       "vm_btn_modbus_en",       ModbusModeEnable),
    STB(VMS_VM_SCHEDULE_EN,     "vm_btn_schedule_en",     SchedulerModeEnable),
    STB(VMS_VM_MANUAL_EN,       "vm_btn_manual_en",       ManualModeEnable),
    STB(VMS_VM_MANUAL_POPUP,    "vm_manual_popup",        ResetManualAttempt),
    STB(VMS_VM_START_VISIBLE,   "vm_start_visible",       StartButtonVisib),
    STB(VMS_VM_SEQ_ABSENT,      "vm_seq_absent_popup",    SequenceAbsent),
    STB(VMS_VM_INCORRECT,       "vm_seq_incorrect_popup", YouHaveIncorrectInletOutletConfig),
    PARAM_ARRAY_BOOL(VMS_VM_INLET_EN,  "vm_inlet_touch_en",  EnableInletValve,  PLAT_LVL_VIEW, PF_RO),
    PARAM_ARRAY_BOOL(VMS_VM_OUTLET_EN, "vm_outlet_touch_en", EnableOutletValve, PLAT_LVL_VIEW, PF_RO),
    STU(VMS_MUX_MODBUS_STATE,   "mux_btn_modbus_state",   ModbusModeStateMUX, CT_U8),
    STU(VMS_MUX_SCHEDULE_STATE, "mux_btn_schedule_state", ScheduleButtonStateMUX, CT_U8),
    STU(VMS_MUX_MANUAL_STATE,   "mux_btn_manual_state",   ManualModeMUX, CT_U8),
    STB(VMS_MUX_MODBUS_EN,      "mux_btn_modbus_en",      ModbusModeEnableMUX),
    STB(VMS_MUX_SCHEDULE_EN,    "mux_btn_schedule_en",    SchedulerModeEnableMUX),
    STB(VMS_MUX_MANUAL_EN,      "mux_btn_manual_en",      ManualModeEnableMUX),
    STB(VMS_MUX_MANUAL_POPUP,   "mux_manual_popup",       ResetManualAttemptMUX),
    STB(VMS_MUX_START_VISIBLE,  "mux_start_visible",      StartButtonVisibMUX),
    STB(VMS_MUX_SELECT_EN,      "mux_inlet_select_en",    EnableSingulManualSelection),
    STB(VMS_MUX_SEQ_ABSENT,     "mux_seq_absent_popup",   SequenceAbsentMUX),
    STB(VMS_MUX_INCORRECT,      "mux_seq_incorrect_popup", YouHaveIncorrectInletOutletConfigMUX),
    PARAM_ARRAY_BOOL(VMS_MUX_INLET_VISIBLE, "mux_inlet_visible", SingularInletVisib, PLAT_LVL_VIEW, PF_RO),
    PARAM_ARRAY_BOOL(VMS_MUX_INLET_ON,      "mux_inlet_on",      SingularInletActivate, PLAT_LVL_VIEW, PF_RO),
    STB(VMS_MUX_PURGE_ON,       "mux_purge_on",           PurgeInletActivate),
};
const param_def_t *const vm_hmi_params = defs;
const size_t vm_n_hmi_params = sizeof defs / sizeof defs[0];
