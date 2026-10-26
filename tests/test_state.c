/* test_state.c - the instrument state and the quality of live values (CIP-09, CIP-10):
 * the table check, precedence, the quality each state gives, stale, range, own quality,
 * array elements, the state/action refusals, maintenance through parameter 301 and the
 * supervisory link age. */
#include <math.h>
#include <string.h>
#include "tst.h"
#include "plat_state.h"
#include "plat_alarm.h"
#include "param.h"
#include "fw_update.h"
#include "bsp_host.h"
#include "param_store.h"
#include "nvm.h"
#include "plat_diag.h"
#include "plat_config.h"

extern uint8_t plat_meas_q[];

static float    conc = 12.5f;
static int16_t  temps[4];
static char     label[8];
static bool     stop_in;

static const param_def_t pdefs[] = {
    PARAM_REAL(1200, "conc", conc, -1000.0f, 1000.0f, 12.5f, "mg/m3", PLAT_LVL_VIEW, PF_RO),
    PARAM_ARRAY_INT(1201, "temps", temps, CT_I16, -500, 500, PLAT_LVL_VIEW, PF_RO),
    PARAM_TEXT(1202, "label", label, NULL, PLAT_LVL_ADMIN, 0),
};
enum { A_STOPPER = 150 };
static const alarm_def_t adefs[] = {
    { .id = A_STOPPER, .severity = AL_SEV_FAULT, .flags = AL_STOPS, .name = "PUMP_FAIL", .text = "Pump failed" },
};
static const plat_meas_def_t meas[] = {
    { .id = 1200, .index = PLAT_MEAS_NO_INDEX, .max_age_ms = 5000, .lo = 0.0f, .hi = 500.0f },
    { .id = 1201, .index = 2, .max_age_ms = 0, .lo = -40.0f, .hi = 150.0f },
};

static void scan(unsigned n) {          /* n scans of 10 ms */
    for (unsigned k = 0; k < n; ++k) {
        bsp_host_advance_ms(10);
        alarm_set(A_STOPPER, stop_in);
        alarm_scan(bsp_ms());
        plat_state_scan(bsp_ms());
    }
    param_publish();
}

static uint8_t   fram[128u * 1024u];
static nvm_ram_t ram;
static nvm_dev_t dev;

