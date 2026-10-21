/* vm_ui.c - the VM/MUX screens and record editors, described by parameter ID for the
 * platform (ui_app.h, decision D26), from docs/SCREENS.md. The platform draws them the same
 * way on the touchscreen (as tabs before its own Status screen) and on the web page, and does
 * every write through the parameter table (levels, LOCAL/REMOTE, PF_CONFIRM, interlocks).
 *
 * Seven tabs (the platform shows at most seven):
 *   Valve Mgr    VM mode buttons, dialogs, the valve row (inlets / outlets), time left, readings
 *   Multiplexor  MUX mode buttons, dialogs, the inlet list, purge, time left
 *   Sequences    the record editors (sequences, schedules) and the manual-run timers
 *   Layout       valve counts, interconnections, back-flush, purge output, MUX master enable
 *   Process      flow validation, analyser lock, link-loss close time, feedback settle time
 *   Inputs       analog and temperature channels (array settings shown; edited on the web
 *                settings page until the touchscreen can edit one array element, R6)
 *   Names        valve names (tube positions in use, MUX inlets fitted)
 *
 * The old valve picture was one row of 9 or 19 positions per inlet / outlet line: tube 1 from
 * the left, tube 2 from the right (4F1 FB_ValveSelectionInter). The grids keep that row, so a
 * touch writes the same position the old picture did (vm_inlet_touch / vm_outlet_touch).
 * Item labels are the valve names (InletName0..18, OutletName0..18, SingularInlet0..18, given
 * consecutive IDs for this: tools/param_ids.csv); a blank name shows the position number. */
#include <stdio.h>
#include <string.h>
#include "vm_app.h"
#include "vm_config.h"
#include "vm_tags.h"
#include "vm_params.h"
#include "vm_params_modbus.h"
#include "vm_alarms.h"
#include "vm_hmi.h"
#include "vm_ui.h"
#include "bsp.h"

/* ---------------- display values ---------------- */

static uint32_t left_s[3];
static bool     in_shown[19], in_open[19], out_shown[19], out_open[19];
static uint8_t  positions;
static float    rtd_deg[2];

#define TOUCH_HOLD_MS 1000u   /* Touched_Bit stays on this long after a touch */

static void route_seen(void);
void vm_ui_pre_logic(void)
{
    vm_do_assign_hook = route_seen;
    uint32_t t = ui_last_touch_ms;
    Touched_Bit = t != 0u && (uint32_t)(bsp_ms() - t) < TOUCH_HOLD_MS;
}

/* The route DO_Assign (4C) drove this scan, seen through its hook (I-13): the selections
 * themselves are cleared at the end of every scan (Function1 R37, V21). */
static int16_t seen[4];   /* first inlet, first outlet, second inlet, second outlet (1-based, 0 = none) */
static void route_seen(void)
{
    seen[0] = SelectedFirstInlet;  seen[1] = SelectedFirstOutlet;
    seen[2] = SelectedSecondInlet; seen[3] = SelectedSecondOutlet;
}

/* one line (inlets or outlets) of the old picture: tube 1 from the left, tube 2 from the right */
static void valve_line(int16_t n1, int16_t n2, bool two, int16_t sel1, int16_t sel2, bool shown[19], bool open[19])
{
    for (int i = 0; i < 19; i++) {
        bool t1 = i < positions && i < n1;
        bool t2 = two && i < positions && i >= positions - n2;
        shown[i] = t1 || t2;
        open[i]  = (t1 && sel1 == i + 1) || (t2 && sel2 >= 1 && i == positions - sel2);
    }
}

