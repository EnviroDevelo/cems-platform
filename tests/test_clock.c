/* test_clock.c - setting the clock: range, the RTC write (calendar and weekday), the
 * Modbus/parameter path, and the log not reporting the step twice. */
#include <string.h>
#include "tst.h"
#include "plat_clock.h"
#include "param.h"
#include "bsp_host.h"
#include "ul_system.h"
#include "plat_log.h"

extern uint32_t plat_log_expected_time;

static void rtc_to_globals(void) {          /* what the scan engine does at the start of a scan */
    RTC_Date.Year = bsp_host.rtc.year; RTC_Date.Month = bsp_host.rtc.month; RTC_Date.Day_In_Month = bsp_host.rtc.day;
    RTC_Time.Hours = bsp_host.rtc.hours; RTC_Time.Minutes = bsp_host.rtc.minutes; RTC_Time.Seconds = bsp_host.rtc.seconds;
}

static void t_request(void) {
    bsp_host_reset();
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    param_bind(NULL, NULL);
    param_set_defaults(PARAM_ALL);
    rtc_to_globals();
    uint32_t t0;
    CHECK(plat_clock_now(&t0));
    CHECK_EQ(t0, plat_unix_from_civil(2026, 10, 19, 12, 0, 0));
    CHECK_EQ(plat_clock_request(1000, PLAT_SRC_WEB, 2, "web"), -1);          /* 1970: refused */
    CHECK_EQ(plat_clock_request(4200000000u, PLAT_SRC_WEB, 2, "web"), -1);    /* after 2099 */
    uint32_t t = plat_unix_from_civil(2028, 2, 29, 23, 59, 58);               /* a leap day, a Tuesday */
    CHECK_EQ(plat_clock_request(t, PLAT_SRC_WEB, 2, "web"), 0);
    CHECK_EQ(bsp_host.rtc_writes, 0);                                         /* applied by the scan, not the caller */
    plat_clock_scan();
    CHECK_EQ(bsp_host.rtc_writes, 1);
    CHECK_EQ(bsp_host.rtc.year, 2028); CHECK_EQ(bsp_host.rtc.month, 2); CHECK_EQ(bsp_host.rtc.day, 29);
    CHECK_EQ(bsp_host.rtc.hours, 23); CHECK_EQ(bsp_host.rtc.minutes, 59); CHECK_EQ(bsp_host.rtc.seconds, 58);
    CHECK_EQ(bsp_host.rtc.dow_iso, 2);
    CHECK_EQ(plat_log_expected_time, t);                                      /* the logger won't report it again */
    plat_clock_scan();
    CHECK_EQ(bsp_host.rtc_writes, 1);                                         /* once only */
    /* Sunday check: 2026-10-18 */
    plat_clock_request(plat_unix_from_civil(2026, 10, 18, 8, 0, 0), PLAT_SRC_LOCAL, 2, "touchscreen");
    plat_clock_scan();
    CHECK_EQ(bsp_host.rtc.dow_iso, 7);
    /* through the parameter (how a PLC sets it): momentary, range-checked */
    plat_cmd_t c = { .id = PARAM_ID_CLOCK_SET, .src = PLAT_SRC_MODBUS, .kind = PLAT_VAL_U32, .level = PLAT_LVL_OPERATOR,
                     .v.u = plat_unix_from_civil(2027, 1, 1, 0, 0, 0) };
    plat_cmd_t m = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .kind = PLAT_VAL_U32, .level = PLAT_LVL_OPERATOR, .v.u = PARAM_MODE_REMOTE };
    CHECK_EQ(param_apply(&m), PERR_OK);
    CHECK_EQ(param_apply(&c), PERR_OK);
    plat_clock_scan();
    CHECK_EQ(bsp_host.rtc.year, 2027); CHECK_EQ(bsp_host.rtc.month, 1);
    param_publish();
    plat_val_t v;
    CHECK(param_read(PARAM_ID_CLOCK_SET, &v)); CHECK_EQ(v.u, 0);              /* momentary */
    c.v.u = 5;
    CHECK(param_apply(&c) != PERR_OK);                                        /* too early: refused */
    rtc_to_globals();
    plat_clock_scan(); param_publish();
    CHECK(param_read(PARAM_ID_CLOCK_UNIX, &v)); CHECK_EQ(v.u, plat_unix_from_civil(2027, 1, 1, 0, 0, 0));
    /* UTC offset parameter in seconds */
    plat_cmd_t o = { .id = PARAM_ID_TIME_UTC_OFFSET, .src = PLAT_SRC_INTERNAL, .kind = PLAT_VAL_I32, .v.i = 600 };
    CHECK_EQ(param_apply(&o), PERR_OK); param_publish();
    CHECK_EQ(plat_clock_utc_offset_s(), 36000);
    plat_log_expected_time = 0;
    m.v.u = PARAM_MODE_LOCAL; param_apply(&m);
}

static void t_quality(void) {                /* CIP-02.6 */
    bsp_host_reset();
    bsp_host.ms = 1000;
    plat_clock_reset_for_test();
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    param_bind(NULL, NULL);
    rtc_to_globals();
    plat_clock_scan();
    CHECK(!plat_clock_synced());                                              /* the RTC alone: not synchronised */
    CHECK_EQ(plat_clock_source(), PLAT_CLK_SRC_NONE);
    CHECK_EQ(plat_clock_sync_time(), 0u);
    uint32_t t = plat_unix_from_civil(2026, 10, 26, 9, 0, 0);
    CHECK_EQ(plat_clock_request(t, PLAT_SRC_WEB, 2, "web"), 0);
    plat_clock_scan();
    CHECK(plat_clock_synced());
    CHECK_EQ(plat_clock_source(), PLAT_CLK_SRC_WEB);
    CHECK_EQ(plat_clock_sync_time(), t);
    rtc_to_globals();
    bsp_host_advance_ms(60000);
    plat_clock_sntp_ok();                                                     /* SNTP agrees: no step, but confirmed */
    plat_clock_scan();
    CHECK_EQ(plat_clock_source(), PLAT_CLK_SRC_SNTP);
    CHECK(!strcmp(plat_clock_source_name(plat_clock_source()), "sntp"));
    param_publish();
    plat_val_t v;
    CHECK(param_read(PARAM_ID_CLOCK_SYNCED, &v) && v.b);
    CHECK(param_read(PARAM_ID_CLOCK_SOURCE, &v) && v.u == PLAT_CLK_SRC_SNTP);
    bsp_host_advance_ms(PLAT_CLOCK_SYNC_MAX_S * 1000u + 1000u);
    plat_clock_scan();
    CHECK(!plat_clock_synced());                                              /* nothing heard for a day */
    CHECK_EQ(plat_clock_source(), PLAT_CLK_SRC_SNTP);                         /* still says where it came from */
    plat_clock_request(t, PLAT_SRC_INTERNAL, 0, "sntp");                      /* an SNTP step */
    plat_clock_scan();
    CHECK(plat_clock_synced());
    plat_clock_reset_for_test();
}

void test_clock_all(void) {
    RUN(t_request);
    RUN(t_quality);
}
