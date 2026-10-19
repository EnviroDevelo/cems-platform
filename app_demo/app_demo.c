/* app_demo.c - see app_demo.h. Tags are plain globals, as in the Stage 4 code. */
#include "app_demo.h"
#include "plat_tasks.h"

/* ---- application tags ---- */
static bool       StartIn;          /* mapped from DI0 */
static bool       ValveOut;         /* mapped to DO0 */
static bool       AckBits[4];       /* like the old acknowledge coils */
static ul_timer_t StartDelay;
static uint32_t   PowerUps;
char              ValveName[11];    /* text, persisted */
uint32_t          RunSeconds;       /* retained, written by the logic */
static uint32_t   run_ms;
static const char valve_name_default[11] = "Valve 1";
static bool       DemoFault;        /* stands in for a fault input: raises a latching alarm */
static float      DemoLevel = 50.0f;/* stands in for a measured value with a range alarm */
static bool       DemoLevelAlarmEn = true;
static uint16_t   DemoLevelText[24];/* editable alarm description (UTF-16, as the old panel's) */

demo_snapshot_t demo_snapshot;

const param_def_t app_demo_params[] = {
    PARAM_UINT(DEMO_PARAM_START_DELAY, "start_delay", StartDelay.Preset, CT_U32, 100, 60000, 2000,
               PLAT_LVL_OPERATOR, PF_PERSIST, "ms"),
    PARAM_UINT(DEMO_PARAM_RUN_HOURS, "run_seconds", RunSeconds, CT_U32, 0, 0xFFFFFFFFu, 0,
               PLAT_LVL_VIEW, PF_PERSIST | PF_RO, "s"),
    PARAM_BIT (DEMO_PARAM_VALVE_OUT, "valve_out", ValveOut, PLAT_LVL_VIEW, PF_RO),
    PARAM_BIT (DEMO_PARAM_START_IN, "start_in", StartIn, PLAT_LVL_VIEW, PF_RO),
    PARAM_TEXT(DEMO_PARAM_VALVE_NAME, "valve_name", ValveName, valve_name_default, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_ARRAY_BOOL(DEMO_PARAM_ACK, "ack", AckBits, PLAT_LVL_OPERATOR, PF_ANY_MODE),
    PARAM_BIT (DEMO_PARAM_FAULT, "demo_fault", DemoFault, PLAT_LVL_OPERATOR, PF_ANY_MODE),
    PARAM_REAL(DEMO_PARAM_LEVEL, "demo_level", DemoLevel, 0.0f, 100.0f, 50.0f, "%", PLAT_LVL_OPERATOR, PF_ANY_MODE),
    PARAM_BIT_D(DEMO_PARAM_LEVEL_AL_EN, "demo_level_alarm", DemoLevelAlarmEn, true, PLAT_LVL_ADMIN, PF_PERSIST | PF_ANY_MODE),
    PARAM_TEXT16(DEMO_PARAM_LEVEL_TEXT, "demo_level_text", DemoLevelText, NULL, PLAT_LVL_ADMIN, PF_PERSIST | PF_ANY_MODE),
};

/* Alarms (IDs 100+). The logic reports conditions; the platform does the rest. */
static const alarm_def_t app_demo_alarms[] = {
    { .id = DEMO_ALARM_FAULT, .severity = AL_SEV_FAULT, .flags = AL_LATCH | AL_NOTIFY, .name = "DEMO_FAULT",
      .text = "Demo fault (latched until acknowledged)" },
    { .id = DEMO_ALARM_LEVEL, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .name = "DEMO_LEVEL_RANGE",
      .text = "Demo level outside 10..90 %", .on_delay_ms = 2000, .off_delay_ms = 2000,
      .enable_param = DEMO_PARAM_LEVEL_AL_EN, .text_param = DEMO_PARAM_LEVEL_TEXT },
};
const size_t app_demo_n_params = sizeof app_demo_params / sizeof app_demo_params[0];

static uint32_t   LifetimeCycles;   /* an internal retained value, not shown anywhere */
const retain_def_t app_demo_retain[] = {
    RETAIN(DEMO_BLOCK_CYCLES, "cycles", LifetimeCycles, 0, NULL),
};
const size_t app_demo_n_retain = sizeof app_demo_retain / sizeof app_demo_retain[0];

/* Modbus: holding 0-1 start delay (U32), 2-3 run seconds (U32), 10-15 valve name,
 * 20 alarm acknowledge; coils 0-3 acknowledge bits; discrete inputs 0 start input,
 * 1 valve output; input registers 0 alarms active, 1 alarms unacknowledged,
 * 10-25 active bits, 30-45 unacknowledged bits (K12: the master sees both). */
const mb_map_t app_demo_modbus[] = {
    MB_ENTRY(MB_HOLDING,  0, DEMO_PARAM_START_DELAY, MBF_U32),
    MB_ENTRY(MB_HOLDING,  2, DEMO_PARAM_RUN_HOURS,   MBF_U32),
    MB_ENTRY(MB_HOLDING, 10, DEMO_PARAM_VALVE_NAME,  MBF_BYTES),
    MB_ENTRY(MB_COILS,    0, DEMO_PARAM_ACK,         MBF_ARRAY),
    MB_ENTRY(MB_DISCRETE, 0, DEMO_PARAM_START_IN,    MBF_BIT),
    MB_ENTRY(MB_DISCRETE, 1, DEMO_PARAM_VALVE_OUT,   MBF_BIT),
    MB_ENTRY(MB_HOLDING, 20, PARAM_ID_ALARM_ACK,     MBF_U16),     /* write an alarm ID, or 65535 for all */
    MB_ENTRY(MB_INPUT,    0, PARAM_ID_ALARM_ACTIVE,  MBF_U16),
    MB_ENTRY(MB_INPUT,    1, PARAM_ID_ALARM_UNACKED, MBF_U16),
    MB_ENTRY(MB_INPUT,   10, PARAM_ID_ALARM_ACTIVE_BITS, MBF_ARRAY), /* 16 registers: bit (ID % 16) of register 10 + ID / 16 */
    MB_ENTRY(MB_INPUT,   30, PARAM_ID_ALARM_UNACK_BITS,  MBF_ARRAY),
};

/* Data-log channels: the logic hands one sample per scan to each (plat_log_put). */
const plat_log_chan_t app_demo_log[] = {
    { "start_in",  NULL, 2 },      /* fraction of the interval the input was on */
    { "valve_out", NULL, 2 },
    { "delay_left", "s", 1 },
};

/* touchscreen home screen tiles */
static const uint16_t app_demo_home[] = { DEMO_PARAM_START_DELAY, DEMO_PARAM_RUN_HOURS, DEMO_PARAM_START_IN,
                                          DEMO_PARAM_VALVE_OUT, DEMO_PARAM_VALVE_NAME };

/* TimerOnDelay, simplified demo model (the real one lives in the application layer). */
static void ton(ul_timer_t *t, bool in) {
    if (!in) { t->Current = t->Preset; t->Out = false; return; }
    if (t->Out) return;
    if (t->Current > g_dt_ms) t->Current -= g_dt_ms;
    else { t->Current = 0; t->Out = true; }
}

static void init(void) { StartDelay.Current = 2000; }    /* Preset comes from the parameter table */

static void io_in(const plat_io_in_t *in) { StartIn = plat_di(in, 0); }

static void logic(void) {            /* "Function1" */
    if (Ladder_Initial_Cycle) PowerUps++;
    ton(&StartDelay, StartIn);
    if (ValveOut == false && StartDelay.Out) LifetimeCycles++;
    ValveOut = StartDelay.Out;
    if (ValveOut) { run_ms += g_dt_ms; while (run_ms >= 1000) { run_ms -= 1000; RunSeconds++; } }
    alarm_set(DEMO_ALARM_FAULT, DemoFault);
    alarm_set_range(DEMO_ALARM_LEVEL, DemoLevel, 10.0f, 90.0f, 2.0f);
    plat_log_put(0, StartIn ? 1.0f : 0.0f, LQ_OK);
    plat_log_put(1, ValveOut ? 1.0f : 0.0f, LQ_OK);
    plat_log_put(2, StartIn ? (float)StartDelay.Current / 1000.0f : 0.0f, StartIn ? LQ_OK : LQ_NA);
}

static void io_out(plat_io_out_t *out) { plat_do_set(out, 0, ValveOut); }

static void publish(void) {
    demo_snapshot.delay_preset = StartDelay.Preset;
    demo_snapshot.delay_current = StartDelay.Current;
    demo_snapshot.start_in = StartIn;
    demo_snapshot.valve_out = ValveOut;
    demo_snapshot.power_ups = PowerUps;
    demo_snapshot.time = RTC_Time;
}

const scan_app_t app_demo = { .init = init, .io_in = io_in, .logic = logic, .io_out = io_out, .publish = publish };

/* everything the platform needs, for plat_start() */
const plat_app_t app_demo_plat = {
    .scan = &app_demo,
    .params = app_demo_params, .n_params = sizeof app_demo_params / sizeof app_demo_params[0],
    .retain = app_demo_retain, .n_retain = sizeof app_demo_retain / sizeof app_demo_retain[0],
    .layout_version = 1, .migrate = NULL,
    .modbus_map = app_demo_modbus, .n_modbus_map = sizeof app_demo_modbus / sizeof app_demo_modbus[0],
    .ident = { .vendor = "CEMS", .product = "CEMS Supervisor (demo app)", .revision = "0.3.0-dev",
               .write_level = PLAT_LVL_OPERATOR },
    .log_chans = app_demo_log, .n_log_chans = sizeof app_demo_log / sizeof app_demo_log[0],
    .ui_home_ids = app_demo_home, .n_ui_home = sizeof app_demo_home / sizeof app_demo_home[0],
    .alarms = app_demo_alarms, .n_alarms = sizeof app_demo_alarms / sizeof app_demo_alarms[0],
};