void vm_ui_scan(void)
{
    left_s[0] = ProcessTimer.Preset / 1000u;    /* (F8 fixed in vm_compat.c, which runs first) */
    left_s[1] = ProcessTimer2.Preset / 1000u;
    left_s[2] = ProcessTimerMUX.Preset / 1000u;

    bool two = NumInterconnections == 2;
    int ins = two ? FirstInlets + SecondInlets : FirstInlets;
    int outs = two ? FirstOutlets + SecondOutlets : FirstOutlets;
    positions = (ins <= 9 && outs <= 9) ? 9u : 19u;     /* 4F1 lines 20-33 */
    valve_line(FirstInlets, SecondInlets, two, seen[0], seen[2], in_shown, in_open);
    valve_line(FirstOutlets, SecondOutlets, two, seen[1], seen[3], out_shown, out_open);

    rtd_deg[0] = TemperatureSensor[0] / 10.0f;
    rtd_deg[1] = TemperatureSensor[1] / 10.0f;
}

#define ROF (PF_RO | PF_NO_MODBUS)   /* display only: read-only, never on Modbus */
static const param_def_t defs[] = {
    PARAM_UINT(VMU_LEFT1_S,   "vm_time_left_s",   left_s[0], CT_U32, 0u, 4294967295u, 0u, PLAT_LVL_VIEW, ROF, "s"),
    PARAM_UINT(VMU_LEFT2_S,   "vm_time_left2_s",  left_s[1], CT_U32, 0u, 4294967295u, 0u, PLAT_LVL_VIEW, ROF, "s"),
    PARAM_UINT(VMU_LEFTMUX_S, "mux_time_left_s",  left_s[2], CT_U32, 0u, 4294967295u, 0u, PLAT_LVL_VIEW, ROF, "s"),
    PARAM_ARRAY_BOOL(VMU_IN_SHOWN,  "vm_inlet_shown",  in_shown,  PLAT_LVL_VIEW, ROF),
    PARAM_ARRAY_BOOL(VMU_IN_OPEN,   "vm_inlet_open",   in_open,   PLAT_LVL_VIEW, ROF),
    PARAM_ARRAY_BOOL(VMU_OUT_SHOWN, "vm_outlet_shown", out_shown, PLAT_LVL_VIEW, ROF),
    PARAM_ARRAY_BOOL(VMU_OUT_OPEN,  "vm_outlet_open",  out_open,  PLAT_LVL_VIEW, ROF),
    PARAM_UINT(VMU_POSITIONS, "vm_positions", positions, CT_U8, 0u, 19u, 0u, PLAT_LVL_VIEW, ROF, NULL),
    PARAM_REAL(VMU_RTD1, "rtd1_deg", rtd_deg[0], -3.4e38f, 3.4e38f, 0.0f, "deg", PLAT_LVL_VIEW, ROF),
    PARAM_REAL(VMU_RTD2, "rtd2_deg", rtd_deg[1], -3.4e38f, 3.4e38f, 0.0f, "deg", PLAT_LVL_VIEW, ROF),
};
const param_def_t *const vm_ui_params = defs;
const size_t vm_n_ui_params = sizeof defs / sizeof defs[0];

/* ---------------- record editors ---------------- */

static const char *const month_names[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const day_names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
/* MUX SingulValves is a 0-based inlet index (4E); shown 1-based */
static const char *const inlet_numbers[19] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10",
                                               "11", "12", "13", "14", "15", "16", "17", "18", "19" };
#define DAY_MS 86400000

#define SEQ_FIELDS(INLETS, OUTLETS) \
    { .label = "Name",   .kind = RF_TEXT,    .off = offsetof(Sequences_t, Name),    .stride = 13, .size = 13 }, \
    { .label = "Time",   .kind = RF_MS_AS_S, .off = offsetof(Sequences_t, Timers),  .stride = 4, .min = 0, .max = DAY_MS, .unit = "s" }, \
    { .label = "Inlet",  .kind = RF_UINT, .ctype = CT_U8, .off = offsetof(Sequences_t, Inlets),  .stride = 1, .min = 1, .max = 19, .max_param = (INLETS) }, \
    { .label = "Outlet", .kind = RF_UINT, .ctype = CT_U8, .off = offsetof(Sequences_t, Outlets), .stride = 1, .min = 1, .max = 19, .max_param = (OUTLETS) }
