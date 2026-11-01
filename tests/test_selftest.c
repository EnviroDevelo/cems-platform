/* test_selftest.c - the start-up self-test list (CIP-11.1): every check's pass, fail and
 * skip, the running-image hash against its bank header, the deferred checks, a product's
 * own checks, the failed bits (parameter 333), the event and alarm 23. And the factory board
 * test built on it (CIP-23.3, plat_boardtest.h). */
#include <string.h>
#include <math.h>
#include "tst.h"
#include "plat_selftest.h"
#include "plat_boardtest.h"
#include "plat_service.h"
#include "plat_alarm.h"
#include "plat_log.h"
#include "log_csv.h"
#include "param.h"
#include "param_store.h"
#include "fw_update.h"
#include "fw_flash_sim.h"
#include "plat_sha256.h"
#include "bsp_host.h"
#include "nvm.h"

static const param_def_t pdefs[] = { 0 };

static uint8_t   fram[128u * 1024u];
static nvm_ram_t ram;
static nvm_dev_t dev;
#define NOR_SIZE (64u * 1024u)
static uint8_t   nor_mem[NOR_SIZE];
static nor_dev_t nor;
static nor_sim_t sim;
#define BANK (64u * 1024u)
static uint8_t b0[BANK], b1[BANK];
static fw_flash_t fl;
static fw_flash_sim_t fs;

static int pump_result;
static int pump(void) { return pump_result; }
static int sensor(void) { return -1; }
static const plat_selftest_def_t app_tests[] = { { "pump", pump }, { "sensor", sensor } };

/* an image the update left in the running bank: payload, and its header in the last sector */
static void running_image(const char *ver, bool damage) {
    uint8_t *a = fw_flash_sim_active(&fs);
    for (uint32_t k = 0; k < 20000u; ++k) a[k] = (uint8_t)(k * 7u + 3u);
    fw_header_t h;
    memset(&h, 0, sizeof h);
    h.magic = FW_MAGIC; h.hdr_size = FW_HDR_SIZE; h.hdr_version = 1; h.product = PLAT_FW_PRODUCT;
    h.version = fw_parse_version(ver);
    strncpy(h.version_str, ver, sizeof h.version_str - 1);
    h.image_size = 20000u;
    sha256_t s; sha256_init(&s); sha256_update(&s, a, 20000u); sha256_final(&s, h.sha256);
    h.hdr_crc = plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc));
    memcpy(a + BANK - fl.sector, &h, sizeof h);
    if (damage) a[12345] ^= 0x10u;
}

static void calibrated(void) {
    plat_cmd_t c = { .id = PARAM_ID_HW_REV, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_FACTORY, .kind = PLAT_VAL_U32, .v.u = 1 };
    CHECK_EQ(param_apply(&c), PERR_OK);
    for (uint32_t t = 1; t <= 2; ++t) {
        param_store_poll(t * STORE_SAVE_PERIOD_MS);
        param_store_scan_service();
        param_store_poll(t * STORE_SAVE_PERIOD_MS);
    }
}

