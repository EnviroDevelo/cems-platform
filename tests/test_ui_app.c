/* test_ui_app.c - records (whole-structure writes with the application's check) and the
 * application's own screens and record editors (ui_app.h, decision D26), on the
 * touchscreen (simulated BT817) and the web API. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "tst.h"
#include "eve.h"
#include "eve_sim.h"
#include "ui.h"
#include "ui_app.h"
#include "param.h"
#include "param_store.h"
#include "plat_log.h"
#include "fw_update.h"
#include "web_auth.h"
#include "http_api.h"
#include "http_util.h"
#include "bsp_host.h"
#include "ul_system.h"
#include "plat_alarm.h"
#include "plat_clock.h"

/* ---- an application shaped like the Valve Manager's ---- */
#define ROWS_N 4
typedef struct {
    uint8_t  NumberArray[ROWS_N];
    char     Name[ROWS_N][13];
    uint32_t Timers[ROWS_N];
    uint8_t  Inlets[ROWS_N];
    uint8_t  NumberOfPeriods;
} seq_t;
typedef struct {
    uint16_t From_Year, To_Year;
    bool     Month[12];
    bool     Day_Of_Week[7];
    uint8_t  Spare;
    uint8_t  From_Hour[3];                       /* [Seconds, Minutes, Hours] */
} sched_t;

static uint8_t  FirstInlets = 3, CmdStart, CmdYes, CmdNo, InletTouch, ShowDialog, Interconnections = 1, CmdDanger;
static bool     Running, StartEnable = true, InletEn[4] = { true, true, false, true }, InletOn[4];
static char     InName1[11], InName2[11], InName3[11];
static const char nm1[11] = "Stack", nm2[11] = "Duct", nm3[11] = "Cal gas";
static float    Temps[2] = { 21.5f, 180.25f };
static seq_t    Seq;
static sched_t  Sched;
static int      checks;
static seq_t    seen;                            /* the image the check was given */

static int check_seq(uint16_t id, const void *img, char *why, size_t cap) {
    (void)id;
    const seq_t *s = img;
    checks++;
    memcpy(&seen, s, sizeof seen);
    if (s->NumberOfPeriods > ROWS_N) { snprintf(why, cap, "At most %u periods", ROWS_N); return 1; }
    for (unsigned k = 0; k < s->NumberOfPeriods; ++k) {
        if (s->Inlets[k] < 1 || s->Inlets[k] > FirstInlets) { snprintf(why, cap, "Period %u: inlet %u is not fitted", k + 1u, s->Inlets[k]); return 1; }
        if (!s->Timers[k]) { snprintf(why, cap, "Period %u: no time set", k + 1u); return 1; }
    }
    return 0;
}