static const ui_field_t seq1_fields[] = { SEQ_FIELDS(VMP_FIRSTINLETS, VMP_FIRSTOUTLETS) };
static const ui_field_t seq2_fields[] = { SEQ_FIELDS(VMP_SECONDINLETS, VMP_SECONDOUTLETS) };

static const ui_field_t mux_fields[] = {
    { .label = "Inlet",     .kind = RF_ENUM, .ctype = CT_U8, .off = offsetof(ScheduleMUX_t, SingulValves), .stride = 1,
      .min = 0, .max = 18, .items = inlet_numbers, .n_items = 19 },
    { .label = "Dwell",     .kind = RF_MS_AS_S, .off = offsetof(ScheduleMUX_t, DwellTimer),        .stride = 4, .min = 0, .max = DAY_MS, .unit = "s" },
    { .label = "Sample",    .kind = RF_MS_AS_S, .off = offsetof(ScheduleMUX_t, SampleTimer),       .stride = 4, .min = 0, .max = DAY_MS, .unit = "s" },
    { .label = "Pre-pump",  .kind = RF_BOOL,    .off = offsetof(ScheduleMUX_t, FastLoopPump),      .stride = 1 },
    { .label = "Pump lead", .kind = RF_MS_AS_S, .off = offsetof(ScheduleMUX_t, FastLoopPumpTimer), .stride = 4, .min = 0, .max = DAY_MS, .unit = "s" },
};

static const ui_field_t sched_fields[] = {
    { .label = "From year", .kind = RF_UINT, .ctype = CT_U16, .off = offsetof(Schedule_t, From_Year), .min = 0, .max = 2099 },
    { .label = "To year",   .kind = RF_UINT, .ctype = CT_U16, .off = offsetof(Schedule_t, To_Year),   .min = 0, .max = 2099 },
    { .label = "Months",    .kind = RF_BOOL, .off = offsetof(Schedule_t, Month),        .count = 12, .items = month_names, .n_items = 12 },
    { .label = "Days",      .kind = RF_BOOL, .off = offsetof(Schedule_t, Day_Of_Month), .count = 31 },
    { .label = "Weekdays",  .kind = RF_BOOL, .off = offsetof(Schedule_t, Day_Of_Week),  .count = 7, .items = day_names, .n_items = 7 },
    { .label = "Start",     .kind = RF_HMS,  .off = offsetof(Schedule_t, From_Hour) },
    { .label = "End",       .kind = RF_HMS,  .off = offsetof(Schedule_t, To_Hour) },
};

#define N(A) (uint8_t)(sizeof A / sizeof A[0])
static const ui_record_t records[] = {
    { .param_id = VMP_SEQUENCE1, .title = "VM sequence 1", .rows = 20,
      .rows_off = offsetof(Sequences_t, NumberOfPeriods), .rows_ctype = CT_U8, .fields = seq1_fields, .n_fields = N(seq1_fields) },
    { .param_id = VMP_SEQUENCE2, .title = "VM sequence 2", .rows = 20,
      .rows_off = offsetof(Sequences_t, NumberOfPeriods), .rows_ctype = CT_U8, .fields = seq2_fields, .n_fields = N(seq2_fields) },
    { .param_id = VMP_SEQUENCEMUX, .title = "MUX channels", .rows = 20,
      .rows_off = offsetof(ScheduleMUX_t, NumberOfPeriods), .rows_ctype = CT_U8, .fields = mux_fields, .n_fields = N(mux_fields) },
    { .param_id = VMP_SCHEDULE1, .title = "VM schedule 1", .fields = sched_fields, .n_fields = N(sched_fields) },
    { .param_id = VMP_SCHEDULE2, .title = "VM schedule 2", .fields = sched_fields, .n_fields = N(sched_fields) },
#if !VM_MUX_CONTINUOUS
    { .param_id = VMP_SCHEDULEMUX, .title = "MUX schedule", .fields = sched_fields, .n_fields = N(sched_fields) },
#endif
};

