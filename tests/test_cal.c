/* test_cal.c - calibration records (CIP-18, CIP-17.6, CIP-19.3): the channel table check, a field
 * calibration from start to PASS, FAIL from the check hook / no result / a coefficient out of
 * range, abort and timeout, factory records and revert, the permission level, the state
 * refusals, survival across a restart and restore defaults, and due dates with alarm 21.
 * Requirements proved here: CIP-18.1, 18.2, 18.3, 18.4, 18.5, 18.6, 17.6, 19.3. */
#include <math.h>
#include <string.h>
#include "tst.h"
#include "plat_cal.h"
#include "plat_state.h"
#include "plat_alarm.h"
#include "plat_cmdq.h"
#include "param.h"
#include "param_store.h"
#include "bsp_host.h"
#include "nvm.h"
#include "plat_config.h"
#include "ul_system.h"

static float zero = 0.0f, span = 1.0f, other = 5.0f;
static int32_t count;

static const param_def_t pdefs[] = {
    PARAM_REAL(1300, "nox_zero", zero, -100.0f, 100.0f, 0.0f, "ppm", PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY),
    PARAM_REAL(1301, "nox_span", span, 0.5f, 2.0f, 1.0f, NULL, PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY),
    PARAM_REAL(1302, "other", other, 0.0f, 10.0f, 5.0f, NULL, PLAT_LVL_ADMIN, PF_PERSIST),
    PARAM_INT (1303, "count", count, CT_I32, 0, 10, 0, PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY, NULL),
};

static const char *reject;                         /* what the check hook says */
static unsigned checked;
static const char *check(unsigned ch, const plat_cal_record_t *p) {
    (void)p;
    checked = ch + 1u;
    return reject;
}

static plat_cal_channel_t chans[] = {
    { .name = "NOx", .coef_id = { 1300, 1301 }, .interval_days = 30, .check = check },
};

static uint8_t     fram[128u * 1024u];
static nvm_ram_t   ram;
static nvm_dev_t   dev;
static plat_cmdq_t q;
static uint32_t    store_ms;

static void set_clock(uint16_t y, uint8_t mo, uint8_t d) {
    RTC_Date.Year = y; RTC_Date.Month = mo; RTC_Date.Day_In_Month = d;
    RTC_Time.Hours = 12; RTC_Time.Minutes = 0; RTC_Time.Seconds = 0;
}

static int drain(void) {                           /* what the scan does with queued writes */
    plat_cmd_t c;
    int last = -1;
    while (plat_cmdq_pop(&q, &c)) last = param_apply(&c);
    return last;
}

static void scan(unsigned n) {
    for (unsigned k = 0; k < n; ++k) {
        bsp_host_advance_ms(10);
        drain();
        alarm_scan(bsp_ms());
        plat_cal_scan(bsp_ms());
        plat_state_scan(bsp_ms());
    }
    param_publish();
}

static void save(void) {
    for (int k = 0; k < 2; ++k) {
        store_ms += STORE_SAVE_PERIOD_MS;
        param_store_poll(store_ms);
        param_store_scan_service();
        param_store_poll(store_ms);
    }
}

/* a start: the table, the store (from what fram holds), the alarms, the state */
static void boot(void) {
    plat_state_reset_for_test();
    plat_cal_reset_for_test();
    CHECK_EQ(param_init(pdefs, sizeof pdefs / sizeof pdefs[0]), PERR_OK);
    plat_cmdq_init(&q);
    param_bind(&q, NULL);
    CHECK_EQ(plat_cal_init(chans, 1), 0);
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK(param_store_init(&dev, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    CHECK_EQ(alarm_init(NULL, 0, 0), 0);
    CHECK_EQ(plat_state_init(NULL, 0), 0);
}

static void setup(void) {
    bsp_host_reset();
    bsp_host.ms = 1000;
    store_ms = 0;
    memset(fram, 0, sizeof fram);
    set_clock(2026, 10, 26);
    reject = NULL; checked = 0;
    boot();
    plat_cmd_t c = { .id = PARAM_ID_HW_REV, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_FACTORY, .kind = PLAT_VAL_U32, .v.u = 1 };
    CHECK_EQ(param_apply(&c), PERR_OK);            /* valid factory data: no CAL_INVALID */
    save();
    CHECK(param_store_calibration_valid());
    scan(21);
    CHECK_EQ(plat_state(), PLAT_ST_READY);
}

static void run(uint8_t level, const float *coef, unsigned n, bool accept) {
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_LOCAL, level), PERR_OK);
    scan(1);
    CHECK(plat_cal_active(NULL));
    CHECK(plat_cal_point(0, 0.0f, 0.4f, 0.0f));
    CHECK(plat_cal_point(1, 80.0f, 78.0f, 80.1f));
    if (coef) CHECK(plat_cal_propose(coef, n));
    CHECK_EQ(plat_cal_end(accept, PLAT_SRC_LOCAL, level), PERR_OK);
    scan(1);
    CHECK(!plat_cal_active(NULL));
}