static const param_def_t defs[] = {
    PARAM_UINT(1001, "first_inlets", FirstInlets, CT_U8, 1, 19, 3, PLAT_LVL_OPERATOR, PF_PERSIST, NULL),
    PARAM_UINT(1002, "cmd_start", CmdStart, CT_U8, 0, 1, 0, PLAT_LVL_VIEW, PF_ANY_MODE, NULL),
    PARAM_BIT (1003, "running", Running, PLAT_LVL_VIEW, PF_RO),
    PARAM_BIT_D(1004, "start_enable", StartEnable, true, PLAT_LVL_VIEW, PF_ANY_MODE),
    PARAM_UINT(1005, "show_dialog", ShowDialog, CT_U8, 0, 1, 0, PLAT_LVL_VIEW, PF_ANY_MODE, NULL),
    PARAM_UINT(1006, "cmd_yes", CmdYes, CT_U8, 0, 1, 0, PLAT_LVL_VIEW, PF_ANY_MODE, NULL),
    PARAM_UINT(1007, "cmd_no", CmdNo, CT_U8, 0, 1, 0, PLAT_LVL_VIEW, PF_ANY_MODE, NULL),
    PARAM_UINT(1008, "inlet_touch", InletTouch, CT_U8, 0, 19, 0, PLAT_LVL_VIEW, PF_ANY_MODE, NULL),
    PARAM_ARRAY_BOOL(1009, "inlet_en", InletEn, PLAT_LVL_VIEW, PF_RO),
    PARAM_ARRAY_BOOL(1010, "inlet_on", InletOn, PLAT_LVL_VIEW, PF_RO),
    PARAM_UINT(1011, "cmd_danger", CmdDanger, CT_U8, 0, 1, 0, PLAT_LVL_VIEW, PF_ANY_MODE | PF_CONFIRM, NULL),
    PARAM_TEXT(1012, "inlet_name1", InName1, nm1, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_TEXT(1013, "inlet_name2", InName2, nm2, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_TEXT(1014, "inlet_name3", InName3, nm3, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_UINT(1015, "interconnections", Interconnections, CT_U8, 1, 2, 1, PLAT_LVL_VIEW, PF_ANY_MODE, NULL),
    PARAM_ARRAY_REAL(1016, "temps", Temps, -50.0f, 500.0f, PLAT_LVL_VIEW, PF_RO),
    PARAM_RECORD(1020, "Sequence1", Seq, check_seq, PLAT_LVL_OPERATOR, PF_PERSIST | PF_ANY_MODE),
    PARAM_RECORD(1021, "Schedule1", Sched, NULL, PLAT_LVL_OPERATOR, PF_PERSIST),
};

static const char *const dow[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const ui_field_t seq_fields[] = {
    { .label = "Name", .kind = RF_TEXT, .off = offsetof(seq_t, Name), .stride = 13, .size = 13 },
    { .label = "Time", .kind = RF_MS_AS_S, .off = offsetof(seq_t, Timers), .stride = 4, .min = 0, .max = 3600000 },
    { .label = "Inlet", .kind = RF_UINT, .ctype = CT_U8, .off = offsetof(seq_t, Inlets), .stride = 1, .min = 1, .max = 19, .max_param = 1001 },
};
static const ui_field_t sched_fields[] = {
    { .label = "From year", .kind = RF_UINT, .ctype = CT_U16, .off = offsetof(sched_t, From_Year), .min = 2020, .max = 2099 },
    { .label = "To year", .kind = RF_UINT, .ctype = CT_U16, .off = offsetof(sched_t, To_Year), .min = 2020, .max = 2099 },
    { .label = "Months", .kind = RF_BOOL, .off = offsetof(sched_t, Month), .count = 12, .max = 1, .items = mon, .n_items = 12 },
    { .label = "Days", .kind = RF_BOOL, .off = offsetof(sched_t, Day_Of_Week), .count = 7, .max = 1, .items = dow, .n_items = 7 },
    { .label = "Start", .kind = RF_HMS, .off = offsetof(sched_t, From_Hour), .max = 86399 },
};
static const ui_record_t records[] = {
    { .param_id = 1020, .title = "Sequence 1", .rows = ROWS_N, .rows_off = offsetof(seq_t, NumberOfPeriods), .rows_ctype = CT_U8,
      .fields = seq_fields, .n_fields = 3 },
    { .param_id = 1021, .title = "Schedule 1", .rows = 0, .fields = sched_fields, .n_fields = 5 },
};
static const ui_widget_t home_w[] = {
    { .kind = UW_SECTION, .label = "Valve Manager" },
    { .kind = UW_BUTTON, .label = "Start", .cmd_id = 1002, .state_id = 1003, .enable_id = 1004 },
    { .kind = UW_BUTTON, .label = "Flush all", .cmd_id = 1011 },
    { .kind = UW_VALUE, .label = "Stack temp", .value_id = 1016, .index = 1 },
    { .kind = UW_VALUE, .label = "Inlets fitted", .value_id = 1001, .index = UW_NO_INDEX },
    { .kind = UW_GRID, .label = "Inlets", .cmd_id = 1008, .count_id = 1001, .enable_id = 1009, .state_id = 1010, .names_id = 1012 },
    { .kind = UW_GRID, .label = "Second tube", .cmd_id = 1008, .count_id = 1001, .show_id = 1015, .show_min = 2 },
    { .kind = UW_RECORD, .label = "Edit sequence", .record_id = 1020 },
    { .kind = UW_DIALOG, .label = "Start in manual mode?", .show_id = 1005, .cmd_id = 1006, .cmd2_id = 1007 },
};
static const ui_widget_t sched_w[] = {
    { .kind = UW_RECORD, .label = "Edit schedule", .record_id = 1021 },
};
static const ui_screen_t screens[] = { { "Valves", home_w, 9 }, { "Schedule", sched_w, 1 } };
static const ui_app_t app = { .screens = screens, .n_screens = 2, .records = records, .n_records = 2 };

/* ---- harness (as test_ui.c) ---- */
static eve_sim_t sim;
static eve_t eve;
static plat_cmdq_t q;
static plat_seqlock_t lk;
static uint8_t fram[PLAT_FRAM_SIZE];
static nvm_dev_t fd; static nvm_ram_t fr;
static uint32_t now = 200000;
static const ui_cfg_t cfg = { .product = "CEMS Supervisor", .version = "0.5.0" };

static void scan(void) {
    plat_cmd_t c; while (plat_cmdq_pop(&q, &c)) (void)param_apply(&c);
    param_blob_drain(); param_store_scan_service(); param_publish();
}
static void store_tick(void) { now += 10; param_store_poll(now); scan(); param_store_poll(now); }
static void frame(void) { now += 20; ui_draw(&eve, now); }
static void press(uint8_t tag) { ui_input(tag, now); now += 60; ui_input(0, now); scan(); frame(); }
static bool tap(const char *label) {
    int t = eve_sim_tag_of(&sim, label);
    if (t <= 0) { printf("  (no button \"%s\")\n", label); return false; }
    press((uint8_t)t);
    return true;
}
static bool shows(const char *s) { return eve_sim_has_text(&sim, s); }
static void type(const char *s) { for (; *s; ++s) press((uint8_t)*s); }
static void dump(const char *name) {
    const char *dir = getenv("UI_DUMP");
    if (!dir) return;
    char p[256]; snprintf(p, sizeof p, "%s/%s.eve", dir, name);
    FILE *f = fopen(p, "wb"); if (f) { eve_sim_dump(&sim, f); fclose(f); }
}

static void boot(bool wipe) {
    if (wipe) memset(fram, 0, sizeof fram);
    nvm_ram_init(&fd, &fr, fram, sizeof fram, true);
    CHECK_EQ(param_init(defs, sizeof defs / sizeof defs[0]), PERR_OK);
    CHECK(param_store_init(&fd, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    plat_cmdq_init(&q); lk.seq = 0;
    param_bind(&q, &lk);
    Temps[0] = 21.5f; Temps[1] = 180.25f;                    /* measured values, written by the logic */
    InletEn[0] = InletEn[1] = InletEn[3] = true; InletEn[2] = false;
    param_publish();
}

static void ui_setup(void) {
    boot(true);
    CHECK_EQ(alarm_init(NULL, 0, BSP_RESET_POWER_ON), 0);
    auth_init();
    CHECK_EQ(ui_app_check(&app), 0);
    ui_app_set(&app);
    RTC_Date.Year = 2026; RTC_Date.Month = 10; RTC_Date.Day_In_Month = 20;
    RTC_Time.Hours = 9; RTC_Time.Minutes = 41; RTC_Time.Seconds = 7;
    eve_sim_init(&sim);
    CHECK_EQ(eve_init(&eve, &sim.bus, &eve_panel_rvt70h), 0);
    ui_init(&cfg, now);
    frame();
}

static void login_operator(void) {
    tap("Log in"); tap("Operator"); type("1111"); tap("Enter");
    CHECK_EQ(ui_level(), PLAT_LVL_OPERATOR);
}

/* ---------------- records: the parameter-table side ---------------- */
static void good_seq(seq_t *s) {
    memset(s, 0, sizeof *s);
    s->NumberOfPeriods = 2;
    strcpy(s->Name[0], "Stack"); strcpy(s->Name[1], "Zero");
    s->Timers[0] = 60000; s->Timers[1] = 30000;
    s->Inlets[0] = 1; s->Inlets[1] = 3;
}

static void t_record_writes(void) {
    boot(true);
    seq_t s;
    good_seq(&s);
    uint32_t t1, t2;
    param_err_t e;
    char why[80];
    /* only whole images, only through param_record_post */
    CHECK_EQ(param_blob_check(1020, 0, &s, 16, false, PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_TYPE);
    CHECK_EQ(param_blob_post(1020, 0, &s, 16, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_TYPE);
    CHECK_EQ(param_record_post(1020, &s, sizeof s - 1u, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, &t1), PERR_RANGE);
    CHECK_EQ(param_record_post(1001, &s, 1, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, &t1), PERR_TYPE);
    CHECK_EQ(param_record_post(1020, &s, sizeof s, PLAT_SRC_LOCAL, PLAT_LVL_VIEW, &t1), PERR_ACCESS);
    CHECK_EQ(param_record_post(1020, &s, sizeof s, PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR, &t1), PERR_ACCESS);   /* never Modbus */
    CHECK(param_find(1020)->flags & PF_NO_MODBUS);
    CHECK_EQ(param_record_check(1021, PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_MODE);         /* not PF_ANY_MODE: REMOTE only */
    CHECK_EQ(param_record_check(1021, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    /* accepted: the check sees the whole new image, the tag changes in one scan */
    checks = 0;
    CHECK_EQ(param_record_post(1020, &s, sizeof s, PLAT_SRC_WEB, PLAT_LVL_OPERATOR, &t1), PERR_OK);
    CHECK_EQ(param_record_post(1020, &s, sizeof s, PLAT_SRC_WEB, PLAT_LVL_OPERATOR, &t2), PERR_BUSY);   /* one at a time */
    CHECK(!param_record_result(t1, &e, why, sizeof why));                                       /* not yet applied */
    CHECK(Seq.NumberOfPeriods == 0);
    scan();
    CHECK_EQ(checks, 1); CHECK(memcmp(&seen, &s, sizeof s) == 0);
    CHECK(param_record_result(t1, &e, why, sizeof why)); CHECK_EQ(e, PERR_OK); CHECK(!strcmp(why, ""));
    CHECK(memcmp(&Seq, &s, sizeof s) == 0);
    seq_t rd; CHECK_EQ(param_read_blob(1020, 0, &rd, sizeof rd), PERR_OK); CHECK(memcmp(&rd, &s, sizeof s) == 0);
    /* refused by the check: nothing changes, the reason comes back */
    seq_t bad = s; bad.Inlets[1] = 7;
    CHECK_EQ(param_record_post(1020, &bad, sizeof bad, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, &t2), PERR_OK);
    CHECK(t2 != t1);
    scan();
    CHECK(param_record_result(t2, &e, why, sizeof why)); CHECK_EQ(e, PERR_INTERLOCK);
    CHECK(!strcmp(why, "Period 2: inlet 7 is not fitted"));
    CHECK(!param_record_result(t1, &e, why, sizeof why));                                       /* an old ticket */
    CHECK_EQ(Seq.Inlets[1], 3);
    /* a check without a reason still says something */
    /* checked again at apply time: the mode changed in between */
    CHECK_EQ(param_record_post(1021, &Sched, sizeof Sched, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, &t1), PERR_OK);
    plat_cmd_t m = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = 1, .kind = PLAT_VAL_U32, .v.u = PARAM_MODE_REMOTE };
    CHECK_EQ(param_apply(&m), PERR_OK);
    scan();
    CHECK(param_record_result(t1, &e, why, sizeof why)); CHECK_EQ(e, PERR_MODE);
    m.v.u = PARAM_MODE_LOCAL; CHECK_EQ(param_apply(&m), PERR_OK);
    /* kept in FRAM over a power cut */
    for (int k = 0; k < 3; ++k) store_tick();
    now += 2000; store_tick(); store_tick();
    memset(&Seq, 0, sizeof Seq);
    boot(false);
    CHECK(memcmp(&Seq, &s, sizeof s) == 0);
    /* a table entry that is not a byte block, or too large, is refused */
    static uint8_t huge[PARAM_RECORD_MAX + 4];
    static uint32_t notblob;
    const param_def_t bad_defs[] = { PARAM_RECORD(1100, "huge", huge, NULL, PLAT_LVL_OPERATOR, 0) };
    CHECK_EQ(param_init(bad_defs, 1), PERR_TABLE); CHECK_EQ(param_table_error(), 1100);
    param_def_t d2 = PARAM_UINT(1101, "x", notblob, CT_U32, 0, 10, 0, PLAT_LVL_OPERATOR, PF_RECORD, NULL);
    CHECK_EQ(param_init(&d2, 1), PERR_OK);       /* a scalar with the flag is simply never a record */
    CHECK_EQ(param_record_post(1101, &notblob, 4, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, &t1), PERR_TYPE);
}

static void t_descriptions(void) {
    boot(true);
    CHECK_EQ(ui_app_check(&app), 0);
    CHECK_EQ(ui_app_check(NULL), 0);
    /* a widget naming a missing parameter */
    ui_widget_t w1[] = { { .kind = UW_BUTTON, .label = "x", .cmd_id = 4321 } };
    ui_screen_t s1[] = { { "x", w1, 1 } };
    ui_app_t a1 = { .screens = s1, .n_screens = 1 };
    CHECK_EQ(ui_app_check(&a1), 4321);
    /* a button without a command, a record button for a record not described */
    ui_widget_t w2[] = { { .kind = UW_BUTTON, .label = "x" } };
    ui_screen_t s2[] = { { "x", w2, 1 } };
    ui_app_t a2 = { .screens = s2, .n_screens = 1 };
    CHECK_EQ(ui_app_check(&a2), 0xFFFF);
    ui_widget_t w3[] = { { .kind = UW_RECORD, .label = "x", .record_id = 1021 } };
    ui_screen_t s3[] = { { "x", w3, 1 } };
    ui_app_t a3 = { .screens = s3, .n_screens = 1, .records = records, .n_records = 1 };
    CHECK_EQ(ui_app_check(&a3), 1021);
    /* a field that runs past the end of the record, a record that is not PF_RECORD */
    ui_field_t f4[] = { { .label = "x", .kind = RF_UINT, .ctype = CT_U32, .off = offsetof(seq_t, Timers), .stride = 4, .min = 0, .max = 1 } };
    ui_record_t r4[] = { { .param_id = 1020, .title = "x", .rows = ROWS_N + 1, .rows_off = UI_ROWS_ALL, .fields = f4, .n_fields = 1 } };
    ui_app_t a4 = { .records = r4, .n_records = 1 };
    CHECK_EQ(ui_app_check(&a4), 0);
    r4[0].rows = 20;
    CHECK_EQ(ui_app_check(&a4), 1020);
    r4[0].param_id = 1001;
    CHECK_EQ(ui_app_check(&a4), 1001);

    /* field access */
    ui_app_set(&app);
    seq_t s; good_seq(&s);
    int64_t v;
    const ui_field_t *time = &seq_fields[1], *inlet = &seq_fields[2];
    CHECK(ui_field_get(time, (uint8_t *)&s, 1, 0, &v)); CHECK_EQ(v, 30000);
    CHECK(ui_field_get(inlet, (uint8_t *)&s, 1, 0, &v)); CHECK_EQ(v, 3);
    CHECK(!ui_field_set(inlet, (uint8_t *)&s, 0, 0, 4));          /* max from first_inlets (3) */
    CHECK_EQ(ui_field_max(inlet), 3);
    CHECK(ui_field_set(inlet, (uint8_t *)&s, 0, 0, 2)); CHECK_EQ(s.Inlets[0], 2);
    CHECK(ui_field_set(time, (uint8_t *)&s, 3, 0, 1500)); CHECK_EQ(s.Timers[3], 1500u);
    CHECK_EQ(ui_record_rows_used(&records[0], (uint8_t *)&s), 2u);
    ui_record_set_rows(&records[0], (uint8_t *)&s, 9); CHECK_EQ(s.NumberOfPeriods, ROWS_N);
    sched_t sc; memset(&sc, 0, sizeof sc);
    CHECK(ui_field_set(&sched_fields[4], (uint8_t *)&sc, 0, 0, 7 * 3600 + 30 * 60 + 5));
    CHECK_EQ(sc.From_Hour[2], 7); CHECK_EQ(sc.From_Hour[1], 30); CHECK_EQ(sc.From_Hour[0], 5);
    CHECK(ui_field_set(&sched_fields[3], (uint8_t *)&sc, 0, 6, 1)); CHECK(sc.Day_Of_Week[6]);
    CHECK(ui_param_value(1016, 1, &v)); CHECK_EQ(v, 180);
    CHECK(ui_param_value(1009, 2, &v)); CHECK_EQ(v, 0);
    CHECK(!ui_param_value(1009, 4, &v));
}

/* ---------------- the touchscreen ---------------- */
static void t_screens(void) {
    ui_setup();
    CHECK_EQ(ui_screen(), UI_APP);                          /* the application's first screen comes first */
    CHECK(shows("Valves")); CHECK(shows("Schedule")); CHECK(shows("Status"));
    CHECK(shows("Valve Manager")); CHECK(shows("180.25")); CHECK(shows("Stack temp"));
    /* the grid: three inlets (first_inlets), named, the third not touchable */
    CHECK(shows("Stack")); CHECK(shows("Duct")); CHECK(shows("Cal gas"));
    CHECK(!shows("Second tube"));                           /* only while interconnections >= 2 */
    dump("app_home");
    tap("Duct");
    CHECK_EQ(InletTouch, 2);
    InletTouch = 0;
    CHECK(eve_sim_tag_of(&sim, "Cal gas") <= 0);             /* disabled: not touchable */
    /* a button with state and enable */
    tap("Start"); CHECK_EQ(CmdStart, 1); CHECK(!strcmp(ui_toast(), "Start"));
    CmdStart = 0; StartEnable = false; param_publish(); frame();
    CHECK(eve_sim_tag_of(&sim, "Start") <= 0);               /* greyed out */
    StartEnable = true; param_publish(); frame();
    /* a PF_CONFIRM command asks first */
    tap("Flush all"); CHECK(shows("Flush all?")); CHECK_EQ(CmdDanger, 0);
    tap("Yes"); CHECK_EQ(CmdDanger, 1);
    /* a dialog the application raises: only its buttons work while it is up */
    int start_tag = eve_sim_tag_of(&sim, "Start");
    CHECK(start_tag > 0);
    ShowDialog = 1; param_publish(); frame();
    CHECK(shows("Start in manual mode?"));
    dump("app_dialog");
    press((uint8_t)start_tag);                               /* the button under the dialog */
    CHECK_EQ(CmdStart, 0);
    tap("No"); CHECK_EQ(CmdNo, 1); CHECK_EQ(CmdYes, 0);
    tap("Yes"); CHECK_EQ(CmdYes, 1);
    ShowDialog = 0; param_publish(); frame();
    CHECK(!shows("Start in manual mode?"));
    /* shown only from a level: the second tube */
    Interconnections = 2; param_publish(); frame();
    CHECK(shows("Second tube"));
    Interconnections = 1; param_publish(); frame();
    /* an editable value opens the ordinary editor (once logged in); back to the screen after */
    CHECK(eve_sim_tag_of(&sim, "Inlets fitted") <= 0);       /* not logged in: not touchable */
    login_operator();
    CHECK_EQ(ui_screen(), UI_APP);
    tap("Inlets fitted"); CHECK_EQ(ui_screen(), UI_EDIT); CHECK(shows("first_inlets  (1001)"));
    tap("Cancel"); CHECK_EQ(ui_screen(), UI_APP);
    /* tabs */
    tap("Schedule"); CHECK_EQ(ui_app_screen(), 1); CHECK(shows("Edit schedule"));
    tap("Status"); CHECK_EQ(ui_screen(), UI_HOME);
    tap("Home"); CHECK_EQ(ui_screen(), UI_APP);
    CHECK(ui_last_touch_ms != 0);                            /* R3: when the panel was last touched */
}

static void t_editor_table(void) {
    ui_setup();
    good_seq(&Seq); param_publish(); frame();
    tap("Edit sequence");
    CHECK_EQ(ui_screen(), UI_REC);
    CHECK(shows("Sequence 1")); CHECK(shows("In use: 2"));
    CHECK(shows("Stack")); CHECK(shows("Zero")); CHECK(shows("60 s")); CHECK(shows("30 s"));
    dump("rec_table");
    /* not logged in: shown, not changeable */
    tap("Zero"); CHECK_EQ(ui_screen(), UI_REC); CHECK(strstr(ui_toast(), "operator") != NULL);
    tap("Close"); CHECK_EQ(ui_screen(), UI_APP);
    login_operator();
    tap("Edit sequence");
    /* change a name and a time; the copy changes, the record not yet */
    tap("Zero"); CHECK_EQ(ui_screen(), UI_EDIT); CHECK(shows("Sequence 1: Name, row 2"));
    tap("Clear"); type("span"); tap("OK");
    CHECK_EQ(ui_screen(), UI_REC); CHECK(shows("span"));
    CHECK(!strcmp(Seq.Name[1], "Zero"));
    tap("60 s"); tap("Clear"); type("12.5"); tap("OK");
    CHECK(shows("12.5 s"));
    /* a third period: + then its inlet (the editor's maximum follows first_inlets) */
    tap("+"); CHECK(shows("In use: 3"));
    int t = eve_sim_tag_of(&sim, "0 s");
    CHECK(t > 0);
    tap("0 s"); type("5"); tap("OK");
    /* inlet of row 3 is 0: open its cell (the third "0" cell), try 9, then 2 */
    int tg = eve_sim_tag_of(&sim, "0");
    CHECK(tg > 0);
    press((uint8_t)tg);
    CHECK_EQ(ui_screen(), UI_EDIT); CHECK(shows("Range 1 .. 3"));
    type("9"); tap("OK");
    CHECK_EQ(ui_screen(), UI_EDIT); CHECK(strstr(ui_toast(), "1 .. 3") != NULL);
    tap("Clear"); type("2"); tap("OK");
    CHECK_EQ(ui_screen(), UI_REC);
    /* save: one record write, applied after the check */
    checks = 0;
    tap("Save");
    scan(); frame();
    CHECK_EQ(checks, 1);
    CHECK_EQ(ui_screen(), UI_APP); CHECK(strstr(ui_toast(), "saved") != NULL);
    CHECK_EQ(Seq.NumberOfPeriods, 3); CHECK(!strcmp(Seq.Name[1], "span")); CHECK_EQ(Seq.Timers[0], 12500u);
    CHECK_EQ(Seq.Timers[2], 5000u); CHECK_EQ(Seq.Inlets[2], 2);
    /* refused by the check: the reason is shown, the editor stays open with the copy */
    tap("Edit sequence");
    tap("+"); CHECK(shows("In use: 4"));
    tap("Save"); scan(); frame();
    CHECK_EQ(ui_screen(), UI_REC);
    CHECK(!strcmp(ui_toast(), "Period 4: inlet 0 is not fitted"));
    CHECK_EQ(Seq.NumberOfPeriods, 3);
    dump("rec_refused");
    tap("Cancel"); CHECK_EQ(ui_screen(), UI_APP); CHECK(!strcmp(ui_toast(), "Not saved"));
    /* the record is also reached from the Settings list */
    tap("Settings");
    for (int k = 0; k < 40 && !shows("Sequence1"); ++k) tap("Down");
    tap("Sequence1"); CHECK_EQ(ui_screen(), UI_REC);
    tap("Close"); CHECK_EQ(ui_screen(), UI_PARAMS);
}

static void t_editor_form(void) {
    ui_setup();
    Sched.From_Year = 2026; Sched.To_Year = 2027; Sched.Month[9] = true; Sched.From_Hour[2] = 6;
    param_publish();
    login_operator();
    tap("Home"); tap("Schedule"); tap("Edit schedule");
    CHECK_EQ(ui_screen(), UI_REC);
    CHECK(shows("From year")); CHECK(shows("2026")); CHECK(shows("Oct")); CHECK(shows("06:00:00"));
    dump("rec_form");
    tap("Mon"); tap("Fri"); tap("Oct");                       /* tick boxes toggle in the copy */
    tap("06:00:00"); CHECK_EQ(ui_screen(), UI_EDIT); tap("Clear"); type("7.30.15"); tap("OK");
    CHECK(shows("07:30:15"));
    tap("2027"); tap("Clear"); type("2019"); tap("OK");       /* out of range */
    CHECK_EQ(ui_screen(), UI_EDIT); tap("Cancel");
    CHECK_EQ(ui_screen(), UI_REC);
    tap("Save"); scan(); frame();
    CHECK(Sched.Day_Of_Week[1]); CHECK(Sched.Day_Of_Week[5]); CHECK(!Sched.Month[9]);
    CHECK_EQ(Sched.From_Hour[2], 7); CHECK_EQ(Sched.From_Hour[1], 30); CHECK_EQ(Sched.From_Hour[0], 15);
    CHECK_EQ(Sched.To_Year, 2027);
    /* Schedule1 is not PF_ANY_MODE: in REMOTE the touchscreen may look but not save */
    plat_cmd_t m = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = 1, .kind = PLAT_VAL_U32, .v.u = PARAM_MODE_REMOTE };
    CHECK_EQ(param_apply(&m), PERR_OK); param_publish();
    tap("Edit schedule");
    CHECK(eve_sim_tag_of(&sim, "Save") <= 0);                /* greyed, with the reason */
    CHECK(shows("Not allowed in REMOTE mode: the PLC or web is in control"));
    tap("Close");
    m.v.u = PARAM_MODE_LOCAL; CHECK_EQ(param_apply(&m), PERR_OK);
}

/* ---------------- the web API ---------------- */
static char cap[64 * 1024]; static size_t capn;
static int cap_send(void *ctx, const void *p, size_t n) {
    (void)ctx; if (capn + n >= sizeof cap) return -1;
    memcpy(cap + capn, p, n); capn += n; cap[capn] = 0; return 0;
}
static char sid[AUTH_TOKEN_LEN + 1];
static uint8_t scratch[20 * 1024];
static int call(const char *method, const char *path, const char *body) {
    static char raw[8192];
    size_t bl = body ? strlen(body) : 0;
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: unit\r\nX-CEMS: 1\r\nConnection: close\r\n%s%s%sContent-Length: %zu\r\n\r\n%s",
             method, path, sid[0] ? "Cookie: sid=" : "", sid, sid[0] ? "\r\n" : "", bl, body ? body : "");
    http_req_t r;
    int h = http_parse_head(raw, (uint32_t)strlen(raw), &r);
    if (h <= 0) return -1;
    r.body = (const uint8_t *)raw + h; r.body_len = r.content_length;
    http_out_t o; out_init(&o, cap_send, NULL); capn = 0;
    http_handle(&r, &o, now);
    int st = 0; sscanf(cap, "HTTP/1.1 %d", &st);
    const char *ck = strstr(cap, "Set-Cookie: sid=");
    if (ck) { ck += 16; if (*ck == ';') sid[0] = 0; else { memcpy(sid, ck, AUTH_TOKEN_LEN); sid[AUTH_TOKEN_LEN] = 0; } }
    return st;
}
static bool has(const char *s) { return strstr(cap, s) != NULL; }

static void t_web(void) {
    boot(true);
    auth_init();
    ui_app_set(&app);
    http_cfg_t c = { .product = "CEMS Supervisor", .fw_version = "0.5.0", .scratch = scratch, .scratch_size = sizeof scratch,
                     .wait = store_tick };
    http_api_init(&c);
    sid[0] = 0;
    CHECK_EQ(call("GET", "/api/ui", NULL), 200);
    CHECK(has("\"title\":\"Valves\"")); CHECK(has("\"kind\":\"grid\"")); CHECK(has("\"names\":1012"));
    CHECK(has("\"kind\":\"dialog\",\"label\":\"Start in manual mode?\""));
    CHECK(has("\"id\":1020,\"title\":\"Sequence 1\""));
    char exp[160];
    snprintf(exp, sizeof exp, "\"label\":\"Time\",\"kind\":\"ms_as_s\",\"off\":%u,\"esize\":4,\"signed\":false,\"row_stride\":4",
             (unsigned)offsetof(seq_t, Timers));
    CHECK(has(exp));
    CHECK(has("\"label\":\"Inlet\",\"kind\":\"uint\"")); CHECK(has("\"max\":3"));   /* the live maximum */
    CHECK(has("\"items\":[\"Sun\",\"Mon\""));
    CHECK_EQ(call("GET", "/api/params", NULL), 200); CHECK(has("\"record\""));
    good_seq(&Seq); param_publish();
    CHECK_EQ(call("GET", "/api/record?id=1020", NULL), 200);
    CHECK(has("\"writable\":false"));
    char hex[2 * sizeof(seq_t) + 1];
    hex_encode((const uint8_t *)&Seq, sizeof Seq, hex);
    CHECK(has(hex));
    CHECK_EQ(call("GET", "/api/record?id=1001", NULL), 404);
    seq_t s = Seq; s.Timers[0] = 99000;
    hex_encode((const uint8_t *)&s, sizeof s, hex);
    static char body[2 * sizeof(seq_t) + 64];
    snprintf(body, sizeof body, "id=1020&data=%s", hex);
    CHECK_EQ(call("POST", "/api/record", body), 401);
    CHECK_EQ(call("POST", "/api/login", "level=operator&password=1111"), 200);
    CHECK_EQ(call("GET", "/api/record?id=1020", NULL), 200); CHECK(has("\"writable\":true"));
    CHECK_EQ(call("POST", "/api/record", body), 200); CHECK(has("\"ok\":true"));
    CHECK_EQ(Seq.Timers[0], 99000u);
    s.Inlets[0] = 12; hex_encode((const uint8_t *)&s, sizeof s, hex);
    snprintf(body, sizeof body, "id=1020&data=%s", hex);
    CHECK_EQ(call("POST", "/api/record", body), 409);
    CHECK(has("\"why\":\"Period 1: inlet 12 is not fitted\""));
    CHECK_EQ(Seq.Inlets[0], 1);
    CHECK_EQ(call("POST", "/api/record", "id=1020&data=00"), 400);
    CHECK_EQ(call("POST", "/api/write", "id=1020&v=00"), 400);       /* not through the ordinary write */
    CHECK(has("\"ok\":false"));
    CHECK_EQ(call("POST", "/api/record", "id=1021&data=00"), 400);
    ui_app_set(NULL);
    CHECK_EQ(call("GET", "/api/ui", NULL), 200); CHECK(has("{\"screens\":[],\"records\":[]}"));
}

void test_ui_app_all(void) {
    RUN(t_record_writes);
    RUN(t_descriptions);
    RUN(t_screens);
    RUN(t_editor_table);
    RUN(t_editor_form);
    RUN(t_web);
    ui_app_set(NULL);
}