/* ---------------- screens ---------------- */

#define SECTION(L)              { .kind = UW_SECTION, .label = (L) }
#define VAL(L, ID)              { .kind = UW_VALUE, .label = (L), .value_id = (ID), .index = UW_NO_INDEX }
#define VAL_IF(L, ID, S, M)     { .kind = UW_VALUE, .label = (L), .value_id = (ID), .index = UW_NO_INDEX, .show_id = (S), .show_min = (M) }
#define ELEM(L, ID, I)          { .kind = UW_VALUE, .label = (L), .value_id = (ID), .index = (I) }
#define BTN(L, CMD, ST, EN, S)  { .kind = UW_BUTTON, .label = (L), .cmd_id = (CMD), .state_id = (ST), .enable_id = (EN), .show_id = (S) }
#define ASK(L, S, YES, NO)      { .kind = UW_DIALOG, .label = (L), .show_id = (S), .cmd_id = (YES), .cmd2_id = (NO) }
#define REC(L, ID, S, M)        { .kind = UW_RECORD, .label = (L), .record_id = (ID), .show_id = (S), .show_min = (M) }
#define CMD(i) (VM_CMD_FIRST + (i))
#define MUXEN VM_PARAM_MUX_ENABLE

static const ui_widget_t w_vm[] = {
    SECTION("Valve Manager"),
    BTN("Modbus mode", CMD(VMC_VM_MODBUS),   VMS_VM_MODBUS_STATE,   VMS_VM_MODBUS_EN,   0),
    BTN("Scheduler",   CMD(VMC_VM_SCHEDULE), VMS_VM_SCHEDULE_STATE, VMS_VM_SCHEDULE_EN, 0),
    BTN("Manual mode", CMD(VMC_VM_MANUAL),   VMS_VM_MANUAL_STATE,   VMS_VM_MANUAL_EN,   0),
    BTN("Start",       CMD(VMC_VM_MANUAL_START), 0, 0, VMS_VM_START_VISIBLE),
    VAL("Time left", VMU_LEFT1_S),
    VAL_IF("Time left, tube 2", VMU_LEFT2_S, VMP_NUMINTERCONNECTIONS, 2),
    { .kind = UW_GRID, .label = "Inlets", .show_id = VMU_IN_SHOWN, .state_id = VMU_IN_OPEN,
      .enable_id = VMS_VM_INLET_EN, .cmd_id = CMD(VMC_VM_INLET), .names_id = VMP_INLETNAME0 },
    { .kind = UW_GRID, .label = "Outlets", .show_id = VMU_OUT_SHOWN, .state_id = VMU_OUT_OPEN,
      .enable_id = VMS_VM_OUTLET_EN, .cmd_id = CMD(VMC_VM_OUTLET), .names_id = VMP_OUTLETNAME0 },
    ASK("Manual mode: valves are opened by touching them. Continue?", VMS_VM_MANUAL_POPUP,
        CMD(VMC_VM_MANUAL_YES), CMD(VMC_VM_MANUAL_NO)),
    ASK("No sequence is set up for the scheduler", VMS_VM_SEQ_ABSENT, CMD(VMC_VM_SEQ_ABSENT_OK), 0),
    ASK("A sequence uses a valve that is not fitted", VMS_VM_INCORRECT, CMD(VMC_VM_INCORRECT_OK), 0),
    SECTION("Readings"),
    ELEM("AI1", VMMB_ANALOGSENSOR, 0),
    ELEM("AI2", VMMB_ANALOGSENSOR, 1),
    VAL("RTD1", VMU_RTD1),
    VAL("RTD2", VMU_RTD2),
};

