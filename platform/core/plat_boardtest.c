/* plat_boardtest.c - see plat_boardtest.h */
#include <string.h>
#include <stdio.h>
#include "plat_boardtest.h"
#include "plat_selftest.h"
#include "plat_service.h"
#include "plat_clock.h"
#include "plat_log.h"
#include "plat_port.h"
#include "param.h"
#include "param_store.h"
#include "iobus.h"
#include "bsp.h"

#define RTC_WAIT_MS 2500u

static const char *const names[PLAT_BT_N] = {
    "fram", "ram", "rtc", "image", "log_flash", "net_phy", "net_link", "io_port", "io_link", "display",
    "supply", "hw_rev",
};

__attribute__((weak)) int plat_net_link_state(void) { return -1; }

static volatile bool req;
static uint8_t  req_src, req_level;
static plat_bt_status_t st;          /* written by the store task; copied out under the lock */
static uint32_t rtc_t0;
static bool     rtc_set0;

const char *plat_boardtest_name(unsigned i) { return i < PLAT_BT_N ? names[i] : NULL; }

int plat_boardtest_start(uint8_t src, uint8_t level) {
    if (level < PLAT_LVL_FACTORY) return -3;
    int r = 0;
    PLAT_CRIT_ENTER();
    if (req || st.running || !plat_selftest_done()) r = -1;
    else { req = true; req_src = src; req_level = level; }
    PLAT_CRIT_EXIT();
    return r;
}

void plat_boardtest_status(plat_bt_status_t *out) {
    PLAT_CRIT_ENTER();
    *out = st;
    if (req) out->running = true;     /* asked, not yet begun */
    PLAT_CRIT_EXIT();
}

static bool rtc_unix(uint32_t *t) {
    bsp_rtc_t r;
    if (!bsp_rtc_read(&r) || r.year < 2000u) return false;
    *t = plat_unix_from_civil(r.year, r.month, r.day, r.hours, r.minutes, r.seconds);
    return true;
}

static void put(unsigned i, int r, const char *detail) {   /* r: 1 pass, 0 fail, -1 skipped */
    st.result[i] = r > 0 ? PLAT_TEST_PASS : r == 0 ? PLAT_TEST_FAIL : PLAT_TEST_NOT_RUN;
    if (detail) snprintf(st.detail[i], sizeof st.detail[i], "%s", detail);
}

static int from_selftest(unsigned id) {
    uint8_t r = PLAT_STR_SKIP;
    plat_selftest_get(id, NULL, &r);
    return r == PLAT_STR_PASS ? 1 : r == PLAT_STR_FAIL ? 0 : -1;
}

static void first_part(void) {
    char d[24];
    unsigned chips = 0;
    uint32_t bytes = param_store_nvm_size(&chips);
    int t = param_store_nvm_test();
    snprintf(d, sizeof d, "%lu KB, %u chip%s", (unsigned long)(bytes / 1024u), chips, chips == 1u ? "" : "s");
    if (bytes == 0u) put(PLAT_BT_FRAM, 0, "none (RAM stand-in)");
    else if (t != 1) put(PLAT_BT_FRAM, 0, "address/data test");
    else put(PLAT_BT_FRAM, bytes >= PLAT_FRAM_SIZE ? 1 : 0, d);

    static uint32_t block[1024];      /* 4 KB */
    put(PLAT_BT_RAM, plat_ram_march(block, 1024u) ? 1 : 0, "4 KB March C-");

    rtc_set0 = rtc_unix(&rtc_t0);
    put(PLAT_BT_IMAGE, from_selftest(PLAT_STEST_IMAGE), NULL);
    if (st.result[PLAT_BT_IMAGE] == PLAT_TEST_NOT_RUN) put(PLAT_BT_IMAGE, -1, "no header (debugger)");
    put(PLAT_BT_LOG_FLASH, from_selftest(PLAT_STEST_LOG_FLASH), NULL);
    put(PLAT_BT_NET_PHY, from_selftest(PLAT_STEST_NET_PHY), NULL);
    int l = plat_net_link_state();
    put(PLAT_BT_NET_LINK, l, l > 0 ? "up" : l == 0 ? "no link: cable?" : NULL);
    int io = from_selftest(PLAT_STEST_IO_PORT);
    put(PLAT_BT_IO_PORT, io, io < 0 ? "not used" : NULL);
    uint8_t ps = iob_port_status;
    put(PLAT_BT_IO_LINK, io < 0 ? -1 : (ps & IOBP_LINK) ? 1 : 0, io < 0 ? NULL : (ps & IOBP_LINK) ? "up" : "no link: cable?");
    put(PLAT_BT_DISPLAY, from_selftest(PLAT_STEST_DISPLAY), NULL);
    uint32_t mv;
    if (bsp_supply_mv(&mv)) {
        snprintf(d, sizeof d, "%lu.%lu V", (unsigned long)(mv / 1000u), (unsigned long)(mv % 1000u / 100u));
        put(PLAT_BT_SUPPLY, mv >= 20000u && mv <= 28800u ? 1 : 0, d);
    } else put(PLAT_BT_SUPPLY, -1, "not measured");
    uint8_t rev;
    if (bsp_hw_straps(&rev)) { snprintf(d, sizeof d, "rev %u", rev); put(PLAT_BT_HW_REV, 1, d); }
    else put(PLAT_BT_HW_REV, -1, "no straps");
}

void plat_boardtest_poll(uint32_t now_ms) {
    if (req && !st.running) {
        PLAT_CRIT_ENTER();
        memset(&st, 0, sizeof st);
        st.running = true;
        st.started_ms = now_ms;
        req = false;
        PLAT_CRIT_EXIT();
        first_part();
        return;
    }
    if (!st.running) return;
    uint32_t t;
    bool moved = rtc_set0 && rtc_unix(&t) && t != rtc_t0;
    if (!moved && now_ms - st.started_ms < RTC_WAIT_MS) return;
    put(PLAT_BT_RTC, moved ? 1 : 0, !rtc_set0 ? "clock not set" : moved ? NULL : "not running");
    int saved = plat_service_test("board", st.result, PLAT_BT_N, req_src, req_level);
    PLAT_CRIT_ENTER();
    st.saved = saved;
    st.running = false;
    st.done = true;
    PLAT_CRIT_EXIT();
}

void plat_boardtest_reset_for_test(void) {
    req = false;
    memset(&st, 0, sizeof st);
}
