/* plat_clock.c - see plat_clock.h */
#include <math.h>
#include <string.h>
#include "plat_clock.h"
#include "plat_log.h"
#include "plat_port.h"
#include "param.h"
#include "bsp.h"
#include "ul_system.h"

static struct { bool pending; uint32_t t; uint8_t src, level; char how[12]; } req;
uint32_t plat_clock_unix;                        /* parameter 86 (RO) */
extern uint32_t plat_log_expected_time;          /* plat_log.c: a step to this time is logged here, not there */

int plat_clock_request(uint32_t t, uint8_t src, uint8_t level, const char *how) {
    if (t < PLAT_CLOCK_MIN_UNIX || t > PLAT_CLOCK_MAX_UNIX) return -1;
    PLAT_CRIT_ENTER();
    req.t = t; req.src = src; req.level = level;
    size_t k = 0;
    for (; how && how[k] && k + 1 < sizeof req.how; ++k) req.how[k] = how[k];
    req.how[k] = 0;
    req.pending = true;
    PLAT_CRIT_EXIT();
    return 0;
}

static uint32_t snapshot_unix(bool *set) {
    *set = RTC_Date.Year >= 2000u && RTC_Date.Month >= 1u && RTC_Date.Month <= 12u && RTC_Date.Day_In_Month >= 1u;
    return *set ? plat_unix_from_civil(RTC_Date.Year, RTC_Date.Month, RTC_Date.Day_In_Month,
                                       RTC_Time.Hours, RTC_Time.Minutes, RTC_Time.Seconds) : 0u;
}

bool plat_clock_now(uint32_t *t) {
    bool set;
    uint32_t u = snapshot_unix(&set);
    if (t) *t = u;
    return set;
}

int32_t plat_clock_utc_offset_s(void) {
    plat_val_t v;
    return param_read(PARAM_ID_TIME_UTC_OFFSET, &v) ? v.i * 60 : 0;
}

void plat_clock_scan(void) {
    bool set;
    uint32_t now = snapshot_unix(&set);
    plat_clock_unix = now;
    if (!req.pending) return;
    PLAT_CRIT_ENTER();
    uint32_t t = req.t; uint8_t src = req.src, level = req.level;
    char how[12]; memcpy(how, req.how, sizeof how);
    req.pending = false;
    PLAT_CRIT_EXIT();
    unsigned y, mo, d, h, mi, s;
    plat_civil_from_unix(t, &y, &mo, &d, &h, &mi, &s);
    uint32_t days = t / 86400u;                       /* 1970-01-01 was a Thursday (ISO 4) */
    bsp_rtc_t r = { .year = (uint16_t)y, .month = (uint8_t)mo, .day = (uint8_t)d, .hours = (uint8_t)h,
                    .minutes = (uint8_t)mi, .seconds = (uint8_t)s, .dow_iso = (uint8_t)((days + 3u) % 7u + 1u) };
    if (!bsp_rtc_write(&r)) return;
    plat_log_expected_time = t;                       /* plat_log would log the jump as "internal" */
    plat_log_event(LOG_EV_TIME_SET, src, level, 0, set ? (double)now : NAN, (double)t, how);
}