/* valid calibration data, so CAL_INVALID does not force FAULT */
static void calibrated(void) {
    memset(fram, 0, sizeof fram);
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK(param_store_init(&dev, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    plat_cmd_t c = { .id = PARAM_ID_HW_REV, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_FACTORY, .kind = PLAT_VAL_U32, .v.u = 1 };
    CHECK_EQ(param_apply(&c), PERR_OK);
    for (uint32_t t = 1; t <= 2; ++t) {
        param_store_poll(t * STORE_SAVE_PERIOD_MS);
        param_store_scan_service();
        param_store_poll(t * STORE_SAVE_PERIOD_MS);
    }
    CHECK(param_store_calibration_valid());
}

static void setup(void) {
    bsp_host_reset();
    bsp_host.ms = 1000;
    plat_state_reset_for_test();
    CHECK_EQ(param_init(pdefs, sizeof pdefs / sizeof pdefs[0]), PERR_OK);
    param_bind(NULL, NULL);
    calibrated();
    CHECK_EQ(alarm_init(adefs, 1, 0), 0);
    stop_in = false;
    conc = 12.5f; temps[2] = 20;
    CHECK_EQ(plat_state_init(meas, 2), 0);
}

extern uint16_t plat_board_rev_tag;      /* fw_update.c: what the straps read */
static void t_hw_rev(void) {              /* CIP-01.3: alarm 22 when the straps disagree with the factory hw_rev */
    setup();                              /* the factory record says hw_rev 1 */
    plat_board_rev_tag = 1;
    param_publish();                      /* the alarm poll reads hw_rev as published */
    scan(21);
    CHECK(!alarm_active(ALARM_ID_HW_REV));
    plat_board_rev_tag = 2;
    scan(42);
    CHECK(alarm_active(ALARM_ID_HW_REV));
    plat_board_rev_tag = 0xFFFFu;         /* no straps: nothing to compare */
    scan(42);
    CHECK(!alarm_active(ALARM_ID_HW_REV));
}

static void t_table(void) {
    setup();
    plat_meas_def_t bad[2] = { meas[0], meas[0] };
    CHECK_EQ(plat_state_init(bad, 2), 2);                              /* duplicate */
    bad[0].id = 4242;
    CHECK_EQ(plat_state_init(bad, 1), 1);                              /* no such ID */
    bad[0].id = 1202;
    CHECK_EQ(plat_state_init(bad, 1), 1);                              /* text is not a measurement */
    bad[0] = meas[1]; bad[0].index = 4;
    CHECK_EQ(plat_state_init(bad, 1), 1);                              /* past the end of the array */
    bad[0].index = PLAT_MEAS_NO_INDEX;
    CHECK_EQ(plat_state_init(bad, 1), 1);                              /* an array needs an index */
    CHECK_EQ(plat_state_init(meas, 2), 0);
    CHECK_EQ(plat_state(), PLAT_ST_INITIALISING);
    CHECK_EQ(plat_meas_count(), 2u);
}

static void t_states(void) {
    setup();
    scan(21);                                                          /* alarm_scan polls the platform every 200 ms */
    CHECK_EQ(plat_state(), PLAT_ST_READY);                             /* nothing set by the application yet */
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_UNAVAILABLE);
    plat_app_state(PLAT_ST_WARMING_UP);
    scan(1);
    CHECK_EQ(plat_state(), PLAT_ST_WARMING_UP);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_WARMUP);
    CHECK(!plat_state_allows(PLAT_ACT_CALIBRATE));
    plat_app_state(PLAT_ST_MEASURING);
    scan(1);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_UNAVAILABLE);   /* measuring, but never updated */
    plat_meas_update(0, PLAT_Q_VALID);
    plat_meas_update(1, PLAT_Q_VALID);
    scan(1);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_VALID);
    CHECK_EQ(param_quality(1201, 2), PLAT_Q_VALID);
    CHECK_EQ(plat_meas_q[0], PLAT_Q_VALID);                            /* the Modbus status block */

    /* maintenance through parameter 301, as the touchscreen and web page do */
    plat_cmd_t c = { .id = PARAM_ID_MAINTENANCE, .src = PLAT_SRC_LOCAL, .kind = PLAT_VAL_BOOL, .level = PLAT_LVL_OPERATOR, .v.b = true };
    CHECK_EQ(param_apply(&c), PERR_OK);
    scan(1);
    CHECK_EQ(plat_state(), PLAT_ST_MAINTENANCE);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_MAINT);
    CHECK(!strcmp(plat_state_name(plat_state()), "MAINTENANCE"));
    plat_val_t v;
    CHECK(param_read(PARAM_ID_STATE, &v) && v.u == PLAT_ST_MAINTENANCE);

    /* calibration beats maintenance; it refuses a firmware restart and restore defaults */
    plat_app_state(PLAT_ST_CALIBRATION);
    scan(1);
    CHECK_EQ(plat_state(), PLAT_ST_CALIBRATION);
    CHECK_EQ(param_quality(1201, 2), PLAT_Q_CAL);
    CHECK(!plat_state_allows(PLAT_ACT_FW_RESTART));
    CHECK(!strcmp(plat_state_refusal(), "not allowed in CALIBRATION"));
    plat_cmd_t rd = { .id = PARAM_ID_RESTORE_DEFAULTS, .src = PLAT_SRC_LOCAL, .kind = PLAT_VAL_BOOL, .level = PLAT_LVL_ADMIN, .v.b = true };
    CHECK_EQ(param_apply(&rd), PERR_INTERLOCK);

    /* an AL_STOPS alarm forces FAULT over everything but safe mode */
    stop_in = true;
    scan(1);
    CHECK_EQ(plat_state(), PLAT_ST_FAULT);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_FAULT);
    plat_state_safe_mode(true);
    scan(1);
    CHECK_EQ(plat_state(), PLAT_ST_SAFE_MODE);
    CHECK(!plat_state_allows(PLAT_ACT_APP_LOGIC));
    CHECK(!strcmp(plat_state_key(plat_state()), "SAFE_MODE"));
    plat_state_safe_mode(false);
    stop_in = false;
    scan(1);
    plat_app_state(PLAT_ST_MEASURING);                                 /* calibration done */
    c.v.b = false;
    CHECK_EQ(param_apply(&c), PERR_OK);
    scan(1);
    CHECK_EQ(plat_state(), PLAT_ST_MEASURING);
    CHECK(plat_state_allows(PLAT_ACT_FW_RESTART));
    CHECK_EQ(param_apply(&rd), PERR_OK);
}