static const ui_widget_t w_mux[] = {
    SECTION("Multiplexor"),
    BTN("Modbus mode", CMD(VMC_MUX_MODBUS),   VMS_MUX_MODBUS_STATE,   VMS_MUX_MODBUS_EN,   MUXEN),
    BTN("Scheduler",   CMD(VMC_MUX_SCHEDULE), VMS_MUX_SCHEDULE_STATE, VMS_MUX_SCHEDULE_EN, MUXEN),
    BTN("Manual mode", CMD(VMC_MUX_MANUAL),   VMS_MUX_MANUAL_STATE,   VMS_MUX_MANUAL_EN,   MUXEN),
    BTN("Start",       CMD(VMC_MUX_MANUAL_START), 0, 0, VMS_MUX_START_VISIBLE),
    VAL_IF("Step time left", VMU_LEFTMUX_S, MUXEN, 1),
    { .kind = UW_GRID, .label = "Inlets", .show_id = VMS_MUX_INLET_VISIBLE, .state_id = VMS_MUX_INLET_ON,
      .enable_id = VMS_MUX_SELECT_EN, .cmd_id = CMD(VMC_MUX_INLET), .names_id = VMP_SINGULARINLET0 },
    BTN("Purge", CMD(VMC_MUX_PURGE), VMS_MUX_PURGE_ON, VMS_MUX_SELECT_EN, MUXEN),
    ASK("Manual mode: inlets are opened by touching them. Continue?", VMS_MUX_MANUAL_POPUP,
        CMD(VMC_MUX_MANUAL_YES), CMD(VMC_MUX_MANUAL_NO)),
    ASK("No channel list is set up for the scheduler", VMS_MUX_SEQ_ABSENT, CMD(VMC_MUX_SEQ_ABSENT_OK), 0),
    ASK("The channel list uses an inlet that is not fitted", VMS_MUX_INCORRECT, CMD(VMC_MUX_INCORRECT_OK), 0),
    VAL("Multiplexor enabled", MUXEN),
};

static const ui_widget_t w_seq[] = {
    SECTION("Valve Manager"),
    REC("Sequence 1", VMP_SEQUENCE1, 0, 0),
    REC("Schedule 1", VMP_SCHEDULE1, 0, 0),
    REC("Sequence 2", VMP_SEQUENCE2, VMP_NUMINTERCONNECTIONS, 2),
    REC("Schedule 2", VMP_SCHEDULE2, VMP_NUMINTERCONNECTIONS, 2),
    SECTION("Multiplexor"),
    REC("Channels", VMP_SEQUENCEMUX, MUXEN, 1),
#if !VM_MUX_CONTINUOUS
    REC("MUX schedule", VMP_SCHEDULEMUX, MUXEN, 1),
#endif
    SECTION("Manual runs"),
    VAL("Manual time, tube 1", VMP_MANUAL_TIMER),
    VAL_IF("Manual time, tube 2", VMP_MANUAL_TIMER2, VMP_NUMINTERCONNECTIONS, 2),
    VAL_IF("Manual time, MUX", VMP_MANUAL_TIMERMUX, MUXEN, 1),
    VAL("Mode at power-up", VMP_DEFAULTMODEONPOWERUP),
};

static const ui_widget_t w_layout[] = {
    SECTION("Valve Manager"),
    VAL("Valve outputs", VMP_MAXNUMBEROFVALVES),
    VAL("Interconnections", VMP_NUMINTERCONNECTIONS),
    VAL("Inlets, tube 1", VMP_FIRSTINLETS),
    VAL("Outlets, tube 1", VMP_FIRSTOUTLETS),
    VAL_IF("Inlets, tube 2", VMP_SECONDINLETS, VMP_NUMINTERCONNECTIONS, 2),
    VAL_IF("Outlets, tube 2", VMP_SECONDOUTLETS, VMP_NUMINTERCONNECTIONS, 2),
    VAL("Back-flush valve", VMP_BACKFLUSHEXISTS),
    VAL_IF("Back-flush output", VMP_BACKFLUSHDONUMBER, VMP_BACKFLUSHEXISTS, 1),
    SECTION("Multiplexor"),
    VAL("Multiplexor enabled", MUXEN),
    VAL_IF("MUX valves", VMP_NUMBEROFMUXVALVES, MUXEN, 1),
    VAL_IF("Purge output", VMP_DOPURGEINLET, MUXEN, 1),
};

