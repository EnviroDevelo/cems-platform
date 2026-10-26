/* plat_selftest.c - see plat_selftest.h */
#include <string.h>
#include <math.h>
#include "plat_selftest.h"
#include "plat_tasks.h"
#include "param_store.h"
#include "plat_log.h"
#include "fw_update.h"
#include "bsp.h"

#define IMAGE_STEP 16384u           /* bytes hashed per poll (the store task polls every 20 ms) */
#define RTC_WAIT_MS 3000u

static const char *const names[PLAT_STEST_N] = {
    "image", "ram", "rtc", "settings", "calibration", "log_flash", "net_phy", "io_port", "display"
};

static const plat_selftest_def_t *app_t;
static size_t   n_app;
static volatile uint8_t res[PLAT_STEST_N + PLAT_STEST_APP_MAX];
static uint32_t t0_ms;
static bool     rtc_set0, first_done, deferred_done;
static volatile bool done;
static uint32_t rtc_t0;
static volatile uint32_t fail_bits;
uint32_t plat_selftest_tag;         /* parameter 333 */

/* The UI task provides the real ones (plat_ui_task.c); builds without it have no display. */
__attribute__((weak)) bool plat_ui_fitted(void) { return false; }
__attribute__((weak)) bool plat_ui_healthy(void) { return true; }

static bool rtc_unix(uint32_t *t) {
    bsp_rtc_t r;
    if (!bsp_rtc_read(&r) || r.year < 2000u) return false;
    *t = plat_unix_from_civil(r.year, r.month, r.day, r.hours, r.minutes, r.seconds);
    return true;
}

bool plat_ram_march(volatile uint32_t *p, unsigned words) {
    unsigned i;
    for (i = 0; i < words; ++i) p[i] = 0u;                                          /* up w0 */
    for (i = 0; i < words; ++i) { if (p[i] != 0u) return false; p[i] = ~0u; }       /* up r0 w1 */
    for (i = 0; i < words; ++i) { if (p[i] != ~0u) return false; p[i] = 0u; }       /* up r1 w0 */
    for (i = words; i-- > 0;) { if (p[i] != 0u) return false; p[i] = ~0u; }        /* down r0 w1 */
    for (i = words; i-- > 0;) { if (p[i] != ~0u) return false; p[i] = 0u; }        /* down r1 w0 */
    for (i = 0; i < words; ++i) if (p[i] != 0u) return false;                       /* r0 */
    for (i = 0; i < words; ++i) p[i] = 0xA5A5A5A5u ^ (uint32_t)i * 0x01010101u;     /* address faults */
    for (i = 0; i < words; ++i) if (p[i] != (0xA5A5A5A5u ^ (uint32_t)i * 0x01010101u)) return false;
    return true;
}

int plat_selftest_init(const plat_selftest_def_t *app, size_t n, uint32_t now_ms) {
    memset((void *)res, 0, sizeof res);
    done = first_done = deferred_done = false;
    fail_bits = 0;
    plat_selftest_tag = 0;
    t0_ms = now_ms;
    if (n > PLAT_STEST_APP_MAX) { app_t = NULL; n_app = 0; return -1; }
    app_t = app;
    n_app = app ? n : 0;
    static uint32_t block[256];
    res[PLAT_STEST_RAM] = plat_ram_march(block, 256u) ? PLAT_STR_PASS : PLAT_STR_FAIL;
    rtc_set0 = rtc_unix(&rtc_t0);
    return 0;
}

void plat_selftest_report(unsigned id, bool pass) {
    if (id < PLAT_STEST_N && !done) res[id] = pass ? PLAT_STR_PASS : PLAT_STR_FAIL;
}

static uint8_t r_of(bool ok) { return ok ? PLAT_STR_PASS : PLAT_STR_FAIL; }

static void first_checks(void) {
    uint32_t ps = param_store_status();
    res[PLAT_STEST_SETTINGS] = r_of(!(ps & (PSTAT_NVM_VOLATILE | PSTAT_WRITE_ERROR | PSTAT_TABLE_ERROR)));
    res[PLAT_STEST_CALIBRATION] = r_of(param_store_calibration_valid());
    uint32_t ls = plat_log_status();
    log_ring_info_t inf;
    bool on = plat_log_info(LOG_RING_DATA, &inf);
    if ((ls & (LSTAT_NOR_FAULT | LSTAT_TOO_SMALL)) || !on) res[PLAT_STEST_LOG_FLASH] = PLAT_STR_FAIL;
    else res[PLAT_STEST_LOG_FLASH] = (ls & LSTAT_VOLATILE) ? PLAT_STR_SKIP : PLAT_STR_PASS;
}