static void t_cal_invalid(void) {
    setup();
    memset(fram, 0, sizeof fram);                                      /* no calibration data at all */
    CHECK(param_store_init(&dev, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    plat_app_state(PLAT_ST_MEASURING);
    plat_meas_update(0, PLAT_Q_VALID);
    scan(21);
    CHECK_EQ(plat_state(), PLAT_ST_FAULT);                             /* CAL_INVALID: measurement blocked */
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_FAULT);
    CHECK(!plat_state_allows(PLAT_ACT_CALIBRATE));
}

static void t_quality(void) {
    setup();
    plat_app_state(PLAT_ST_MEASURING);
    plat_meas_update(0, PLAT_Q_VALID);
    plat_meas_update(1, PLAT_Q_VALID);
    scan(1);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_VALID);
    /* stale: the application stops updating it (max 5 s); measurement 1 never goes stale */
    scan(499);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_VALID);
    scan(2);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_STALE);
    CHECK_EQ(param_quality(1201, 2), PLAT_Q_VALID);
    plat_meas_update(0, PLAT_Q_VALID);
    scan(1);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_VALID);
    /* range: outside lo..hi, or NaN */
    conc = 600.0f; scan(1);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_RANGE);
    conc = NAN; scan(1);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_RANGE);
    conc = 500.0f; scan(1);
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_VALID);   /* the limits themselves are valid */
    temps[2] = -41; scan(1);
    CHECK_EQ(param_quality(1201, 2), PLAT_Q_RANGE);                    /* an array element */
    temps[2] = 20;
    /* the value's own quality wins over stale and range */
    plat_meas_update(1, PLAT_Q_FAULT);
    scan(1);
    CHECK_EQ(param_quality(1201, 2), PLAT_Q_FAULT);
    CHECK(!strcmp(plat_quality_name(PLAT_Q_FAULT), "fault"));
    CHECK(!strcmp(plat_quality_code(PLAT_Q_RANGE), "range"));
    CHECK(!strcmp(plat_quality_name(PLAT_Q_RANGE), "out of range"));
    CHECK(!strcmp(plat_quality_name(PLAT_Q_VALID), ""));
    /* readers */
    CHECK_EQ(param_quality(1202, PLAT_MEAS_NO_INDEX), PLAT_Q_VALID);   /* not a measurement */
    CHECK(param_has_quality(1201, 2));
    CHECK(param_has_quality(1201, PLAT_MEAS_NO_INDEX));                /* some element of it */
    CHECK(!param_has_quality(1201, 1));
    CHECK(!param_has_quality(1202, PLAT_MEAS_NO_INDEX));
    uint8_t qb[2];
    CHECK_EQ(param_read_blob(PARAM_ID_MEAS_Q, 0, qb, 2), PERR_OK);
    CHECK(qb[0] == PLAT_Q_VALID && qb[1] == PLAT_Q_FAULT);
    plat_meas_update(9, PLAT_Q_VALID);                                 /* out of range: ignored */
}

