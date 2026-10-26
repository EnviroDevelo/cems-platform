/* test_service.c - the unit's service history (CIP-22.2, 22.4, 23.4, 05.4): operating hours in
 * 0.1 h steps, the service log (newest first, wraps, restarts "since service"), the test record,
 * the factory-level gate, survival across a restart and restore defaults, a backup restore that
 * does not copy them into another unit, and the configuration checksum. */
#include <string.h>
#include "tst.h"
#include "plat_service.h"
#include "param.h"
#include "param_store.h"
#include "bsp_host.h"
#include "nvm.h"
#include "plat_config.h"

static int32_t setpoint = 10;
static float   secret_ish = 1.0f;
static const param_def_t pdefs[] = {
    PARAM_INT (1400, "setpoint", setpoint, CT_I32, 0, 100, 10, PLAT_LVL_OPERATOR, PF_PERSIST, NULL),
    PARAM_REAL(1401, "coef", secret_ish, 0.0f, 10.0f, 1.0f, NULL, PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY),
};

static uint8_t   fram[128u * 1024u];
static nvm_ram_t ram;
static nvm_dev_t dev;
static uint32_t  store_ms;

static void save(void) {
    for (int k = 0; k < 2; ++k) {
        store_ms += STORE_SAVE_PERIOD_MS;
        param_store_poll(store_ms);
        param_store_scan_service();
        param_store_poll(store_ms);
    }
}

static void boot(void) {
    plat_service_reset_for_test();
    CHECK_EQ(param_init(pdefs, sizeof pdefs / sizeof pdefs[0]), PERR_OK);
    param_bind(NULL, NULL);
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK(param_store_init(&dev, NULL, NULL, 0, 1, NULL));
    param_store_boot();
}

static void run(uint32_t ms) {                      /* the scan, in 100 ms steps */
    for (uint32_t t = 0; t < ms; t += 100u) { bsp_host_advance_ms(100); plat_service_scan(bsp_ms()); }
}

static void setup(void) {
    bsp_host_reset();
    bsp_host.ms = 1000;
    store_ms = 0;
    memset(fram, 0, sizeof fram);
    boot();
    plat_service_scan(bsp_ms());
}

static void t_hours(void) {
    setup();
    CHECK_EQ(plat_service_hours_x10(), 0u);
    run(359900);
    CHECK_EQ(plat_service_hours_x10(), 0u);
    run(200);
    CHECK_EQ(plat_service_hours_x10(), 1u);         /* 6 minutes = 0.1 h */
    run(3u * 360000u);
    CHECK_EQ(plat_service_hours_x10(), 4u);
    CHECK_EQ(plat_service_since_x10(), 4u);
    param_publish();
    plat_val_t v;
    CHECK(param_read(PARAM_ID_RUN_HOURS, &v) && v.u == 4u);
}

static void t_log(void) {
    setup();
    run(720000);
    CHECK_EQ(plat_service_log("Sam", "Pump replaced", PLAT_SRC_WEB, PLAT_LVL_ADMIN), -3);
    CHECK_EQ(plat_service_log("", "x", PLAT_SRC_WEB, PLAT_LVL_FACTORY), -2);
    CHECK_EQ(plat_service_log("Sam", "Pump replaced", PLAT_SRC_WEB, PLAT_LVL_FACTORY), 0);
    CHECK_EQ(plat_service_log("Sam", "again", PLAT_SRC_WEB, PLAT_LVL_FACTORY), -1);   /* one waiting at a time */
    run(100);
    plat_svc_entry_t e;
    CHECK(plat_service_entry(0, &e));
    CHECK(!strcmp(e.tech, "Sam") && !strcmp(e.work, "Pump replaced"));
    CHECK_EQ(e.level, PLAT_LVL_FACTORY);
    CHECK(!plat_service_entry(1, &e));
    CHECK_EQ(plat_service_hours_x10(), 2u);
    CHECK_EQ(plat_service_since_x10(), 0u);         /* restarted by the service */
    run(360000);
    CHECK_EQ(plat_service_since_x10(), 1u);
    /* a long work text is cut, not overrun */
    CHECK_EQ(plat_service_log("A technician with a long name", "0123456789012345678901234567890123456789XYZ",
                              PLAT_SRC_LOCAL, PLAT_LVL_FACTORY), 0);
    run(100);
    CHECK(plat_service_entry(0, &e));
    CHECK_EQ(strlen(e.tech), sizeof e.tech - 1u);
    CHECK_EQ(strlen(e.work), sizeof e.work - 1u);
    /* the log keeps the newest PLAT_SVC_LOG */
    char w[8];
    for (unsigned k = 0; k < PLAT_SVC_LOG + 3u; ++k) {
        w[0] = (char)('a' + k); w[1] = 0;
        CHECK_EQ(plat_service_log("T", w, PLAT_SRC_WEB, PLAT_LVL_FACTORY), 0);
        run(100);
    }
    CHECK(plat_service_entry(0, &e)); CHECK_EQ(e.work[0], (char)('a' + PLAT_SVC_LOG + 2u));
    CHECK(plat_service_entry(PLAT_SVC_LOG - 1u, &e)); CHECK_EQ(e.work[0], (char)('a' + 3u));
    CHECK(!plat_service_entry(PLAT_SVC_LOG, &e));
}