static void t_table(void) {
    setup();
    plat_cal_channel_t bad[PLAT_CAL_CH_MAX + 1];
    for (unsigned k = 0; k <= PLAT_CAL_CH_MAX; ++k) bad[k] = chans[0];
    CHECK_EQ(plat_cal_init(bad, PLAT_CAL_CH_MAX + 1), (int)PLAT_CAL_CH_MAX + 1);   /* too many */
    bad[0].name = NULL;
    CHECK_EQ(plat_cal_init(bad, 1), 1);
    bad[0] = chans[0]; bad[0].coef_id[1] = 4242;
    CHECK_EQ(plat_cal_init(bad, 1), 1);                    /* no such parameter */
    bad[0].coef_id[1] = 1302;
    CHECK_EQ(plat_cal_init(bad, 1), 1);                    /* not factory class */
    bad[0].coef_id[1] = 1303;
    CHECK_EQ(plat_cal_init(bad, 1), 1);                    /* not a float */
    bad[0].coef_id[0] = 0;
    CHECK_EQ(plat_cal_init(bad, 1), 1);                    /* no coefficients */
    CHECK_EQ(plat_cal_init(chans, 1), 0);
    CHECK_EQ(plat_cal_count(), 1u);
    CHECK(!strcmp(plat_cal_name(0), "NOx"));
}