static void t_request_and_link(void) {
    setup();
    CHECK_EQ(plat_state_request(PLAT_ST_FAULT, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);
    CHECK_EQ(plat_state_request(PLAT_ST_MAINTENANCE, PLAT_SRC_LOCAL, PLAT_LVL_VIEW), PERR_ACCESS);
    CHECK_EQ(plat_state_request(PLAT_ST_MAINTENANCE, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_BUSY);   /* no queue in this test */
    /* the supervisory link: none yet, then the age of the last Modbus or web request */
    CHECK_EQ(plat_link_age_ms(bsp_ms()), UINT32_MAX);
    param_comms_activity(PLAT_SRC_LOCAL, bsp_ms());                    /* the touchscreen is not a supervisory link */
    CHECK_EQ(plat_link_age_ms(bsp_ms()), UINT32_MAX);
    param_comms_activity(PLAT_SRC_MODBUS, bsp_ms());
    bsp_host_advance_ms(1234);
    CHECK_EQ(plat_link_age_ms(bsp_ms()), 1234u);
}

/* CIP-08.5: crashes in a row with little running between them */
static void crash(bsp_reset_cause_t how, uint32_t ran_ms) {
    plat_diag_tick(ran_ms);
    if (how == BSP_RESET_SOFTWARE) plat_diag_fault(PLAT_FAULT_TASK_SILENT, 1, "scan", 0, ran_ms);
    plat_diag_boot(how);
}

static void t_reboot_loop(void) {
    plat_diag_boot(BSP_RESET_POWER_ON);
    CHECK(!plat_diag_reboot_loop());
    crash(BSP_RESET_IWDG, 30000);
    crash(BSP_RESET_SOFTWARE, 30000);                                   /* a fault, then the reset */
    CHECK(!plat_diag_reboot_loop());
    CHECK_EQ(plat_diag.loop_crashes, 2u);
    crash(BSP_RESET_IWDG, 30000);
    CHECK(plat_diag_reboot_loop());                                     /* 3 in 90 s of running */
    CHECK_EQ(plat_diag.fault_armed, 0u);
    plat_diag_boot(BSP_RESET_POWER_ON);                                 /* a power cycle clears it */
    CHECK(!plat_diag_reboot_loop());

    /* a clean software restart (firmware update, no fault recorded) is not a crash, and ends a series */
    crash(BSP_RESET_IWDG, 1000); crash(BSP_RESET_IWDG, 1000);
    plat_diag_tick(5000); plat_diag_boot(BSP_RESET_SOFTWARE);
    CHECK_EQ(plat_diag.loop_crashes, 0u);
    crash(BSP_RESET_IWDG, 1000);
    CHECK(!plat_diag_reboot_loop());

    /* crashes far apart are not a loop: the window counts running time */
    plat_diag_boot(BSP_RESET_POWER_ON);
    crash(BSP_RESET_IWDG, PLAT_LOOP_WINDOW_MS / 2);
    crash(BSP_RESET_IWDG, PLAT_LOOP_WINDOW_MS / 2);
    crash(BSP_RESET_IWDG, PLAT_LOOP_WINDOW_MS / 2);                    /* over the window: a new series */
    CHECK(!plat_diag_reboot_loop());
    CHECK_EQ(plat_diag.loop_crashes, 1u);
    /* a boot that runs for the whole window clears the count */
    crash(BSP_RESET_IWDG, 1000);
    CHECK_EQ(plat_diag.loop_crashes, 2u);
    plat_diag_tick(PLAT_LOOP_WINDOW_MS + 1u);
    CHECK_EQ(plat_diag.loop_crashes, 0u);
    CHECK(plat_diag_valid());
    plat_diag_boot(BSP_RESET_POWER_ON);
}

/* safe mode raises its alarm and keeps the application's logic from running */
static void t_safe_mode_alarm(void) {
    setup();
    plat_state_safe_mode(true);
    scan(1);
    CHECK_EQ(plat_state(), PLAT_ST_SAFE_MODE);
    scan(21);
    CHECK(alarm_active(ALARM_ID_SAFE_MODE));
    CHECK_EQ(param_quality(1200, PLAT_MEAS_NO_INDEX), PLAT_Q_UNAVAILABLE);
    plat_state_safe_mode(false);
    scan(21);
    CHECK(!alarm_active(ALARM_ID_SAFE_MODE));
}

void test_state_all(void) {
    RUN(t_table); RUN(t_states); RUN(t_cal_invalid); RUN(t_quality); RUN(t_request_and_link); RUN(t_reboot_loop); RUN(t_safe_mode_alarm); RUN(t_hw_rev);
    plat_state_reset_for_test();
}
