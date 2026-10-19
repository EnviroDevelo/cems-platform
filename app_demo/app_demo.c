/* app_demo.c - see app_demo.h. Tags are plain globals, as in the Stage 4 code. */
#include "app_demo.h"
#include "plat_tasks.h"

/* ---- application tags ---- */
static bool       StartIn;          /* mapped from DI0 */
static bool       ValveOut;         /* mapped to DO0 */
static ul_timer_t StartDelay;
static uint32_t   PowerUps;
char              ValveName[11];    /* retained block */
uint32_t          RunSeconds;       /* retained, written by the logic */
static uint32_t   run_ms;

demo_snapshot_t demo_snapshot;

const param_def_t app_demo_params[] = {
    PARAM_UINT(DEMO_PARAM_START_DELAY, "start_delay", StartDelay.Preset, CT_U32, 100, 60000, 2000,
               PLAT_LVL_OPERATOR, PF_PERSIST, "ms"),
    PARAM_UINT(DEMO_PARAM_RUN_HOURS, "run_seconds", RunSeconds, CT_U32, 0, 0xFFFFFFFFu, 0,
               PLAT_LVL_VIEW, PF_PERSIST | PF_RO, "s"),
};
const size_t app_demo_n_params = sizeof app_demo_params / sizeof app_demo_params[0];

static const char valve_name_default[11] = "Valve 1";
const retain_def_t app_demo_retain[] = {
    RETAIN(DEMO_BLOCK_VALVE_NAME, "valve_name", ValveName, 0, valve_name_default),
};
const size_t app_demo_n_retain = sizeof app_demo_retain / sizeof app_demo_retain[0];

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
    ValveOut = StartDelay.Out;
    if (ValveOut) { run_ms += g_dt_ms; while (run_ms >= 1000) { run_ms -= 1000; RunSeconds++; } }
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
};