static const ui_widget_t w_process[] = {
    SECTION("Flow and analyser"),
    VAL("Flow validation", VMP_FLOWVALIDATION),
    VAL_IF("Flow input", VMP_FLOWANALOGINPUT, VMP_FLOWVALIDATION, 1),
    VAL_IF("No-flow delay", VMP_FLOWVALIDATIONTIMER, VMP_FLOWVALIDATION, 1),
    VAL("Lock valves if analyser not OK", VMP_ENABLELOCKVALVESIFANALYZERNOTOK),
    SECTION("Remote and feedback"),
    VAL("Close VM after link loss", VM_PARAM_LINK_CLOSE_MIN),
    VAL("Valve feedback settle", VM_PARAM_FB_SETTLE_S),
};

static const ui_widget_t w_inputs[] = {
    SECTION("Analog input 1"),
    ELEM("Enabled", VMP_ANALOGENABLE, 0),  ELEM("Low (4 mA)", VMP_FIRSTCALIBPOINT, 0),
    ELEM("High (20 mA)", VMP_SECONDCALIBPOINT, 0),
    ELEM("Alarm", VMP_ANALOGALARMENABLED, 0), ELEM("Range min", VMP_RANGEMIN, 0), ELEM("Range max", VMP_RANGEMAX, 0),
    SECTION("Analog input 2"),
    ELEM("Enabled", VMP_ANALOGENABLE, 1),  ELEM("Low (4 mA)", VMP_FIRSTCALIBPOINT, 1),
    ELEM("High (20 mA)", VMP_SECONDCALIBPOINT, 1),
    ELEM("Alarm", VMP_ANALOGALARMENABLED, 1), ELEM("Range min", VMP_RANGEMIN, 1), ELEM("Range max", VMP_RANGEMAX, 1),
    SECTION("RTD 1 (0.1 degree)"),
    ELEM("Enabled", VMP_TEMPERATUREENABLE, 0), ELEM("Shift", VMP_TEMPERATURESHIFT, 0),
    ELEM("Alarm", VMP_TEMPERATUREALARMENABLED, 0),
    ELEM("Range min", VMP_MINTEMPERATURERANGE, 0), ELEM("Range max", VMP_MAXTEMPERATURERANGE, 0),
    VAL("Alarm delay", VM_PARAM_RTD1_DELAY_S), VAL("Hysteresis", VM_PARAM_RTD1_HYST),
    SECTION("RTD 2 (0.1 degree)"),
    ELEM("Enabled", VMP_TEMPERATUREENABLE, 1), ELEM("Shift", VMP_TEMPERATURESHIFT, 1),
    ELEM("Alarm", VMP_TEMPERATUREALARMENABLED, 1),
    ELEM("Range min", VMP_MINTEMPERATURERANGE, 1), ELEM("Range max", VMP_MAXTEMPERATURERANGE, 1),
    VAL("Alarm delay", VM_PARAM_RTD2_DELAY_S), VAL("Hysteresis", VM_PARAM_RTD2_HYST),
};