static void t_field_pass(void) {                           /* CIP-18.2 */
    setup();
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_ACCESS);   /* setting 321: admin */
    CHECK(!strcmp(plat_cal_refusal(), "login level too low"));
    CHECK_EQ(plat_cal_begin(1, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_RANGE);
    CHECK_EQ(plat_cal_end(true, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_RANGE);       /* nothing running */
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_WEB, PLAT_LVL_ADMIN), PERR_MODE);       /* LOCAL mode: the web may not */
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);
    scan(1);
    unsigned ch = 9;
    CHECK(plat_cal_active(&ch));
    CHECK_EQ(ch, 0u);
    CHECK_EQ(plat_state(), PLAT_ST_CALIBRATION);
    CHECK(!plat_state_allows(PLAT_ACT_RESTORE_DEFAULTS));
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_BUSY);
    CHECK(plat_cal_point(0, 0.0f, 0.4f, 0.0f));
    CHECK(plat_cal_point(1, 80.0f, 78.0f, 80.1f));
    CHECK(!plat_cal_point(PLAT_CAL_PTS_MAX, 1.0f, 1.0f, 1.0f));
    float bad_n[3] = { 0 };
    CHECK(!plat_cal_propose(bad_n, 3));                    /* the channel has 2 */
    const float coef[2] = { -0.4f, 1.025f };
    CHECK(plat_cal_propose(coef, 2));
    CHECK_EQ(plat_cal_end(true, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);
    scan(1);
    CHECK_EQ(checked, 1u);
    CHECK(!plat_cal_active(NULL));
    CHECK_EQ(plat_state(), PLAT_ST_READY);
    CHECK(fabsf(zero + 0.4f) < 1e-6f && fabsf(span - 1.025f) < 1e-6f);
    plat_cal_record_t r;
    CHECK(plat_cal_in_force(0, &r));
    CHECK_EQ(r.kind, PLAT_CAL_FIELD);
    CHECK_EQ(r.outcome, PLAT_CAL_PASS);
    CHECK_EQ(r.level, PLAT_LVL_ADMIN);
    CHECK_EQ(r.src, PLAT_SRC_LOCAL);
    CHECK_EQ(r.n_pts, 2u);
    CHECK_EQ(r.n_coef, 2u);
    CHECK(r.ref[1] == 80.0f && r.found[1] == 78.0f && r.left[1] == 80.1f);
    CHECK(isnan(r.ref[2]));
    CHECK(r.time != 0);
    CHECK(!plat_cal_factory(0, &r));                       /* a field calibration is not a factory one */
    CHECK(plat_cal_history(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_PASS);
    CHECK(!plat_cal_history(1, &r));
    CHECK(!strcmp(plat_cal_outcome_name(PLAT_CAL_PASS), "pass"));
}

static void t_fail_abort(void) {                           /* CIP-18.4 */
    setup();
    const float coef[2] = { 1.0f, 1.1f };
    reject = "span drift";
    run(PLAT_LVL_ADMIN, coef, 2, true);
    plat_cal_record_t r;
    CHECK(plat_cal_history(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_FAIL);
    CHECK(r.coef[1] == 1.1f);                              /* what was proposed */
    CHECK(zero == 0.0f && span == 1.0f);                   /* nothing in force changed */
    CHECK(!plat_cal_in_force(0, &r));
    reject = NULL;
    run(PLAT_LVL_ADMIN, NULL, 0, true);                    /* accepted with no result */
    CHECK(plat_cal_history(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_FAIL);
    const float wild[2] = { 1.0f, 3.0f };                  /* span limit is 2.0: none written */
    run(PLAT_LVL_ADMIN, wild, 2, true);
    CHECK(plat_cal_history(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_FAIL);
    CHECK(zero == 0.0f && span == 1.0f);
    run(PLAT_LVL_ADMIN, coef, 2, false);
    CHECK(plat_cal_history(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_ABORTED);
    CHECK(zero == 0.0f);
    if (PLAT_CAL_HIST > 3u) {                              /* the 32 KB map keeps 3 */
        CHECK(plat_cal_history(3, &r));
        CHECK_EQ(r.outcome, PLAT_CAL_FAIL);
    }
    /* left running: aborted after the timeout */
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);
    scan(1);
    bsp_host_advance_ms(PLAT_CAL_TIMEOUT_MS);
    scan(1);
    CHECK(!plat_cal_active(NULL));
    CHECK_EQ(plat_state(), PLAT_ST_READY);
    CHECK(plat_cal_history(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_ABORTED);
    /* the history keeps the newest PLAT_CAL_HIST */
    for (unsigned k = 0; k < PLAT_CAL_HIST + 2u; ++k) run(PLAT_LVL_ADMIN, coef, 2, false);
    CHECK(plat_cal_history(PLAT_CAL_HIST - 1u, &r));
    CHECK(!plat_cal_history(PLAT_CAL_HIST, &r));
}

static void t_factory_revert(void) {                       /* CIP-18.3 */
    setup();
    const float f[2] = { 0.2f, 0.98f }, g[2] = { -1.0f, 1.2f };
    run(PLAT_LVL_FACTORY, f, 2, true);
    plat_cal_record_t r;
    CHECK(plat_cal_factory(0, &r));
    CHECK_EQ(r.kind, PLAT_CAL_FACTORY);
    CHECK(plat_cal_in_force(0, &r));
    CHECK_EQ(r.kind, PLAT_CAL_FACTORY);
    run(PLAT_LVL_ADMIN, g, 2, true);
    CHECK(span == 1.2f);
    CHECK(plat_cal_factory(0, &r));
    CHECK(r.coef[1] == 0.98f);                             /* field calibration left it alone */
    CHECK_EQ(plat_cal_revert(0, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_ACCESS);
    CHECK_EQ(plat_cal_revert(0, PLAT_SRC_LOCAL, PLAT_LVL_FACTORY), PERR_OK);
    scan(1);
    CHECK(zero == 0.2f && span == 0.98f);
    CHECK(plat_cal_in_force(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_REVERTED);
    CHECK_EQ(r.kind, PLAT_CAL_FACTORY);
    CHECK(plat_cal_history(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_REVERTED);
    /* calibration level (setting 321, factory only): operators allowed */
    plat_cmd_t c = { .id = PARAM_ID_CAL_LEVEL, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_ADMIN, .kind = PLAT_VAL_U32, .v.u = PLAT_LVL_OPERATOR };
    CHECK_EQ(param_apply(&c), PERR_ACCESS);
    c.level = PLAT_LVL_FACTORY;
    CHECK_EQ(param_apply(&c), PERR_OK);
    CHECK_EQ(plat_cal_check(PLAT_CAL_CMD_BEGIN, 0, PLAT_LVL_OPERATOR), PERR_OK);
    /* no factory record: nothing to revert to */
    setup();
    CHECK_EQ(plat_cal_revert(0, PLAT_SRC_LOCAL, PLAT_LVL_FACTORY), PERR_INTERLOCK);
}

static void t_states(void) {
    setup();
    plat_app_state(PLAT_ST_WARMING_UP);
    scan(1);
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_INTERLOCK);
    CHECK(!strcmp(plat_cal_refusal(), "not allowed in WARMING UP"));
    plat_app_state(PLAT_ST_MEASURING);
    scan(1);
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);
    /* the state changed before the scan applied it: refused there too */
    plat_state_safe_mode(true);
    plat_state_scan(bsp_ms());
    scan(1);
    CHECK(!plat_cal_active(NULL));
    plat_state_safe_mode(false);
    scan(1);
    CHECK_EQ(plat_cal_check(PLAT_CAL_CMD_BEGIN, 0, PLAT_LVL_ADMIN), PERR_OK);
}

static void t_survives(void) {                             /* CIP-18.5 */
    setup();
    const float f[2] = { 0.2f, 0.98f }, g[2] = { -1.0f, 1.2f };
    run(PLAT_LVL_FACTORY, f, 2, true);
    run(PLAT_LVL_ADMIN, g, 2, true);
    CHECK_EQ(plat_cal_begin(0, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);
    scan(1);
    save();                                                /* power lost part way through a calibration */
    zero = 9.0f; span = 0.5f;
    boot();
    plat_cal_record_t r;
    CHECK(zero == -1.0f && span == 1.2f);
    CHECK(plat_cal_factory(0, &r));
    CHECK(r.coef[0] == 0.2f);
    CHECK(plat_cal_in_force(0, &r));
    CHECK_EQ(r.kind, PLAT_CAL_FIELD);
    CHECK(!plat_cal_active(NULL));
    CHECK(plat_cal_history(0, &r));
    CHECK_EQ(r.outcome, PLAT_CAL_ABORTED);                 /* the interrupted one */
    CHECK(plat_cal_history(2, &r));
    CHECK_EQ(r.kind, PLAT_CAL_FACTORY);
    param_set_defaults(PARAM_USER_ONLY);                   /* restore defaults */
    CHECK(plat_cal_in_force(0, &r));
    CHECK(span == 1.2f);
}

static void t_due(void) {
    setup();
    plat_cal_record_t r;
    CHECK_EQ(plat_cal_due(0), 0u);                         /* nothing in force */
    const float g[2] = { -1.0f, 1.2f };
    run(PLAT_LVL_ADMIN, g, 2, true);
    CHECK(plat_cal_in_force(0, &r));
    CHECK_EQ(plat_cal_due(0), r.time + 30u * 86400u);
    scan(25);
    CHECK(!plat_cal_any_due());
    CHECK(!alarm_active(ALARM_ID_CAL_DUE));
    set_clock(2026, 11, 26);                               /* 31 days on */
    scan(25);
    CHECK(plat_cal_any_due());
    CHECK(plat_cal_overdue(0));
    CHECK(alarm_active(ALARM_ID_CAL_DUE));
    run(PLAT_LVL_ADMIN, g, 2, true);
    scan(25);
    CHECK(!plat_cal_any_due());
    CHECK(!alarm_active(ALARM_ID_CAL_DUE));
}

void test_cal_all(void) {
    RUN(t_table); RUN(t_field_pass); RUN(t_fail_abort); RUN(t_factory_revert); RUN(t_states); RUN(t_survives); RUN(t_due);
    plat_cal_reset_for_test();
    plat_state_reset_for_test();
    set_clock(0, 0, 0);
}