static void t_test_record(void) {
    setup();
    plat_test_record_t t;
    CHECK(!plat_service_test_record(&t));
    const uint8_t ok[3] = { PLAT_TEST_PASS, PLAT_TEST_PASS, PLAT_TEST_PASS }, bad[3] = { 1, 2, 1 }, wrong[1] = { 7 };
    CHECK_EQ(plat_service_test("ST1", ok, 3, PLAT_SRC_WEB, PLAT_LVL_ADMIN), -3);
    CHECK_EQ(plat_service_test("ST1", wrong, 1, PLAT_SRC_WEB, PLAT_LVL_FACTORY), -2);
    CHECK_EQ(plat_service_test("ST1", ok, 0, PLAT_SRC_WEB, PLAT_LVL_FACTORY), -2);
    CHECK_EQ(plat_service_test("ST1", bad, 3, PLAT_SRC_WEB, PLAT_LVL_FACTORY), 0);
    run(100);
    CHECK(plat_service_test_record(&t));
    CHECK(!t.passed); CHECK_EQ(t.n, 3); CHECK_EQ(t.result[1], PLAT_TEST_FAIL);
    CHECK(!strcmp(t.station, "ST1"));
    CHECK_EQ(plat_service_test("ST2", ok, 3, PLAT_SRC_WEB, PLAT_LVL_FACTORY), 0);
    run(100);
    CHECK(plat_service_test_record(&t));
    CHECK(t.passed);
}

static void t_survives(void) {
    setup();
    run(3u * 360000u);
    CHECK_EQ(plat_service_log("Sam", "Annual service", PLAT_SRC_WEB, PLAT_LVL_FACTORY), 0);
    const uint8_t ok[2] = { 1, 1 };
    CHECK_EQ(plat_service_test("ST1", ok, 2, PLAT_SRC_WEB, PLAT_LVL_FACTORY), 0);
    run(360000);
    save();
    static uint8_t backup[64 * 1024];
    uint32_t n = param_store_export(backup, sizeof backup);
    CHECK(n > 0);
    boot();                                          /* power cycle */
    plat_service_scan(bsp_ms());
    CHECK_EQ(plat_service_hours_x10(), 4u);
    CHECK_EQ(plat_service_since_x10(), 1u);
    plat_svc_entry_t e;
    CHECK(plat_service_entry(0, &e) && !strcmp(e.work, "Annual service"));
    plat_test_record_t t;
    CHECK(plat_service_test_record(&t) && t.passed);
    param_set_defaults(PARAM_USER_ONLY);             /* restore defaults leaves them */
    CHECK(plat_service_entry(0, &e));
    /* the backup restored into another (new) unit does not give it this unit's history */
    memset(fram, 0, sizeof fram);
    boot();
    plat_service_scan(bsp_ms());
    CHECK_EQ(param_store_import(backup, n, PLAT_LVL_FACTORY), 0);
    param_store_scan_service();
    CHECK(!plat_service_entry(0, &e));
    CHECK(!plat_service_test_record(&t));
    CHECK_EQ(plat_service_hours_x10(), 0u);
}

static void t_config_crc(void) {
    setup();
    run(1000);
    uint32_t a = plat_config_crc();
    CHECK(a != 0);
    setpoint = 11;
    run(1000);
    uint32_t b = plat_config_crc();
    CHECK(b != a);                                   /* a setting changed */
    secret_ish = 2.0f;
    run(1000);
    CHECK_EQ(plat_config_crc(), b);                  /* factory values (calibration) are not configuration */
    setpoint = 10;
    run(1000);
    CHECK_EQ(plat_config_crc(), a);                  /* same settings, same checksum */
}

void test_service_all(void) {
    RUN(t_hours); RUN(t_log); RUN(t_test_record); RUN(t_survives); RUN(t_config_crc);
    plat_service_reset_for_test();
}