/* names: VM positions in use (9 or 19), MUX inlets fitted (valves - 1, the last is the purge) */
static const ui_widget_t w_names[] = {
    SECTION("Valve Manager inlets (tube 2 from the right)"),
    { .kind = UW_VALUE, .label = "Inlet 1",  .value_id = VMP_INLETNAME0,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 1 },
    { .kind = UW_VALUE, .label = "Inlet 2",  .value_id = VMP_INLETNAME1,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 2 },
    { .kind = UW_VALUE, .label = "Inlet 3",  .value_id = VMP_INLETNAME2,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 3 },
    { .kind = UW_VALUE, .label = "Inlet 4",  .value_id = VMP_INLETNAME3,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 4 },
    { .kind = UW_VALUE, .label = "Inlet 5",  .value_id = VMP_INLETNAME4,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 5 },
    { .kind = UW_VALUE, .label = "Inlet 6",  .value_id = VMP_INLETNAME5,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 6 },
    { .kind = UW_VALUE, .label = "Inlet 7",  .value_id = VMP_INLETNAME6,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 7 },
    { .kind = UW_VALUE, .label = "Inlet 8",  .value_id = VMP_INLETNAME7,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 8 },
    { .kind = UW_VALUE, .label = "Inlet 9",  .value_id = VMP_INLETNAME8,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 9 },
    { .kind = UW_VALUE, .label = "Inlet 10", .value_id = VMP_INLETNAME9,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 10 },
    { .kind = UW_VALUE, .label = "Inlet 11", .value_id = VMP_INLETNAME10, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 11 },
    { .kind = UW_VALUE, .label = "Inlet 12", .value_id = VMP_INLETNAME11, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 12 },
    { .kind = UW_VALUE, .label = "Inlet 13", .value_id = VMP_INLETNAME12, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 13 },
    { .kind = UW_VALUE, .label = "Inlet 14", .value_id = VMP_INLETNAME13, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 14 },
    { .kind = UW_VALUE, .label = "Inlet 15", .value_id = VMP_INLETNAME14, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 15 },
    { .kind = UW_VALUE, .label = "Inlet 16", .value_id = VMP_INLETNAME15, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 16 },
    { .kind = UW_VALUE, .label = "Inlet 17", .value_id = VMP_INLETNAME16, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 17 },
    { .kind = UW_VALUE, .label = "Inlet 18", .value_id = VMP_INLETNAME17, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 18 },
    { .kind = UW_VALUE, .label = "Inlet 19", .value_id = VMP_INLETNAME18, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 19 },
    SECTION("Valve Manager outlets (tube 2 from the right)"),
    { .kind = UW_VALUE, .label = "Outlet 1",  .value_id = VMP_OUTLETNAME0,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 1 },
    { .kind = UW_VALUE, .label = "Outlet 2",  .value_id = VMP_OUTLETNAME1,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 2 },
    { .kind = UW_VALUE, .label = "Outlet 3",  .value_id = VMP_OUTLETNAME2,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 3 },
    { .kind = UW_VALUE, .label = "Outlet 4",  .value_id = VMP_OUTLETNAME3,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 4 },
    { .kind = UW_VALUE, .label = "Outlet 5",  .value_id = VMP_OUTLETNAME4,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 5 },
    { .kind = UW_VALUE, .label = "Outlet 6",  .value_id = VMP_OUTLETNAME5,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 6 },
    { .kind = UW_VALUE, .label = "Outlet 7",  .value_id = VMP_OUTLETNAME6,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 7 },
    { .kind = UW_VALUE, .label = "Outlet 8",  .value_id = VMP_OUTLETNAME7,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 8 },
    { .kind = UW_VALUE, .label = "Outlet 9",  .value_id = VMP_OUTLETNAME8,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 9 },
    { .kind = UW_VALUE, .label = "Outlet 10", .value_id = VMP_OUTLETNAME9,  .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 10 },
    { .kind = UW_VALUE, .label = "Outlet 11", .value_id = VMP_OUTLETNAME10, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 11 },
    { .kind = UW_VALUE, .label = "Outlet 12", .value_id = VMP_OUTLETNAME11, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 12 },
    { .kind = UW_VALUE, .label = "Outlet 13", .value_id = VMP_OUTLETNAME12, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 13 },
    { .kind = UW_VALUE, .label = "Outlet 14", .value_id = VMP_OUTLETNAME13, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 14 },
    { .kind = UW_VALUE, .label = "Outlet 15", .value_id = VMP_OUTLETNAME14, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 15 },
    { .kind = UW_VALUE, .label = "Outlet 16", .value_id = VMP_OUTLETNAME15, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 16 },
    { .kind = UW_VALUE, .label = "Outlet 17", .value_id = VMP_OUTLETNAME16, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 17 },
    { .kind = UW_VALUE, .label = "Outlet 18", .value_id = VMP_OUTLETNAME17, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 18 },
    { .kind = UW_VALUE, .label = "Outlet 19", .value_id = VMP_OUTLETNAME18, .index = UW_NO_INDEX, .show_id = VMU_POSITIONS, .show_min = 19 },
    SECTION("Multiplexor inlets"),
    { .kind = UW_VALUE, .label = "MUX inlet 1",  .value_id = VMP_SINGULARINLET0,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 2 },
    { .kind = UW_VALUE, .label = "MUX inlet 2",  .value_id = VMP_SINGULARINLET1,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 3 },
    { .kind = UW_VALUE, .label = "MUX inlet 3",  .value_id = VMP_SINGULARINLET2,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 4 },
    { .kind = UW_VALUE, .label = "MUX inlet 4",  .value_id = VMP_SINGULARINLET3,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 5 },
    { .kind = UW_VALUE, .label = "MUX inlet 5",  .value_id = VMP_SINGULARINLET4,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 6 },
    { .kind = UW_VALUE, .label = "MUX inlet 6",  .value_id = VMP_SINGULARINLET5,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 7 },
    { .kind = UW_VALUE, .label = "MUX inlet 7",  .value_id = VMP_SINGULARINLET6,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 8 },
    { .kind = UW_VALUE, .label = "MUX inlet 8",  .value_id = VMP_SINGULARINLET7,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 9 },
    { .kind = UW_VALUE, .label = "MUX inlet 9",  .value_id = VMP_SINGULARINLET8,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 10 },
    { .kind = UW_VALUE, .label = "MUX inlet 10", .value_id = VMP_SINGULARINLET9,  .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 11 },
    { .kind = UW_VALUE, .label = "MUX inlet 11", .value_id = VMP_SINGULARINLET10, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 12 },
    { .kind = UW_VALUE, .label = "MUX inlet 12", .value_id = VMP_SINGULARINLET11, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 13 },
    { .kind = UW_VALUE, .label = "MUX inlet 13", .value_id = VMP_SINGULARINLET12, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 14 },
    { .kind = UW_VALUE, .label = "MUX inlet 14", .value_id = VMP_SINGULARINLET13, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 15 },
    { .kind = UW_VALUE, .label = "MUX inlet 15", .value_id = VMP_SINGULARINLET14, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 16 },
    { .kind = UW_VALUE, .label = "MUX inlet 16", .value_id = VMP_SINGULARINLET15, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 17 },
    { .kind = UW_VALUE, .label = "MUX inlet 17", .value_id = VMP_SINGULARINLET16, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 18 },
    { .kind = UW_VALUE, .label = "MUX inlet 18", .value_id = VMP_SINGULARINLET17, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 19 },
    { .kind = UW_VALUE, .label = "MUX inlet 19", .value_id = VMP_SINGULARINLET18, .index = UW_NO_INDEX, .show_id = VMP_NUMBEROFMUXVALVES, .show_min = 20 },
};

static const ui_screen_t screens[] = {
    { "Valve Mgr",   w_vm,      N(w_vm) },
    { "Multiplexor", w_mux,     N(w_mux) },
    { "Sequences",   w_seq,     N(w_seq) },
    { "Layout",      w_layout,  N(w_layout) },
    { "Process",     w_process, N(w_process) },
    { "Inputs",      w_inputs,  N(w_inputs) },
    { "Names",       w_names,   N(w_names) },
};

const ui_app_t vm_ui = {
    .screens = screens, .n_screens = sizeof screens / sizeof screens[0],
    .records = records, .n_records = sizeof records / sizeof records[0],
};