static void setup(bool cal, bool log_flash) {
    bsp_host_reset();
    bsp_host.ms = 1000;
    plat_selftest_reset_for_test();
    CHECK_EQ(param_init(pdefs, 0), PERR_OK);
    param_bind(NULL, NULL);
    memset(fram, 0, sizeof fram);
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK(param_store_init(&dev, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    if (cal) calibrated();
    nor_sim_init(&nor, &sim, nor_mem, NOR_SIZE, true, true);
    plat_log_reset_for_test();
    plat_log_cfg_t lc = { .nor = log_flash ? &nor : NULL, .fram = &dev, .fram_addr = STORE_META_ADDR, .fw_version = "1.0.0",
                          .reset_cause = BSP_RESET_POWER_ON, .unix_now = 1792411200u };
    plat_log_init(&lc, bsp_host.ms);
    memset(b0, 0xFF, sizeof b0); memset(b1, 0xFF, sizeof b1);
    fw_flash_sim_init(&fl, &fs, b0, b1, BANK);
    fw_reset_for_test();
    fw_init(&fl, NULL, 0, "1.0.0");
    CHECK_EQ(alarm_init(NULL, 0, 0), 0);
}

static void run(uint32_t ms) {          /* the store task, every 20 ms; the RTC ticks with it */
    for (uint32_t t = 0; t < ms; t += 20u) {
        bsp_host_advance_ms(20);
        if (bsp_host.ms % 1000u == 0) {
            bsp_host.rtc.seconds = (uint8_t)((bsp_host.rtc.seconds + 1u) % 60u);
            if (bsp_host.rtc.seconds == 0) bsp_host.rtc.minutes++;
        }
        plat_selftest_poll(bsp_host.ms);
    }
}

static uint8_t result(unsigned i) { uint8_t r = 0xFF; plat_selftest_get(i, NULL, &r); return r; }

/* the text of the newest event */
static uint8_t rd[NOR_SIZE];
static bool last_event(log_event_t *e) {
    for (int k = 0; k < 4; ++k) plat_log_poll(bsp_host.ms);
    log_ring_info_t inf;
    if (!plat_log_info(LOG_RING_EVENTS, &inf) || !inf.used) return false;
    if (plat_log_read_start(LOG_RING_EVENTS, inf.oldest, inf.used, rd) != 0) return false;
    plat_log_poll(bsp_host.ms);
    int32_t res; if (!plat_log_read_done(&res)) return false;
    bool got = false;
    for (uint32_t k = 0; k < inf.used; ++k) { log_event_t x; if (plat_log_decode_event(rd + k * LOG_SLOT, &x)) { *e = x; got = true; } }
    return got;
}

static void t_march(void) {
    static uint32_t blk[64];
    CHECK(plat_ram_march(blk, 64));
    CHECK_EQ(blk[0], 0xA5A5A5A5u);
}

static void t_all_pass(void) {
    setup(true, true);
    running_image("1.0.0", false);
    pump_result = 1;
    CHECK_EQ(plat_selftest_init(app_tests, 2, bsp_host.ms), 0);
    CHECK_EQ(plat_selftest_count(), PLAT_STEST_N + 2u);
    CHECK_EQ(result(PLAT_STEST_RAM), PLAT_STR_PASS);         /* at init, before the tasks */
    run(1000);
    CHECK_EQ(result(PLAT_STEST_IMAGE), PLAT_STR_PASS);       /* 20 KB hashed within the first second */
    CHECK_EQ(result(PLAT_STEST_SETTINGS), PLAT_STR_PASS);
    CHECK_EQ(result(PLAT_STEST_CALIBRATION), PLAT_STR_PASS);
    CHECK_EQ(result(PLAT_STEST_LOG_FLASH), PLAT_STR_PASS);
    CHECK_EQ(result(PLAT_STEST_RTC), PLAT_STR_PENDING);
    plat_selftest_report(PLAT_STEST_NET_PHY, true);          /* the Ethernet driver */
    run(3000);
    CHECK_EQ(result(PLAT_STEST_RTC), PLAT_STR_PASS);
    CHECK(!plat_selftest_done());                            /* the deferred checks wait 10 s */
    run(PLAT_STEST_DEFER_MS);
    CHECK(plat_selftest_done());
    CHECK_EQ(plat_selftest_failed(), 0u);
    CHECK_EQ(result(PLAT_STEST_NET_PHY), PLAT_STR_PASS);
    CHECK_EQ(result(PLAT_STEST_IO_PORT), PLAT_STR_SKIP);     /* nothing used it */
    CHECK_EQ(result(PLAT_STEST_DISPLAY), PLAT_STR_SKIP);
    CHECK_EQ(result(PLAT_STEST_N), PLAT_STR_PASS);
    CHECK_EQ(result(PLAT_STEST_N + 1u), PLAT_STR_SKIP);
    const char *nm = NULL;
    CHECK(plat_selftest_get(PLAT_STEST_N + 1u, &nm, NULL) && !strcmp(nm, "sensor"));
    CHECK(plat_selftest_get(PLAT_STEST_LOG_FLASH, &nm, NULL) && !strcmp(nm, "log_flash"));
    CHECK(!plat_selftest_get(PLAT_STEST_N + 2u, &nm, NULL));
    log_event_t e;
    CHECK(last_event(&e)); CHECK_EQ(e.code, LOG_EV_SELFTEST); CHECK_EQ(e.arg, 0u); CHECK(!strcmp(e.text, "passed"));
    alarm_scan(bsp_host.ms);
    CHECK(!alarm_active(ALARM_ID_SELFTEST));
    plat_selftest_report(PLAT_STEST_NET_PHY, false);         /* after the list is done: no change */
    CHECK_EQ(result(PLAT_STEST_NET_PHY), PLAT_STR_PASS);
}

static void t_failures(void) {
    setup(false, true);                                      /* new unit: no calibration */
    running_image("1.0.0", true);                            /* and the image damaged */
    bsp_host.rtc_valid = false;                              /* clock never set */
    pump_result = 0;
    CHECK_EQ(plat_selftest_init(app_tests, 2, bsp_host.ms), 0);
    plat_selftest_report(PLAT_STEST_NET_PHY, false);
    plat_selftest_report(PLAT_STEST_IO_PORT, false);         /* the W5500 did not answer, then did */
    plat_selftest_report(PLAT_STEST_IO_PORT, true);
    run(PLAT_STEST_DEFER_MS + 100u);
    CHECK(plat_selftest_done());
    CHECK_EQ(result(PLAT_STEST_IMAGE), PLAT_STR_FAIL);
    CHECK_EQ(result(PLAT_STEST_CALIBRATION), PLAT_STR_FAIL);
    CHECK_EQ(result(PLAT_STEST_RTC), PLAT_STR_SKIP);
    CHECK_EQ(result(PLAT_STEST_N), PLAT_STR_FAIL);
    CHECK_EQ(result(PLAT_STEST_IO_PORT), PLAT_STR_PASS);
    uint32_t want = 1u << PLAT_STEST_IMAGE | 1u << PLAT_STEST_CALIBRATION | 1u << PLAT_STEST_NET_PHY | 1u << 16;
    CHECK_EQ(plat_selftest_failed(), want);
    param_publish();
    plat_val_t v;
    CHECK(param_read(PARAM_ID_SELFTEST, &v) && v.u == want);
    log_event_t e;
    CHECK(last_event(&e)); CHECK_EQ(e.code, LOG_EV_SELFTEST); CHECK_EQ(e.arg, want);
    CHECK(!strncmp(e.text, "image calibration", 17));        /* the names, as many as fit */
    char what[32], det[64];
    log_event_text(&e, what, sizeof what, det, sizeof det);
    CHECK(!strcmp(what, "Self-test")); CHECK(!strncmp(det, "failed: image", 13));
    alarm_scan(bsp_host.ms);
    CHECK(alarm_active(ALARM_ID_SELFTEST));
    /* calibration alone: CAL_INVALID says it, SELFTEST does not repeat it */
    setup(false, true);
    running_image("1.0.0", false);
    CHECK_EQ(plat_selftest_init(NULL, 0, bsp_host.ms), 0);
    run(PLAT_STEST_DEFER_MS + 100u);
    CHECK_EQ(plat_selftest_failed(), 1u << PLAT_STEST_CALIBRATION);
    alarm_scan(bsp_host.ms);
    CHECK(!alarm_active(ALARM_ID_SELFTEST));
}

static void t_skips(void) {
    setup(true, true);
    /* programmed with the debugger: no header; then a header left by an older update */
    CHECK_EQ(plat_selftest_init(NULL, 0, bsp_host.ms), 0);
    run(100);
    CHECK_EQ(result(PLAT_STEST_IMAGE), PLAT_STR_SKIP);
    setup(true, true);
    running_image("0.9.0", false);
    CHECK_EQ(plat_selftest_init(NULL, 0, bsp_host.ms), 0);
    run(100);
    CHECK_EQ(result(PLAT_STEST_IMAGE), PLAT_STR_SKIP);
    run(PLAT_STEST_DEFER_MS);
    CHECK_EQ(result(PLAT_STEST_NET_PHY), PLAT_STR_SKIP);    /* nothing reported it */
    CHECK(plat_selftest_done()); CHECK_EQ(plat_selftest_failed(), 0u);
    /* the RTC readable but stopped */
    setup(true, true);
    CHECK_EQ(plat_selftest_init(NULL, 0, bsp_host.ms), 0);
    for (uint32_t t = 0; t < 4000u; t += 20u) { bsp_host_advance_ms(20); plat_selftest_poll(bsp_host.ms); }
    CHECK_EQ(result(PLAT_STEST_RTC), PLAT_STR_FAIL);
    /* log flash fitted but not answering */
    setup(true, false);
    CHECK_EQ(plat_selftest_init(NULL, 0, bsp_host.ms), 0);
    run(100);
    CHECK_EQ(result(PLAT_STEST_LOG_FLASH), PLAT_STR_FAIL);
    /* too many product checks is a table error */
    static const plat_selftest_def_t many[PLAT_STEST_APP_MAX + 1u] = { { "a", pump } };
    CHECK_EQ(plat_selftest_init(many, PLAT_STEST_APP_MAX + 1u, bsp_host.ms), -1);
    CHECK_EQ(plat_selftest_count(), (unsigned)PLAT_STEST_N);
    CHECK_EQ(strcmp(plat_selftest_result_name(PLAT_STR_SKIP), "skipped"), 0);
}

/* ---------------- the factory board test (CIP-23.3) ---------------- */
static int (*real_read)(nvm_dev_t *, uint32_t, void *, uint32_t);
static int (*real_write)(nvm_dev_t *, uint32_t, const void *, uint32_t);
static int alias_read(nvm_dev_t *d, uint32_t a, void *p, uint32_t n) { return real_read(d, a & 0xFFFFu, p, n); }
static int alias_write(nvm_dev_t *d, uint32_t a, const void *p, uint32_t n) { return real_write(d, a & 0xFFFFu, p, n); }

static void run_bt(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 20u) {
        bsp_host_advance_ms(20);
        if (bsp_host.ms % 1000u == 0) bsp_host.rtc.seconds = (uint8_t)((bsp_host.rtc.seconds + 1u) % 60u);
        plat_selftest_poll(bsp_host.ms);
        plat_boardtest_poll(bsp_host.ms);
    }
}

static void t_boardtest(void) {
    static uint8_t before[sizeof fram];
    plat_bt_status_t st;
    setup(true, true);
    running_image("1.0.0", false);
    plat_boardtest_reset_for_test();
    plat_service_reset_for_test();
    CHECK_EQ(plat_selftest_init(NULL, 0, bsp_host.ms), 0);
    CHECK_EQ(plat_boardtest_start(PLAT_SRC_WEB, PLAT_LVL_FACTORY), -1);   /* the start-up checks first */
    plat_selftest_report(PLAT_STEST_NET_PHY, true);
    run_bt(PLAT_STEST_DEFER_MS + 100u);
    CHECK(plat_selftest_done());
    CHECK_EQ(plat_boardtest_start(PLAT_SRC_WEB, PLAT_LVL_ADMIN), -3);
    bsp_host.supply_mv = 24100; bsp_host.straps_fitted = true; bsp_host.hw_straps = 2;
    memcpy(before, fram, sizeof fram);
    CHECK_EQ(plat_boardtest_start(PLAT_SRC_WEB, PLAT_LVL_FACTORY), 0);
    CHECK_EQ(plat_boardtest_start(PLAT_SRC_WEB, PLAT_LVL_FACTORY), -1);   /* once at a time */
    plat_boardtest_status(&st); CHECK(st.running);
    run_bt(40);
    CHECK(memcmp(before, fram, sizeof fram) == 0);           /* the settings memory is as it was */
    run_bt(3000);
    plat_boardtest_status(&st);
    CHECK(!st.running); CHECK(st.done); CHECK_EQ(st.saved, 0);
    CHECK_EQ(st.result[PLAT_BT_FRAM], PLAT_TEST_PASS); CHECK(!strcmp(st.detail[PLAT_BT_FRAM], "128 KB, 1 chip"));
    CHECK_EQ(st.result[PLAT_BT_RAM], PLAT_TEST_PASS);
    CHECK_EQ(st.result[PLAT_BT_RTC], PLAT_TEST_PASS);
    CHECK_EQ(st.result[PLAT_BT_IMAGE], PLAT_TEST_PASS);
    CHECK_EQ(st.result[PLAT_BT_LOG_FLASH], PLAT_TEST_PASS);
    CHECK_EQ(st.result[PLAT_BT_NET_PHY], PLAT_TEST_PASS);
    CHECK_EQ(st.result[PLAT_BT_NET_LINK], PLAT_TEST_NOT_RUN);   /* not known on the PC */
    CHECK_EQ(st.result[PLAT_BT_IO_PORT], PLAT_TEST_NOT_RUN); CHECK(!strcmp(st.detail[PLAT_BT_IO_PORT], "not used"));
    CHECK_EQ(st.result[PLAT_BT_DISPLAY], PLAT_TEST_NOT_RUN);
    CHECK_EQ(st.result[PLAT_BT_SUPPLY], PLAT_TEST_PASS); CHECK(!strcmp(st.detail[PLAT_BT_SUPPLY], "24.1 V"));
    CHECK_EQ(st.result[PLAT_BT_HW_REV], PLAT_TEST_PASS); CHECK(!strcmp(st.detail[PLAT_BT_HW_REV], "rev 2"));
    CHECK(!strcmp(plat_boardtest_name(PLAT_BT_IO_LINK), "io_link")); CHECK(plat_boardtest_name(PLAT_BT_N) == NULL);
    for (int k = 0; k < 3; ++k) { plat_service_scan(bsp_host.ms); param_store_poll(bsp_host.ms + 2000u * (uint32_t)(k + 1)); }
    plat_test_record_t rec;
    CHECK(plat_service_test_record(&rec));
    CHECK(!strcmp(rec.station, "board")); CHECK_EQ(rec.n, PLAT_BT_N); CHECK_EQ(rec.result[PLAT_BT_SUPPLY], PLAT_TEST_PASS);

    /* faults: two addresses landing on one cell (a missing chip select / address line),
     * a low supply, the clock stopped */
    real_read = dev.read; real_write = dev.write;
    dev.read = alias_read; dev.write = alias_write;
    bsp_host.supply_mv = 18500;
    CHECK_EQ(plat_boardtest_start(PLAT_SRC_WEB, PLAT_LVL_FACTORY), 0);
    for (uint32_t t = 0; t < 3000u; t += 20u) { bsp_host_advance_ms(20); plat_boardtest_poll(bsp_host.ms); }
    plat_boardtest_status(&st);
    CHECK(st.done);
    CHECK_EQ(st.result[PLAT_BT_FRAM], PLAT_TEST_FAIL); CHECK(!strcmp(st.detail[PLAT_BT_FRAM], "address/data test"));
    CHECK_EQ(st.result[PLAT_BT_SUPPLY], PLAT_TEST_FAIL);
    CHECK_EQ(st.result[PLAT_BT_RTC], PLAT_TEST_FAIL); CHECK(!strcmp(st.detail[PLAT_BT_RTC], "not running"));
    dev.read = real_read; dev.write = real_write;
    bsp_host.supply_mv = 0; bsp_host.straps_fitted = false;
}

void test_selftest_all(void) {
    RUN(t_march); RUN(t_all_pass); RUN(t_failures); RUN(t_skips); RUN(t_boardtest);
    plat_selftest_reset_for_test();
    plat_log_reset_for_test();
    fw_reset_for_test();
}
