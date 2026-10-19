/* app_demo.c - see app_demo.h. Tags are plain globals, as in the Stage 4 code. */
#include "app_demo.h"

/* ---- application tags ---- */
static bool       StartIn;          /* mapped from DI0 */
static bool       ValveOut;         /* mapped to DO0 */
static ul_timer_t StartDelay;
static uint32_t   PowerUps;

demo_snapshot_t demo_snapshot;

/* TimerOnDelay, simplified demo model (the real one lives in the application layer). */
static void ton(ul_timer_t *t, bool in) {
    if (!in) { t->Current = t->Preset; t->Out = false; return; }
    if (t->Out) return;
    if (t->Current > g_dt_ms) t->Current -= g_dt_ms;
    else { t->Current = 0; t->Out = true; }
}

static void init(void) {
    StartDelay.Preset = 2000; StartDelay.Current = 2000;
}

static void io_in(const plat_io_in_t *in) { StartIn = plat_di(in, 0); }

static int apply(const plat_cmd_t *c) {
    if (c->id == DEMO_PARAM_START_DELAY && c->kind == PLAT_VAL_U32 && c->v.u >= 100 && c->v.u <= 60000) {
        StartDelay.Preset = c->v.u;
        return 0;
    }
    return -1;                       /* unknown parameter or out of range */
}

static void logic(void) {            /* "Function1" */
    if (Ladder_Initial_Cycle) PowerUps++;
    ton(&StartDelay, StartIn);
    ValveOut = StartDelay.Out;
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

const scan_app_t app_demo = { init, io_in, apply, logic, io_out, publish };