static void deferred_checks(void) {
    if (res[PLAT_STEST_NET_PHY] == PLAT_STR_PENDING) res[PLAT_STEST_NET_PHY] = PLAT_STR_SKIP;
    if (res[PLAT_STEST_IO_PORT] == PLAT_STR_PENDING) res[PLAT_STEST_IO_PORT] = PLAT_STR_SKIP;
    res[PLAT_STEST_DISPLAY] = plat_ui_fitted() ? r_of(plat_ui_healthy()) : PLAT_STR_SKIP;
    for (size_t j = 0; j < n_app; ++j) {
        int r = app_t[j].run ? app_t[j].run() : -1;
        res[PLAT_STEST_N + j] = r > 0 ? PLAT_STR_PASS : r == 0 ? PLAT_STR_FAIL : PLAT_STR_SKIP;
    }
}

static void finish(void) {
    uint32_t bits = 0;
    char txt[23] = { 0 };
    size_t k = 0;
    for (unsigned i = 0; i < plat_selftest_count(); ++i) {
        if (res[i] != PLAT_STR_FAIL) continue;
        bits |= i < PLAT_STEST_N ? 1u << i : 1u << (16u + i - PLAT_STEST_N);
        const char *nm = NULL;
        plat_selftest_get(i, &nm, NULL);
        for (const char *p = k ? " " : ""; *p && k < sizeof txt - 1u; ++p) txt[k++] = *p;
        for (const char *p = nm; p && *p && k < sizeof txt - 1u; ++p) txt[k++] = *p;
    }
    if (!bits) strcpy(txt, "passed");
    fail_bits = bits;
    plat_selftest_tag = bits;
    done = true;
    plat_log_event(LOG_EV_SELFTEST, PLAT_SRC_INTERNAL, 0, bits, NAN, NAN, txt);
}

void plat_selftest_poll(uint32_t now_ms) {
    if (done) return;
    if (!first_done) { first_checks(); first_done = true; }
    if (res[PLAT_STEST_IMAGE] == PLAT_STR_PENDING) {
        int r = fw_running_check(IMAGE_STEP);
        if (r) res[PLAT_STEST_IMAGE] = r > 0 ? PLAT_STR_PASS : r == -1 ? PLAT_STR_FAIL : PLAT_STR_SKIP;
    }
    uint32_t el = now_ms - t0_ms;
    if (res[PLAT_STEST_RTC] == PLAT_STR_PENDING && el >= RTC_WAIT_MS) {
        uint32_t t;
        if (!rtc_set0) res[PLAT_STEST_RTC] = PLAT_STR_SKIP;
        else res[PLAT_STEST_RTC] = r_of(rtc_unix(&t) && t != rtc_t0);
    }
    if (!deferred_done && el >= PLAT_STEST_DEFER_MS) { deferred_checks(); deferred_done = true; }
    for (unsigned i = 0; i < plat_selftest_count(); ++i) if (res[i] == PLAT_STR_PENDING) return;
    finish();
}

bool     plat_selftest_done(void)   { return done; }
uint32_t plat_selftest_failed(void) { return fail_bits; }
unsigned plat_selftest_count(void)  { return PLAT_STEST_N + (unsigned)n_app; }

bool plat_selftest_get(unsigned i, const char **name, uint8_t *result) {
    if (i >= plat_selftest_count()) return false;
    if (name) *name = i < PLAT_STEST_N ? names[i] : (app_t[i - PLAT_STEST_N].name ? app_t[i - PLAT_STEST_N].name : "product");
    if (result) *result = res[i];
    return true;
}

const char *plat_selftest_result_name(uint8_t r) {
    switch (r) { case PLAT_STR_PASS: return "pass"; case PLAT_STR_FAIL: return "fail"; case PLAT_STR_SKIP: return "skipped"; default: return "pending"; }
}

void plat_selftest_reset_for_test(void) {
    memset((void *)res, 0, sizeof res);
    app_t = NULL; n_app = 0;
    done = first_done = deferred_done = false;
    fail_bits = 0; plat_selftest_tag = 0;
}
