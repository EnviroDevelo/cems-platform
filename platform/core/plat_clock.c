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
bool     plat_clock_synced_tag;                  /* parameter 316 */
uint32_t plat_clock_sync_time_tag;               /* parameter 317 */
uint8_t  plat_clock_source_tag;                  /* parameter 318 */
static uint32_t sync_ms;                         /* uptime of the last setting or confirmation (0 = none) */
static volatile bool sntp_ok;                    /* set by the net task, taken by the scan */
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

static void synced_by(uint8_t source, uint32_t t) {
    uint32_t ms = bsp_ms();
    sync_ms = ms ? ms : 1u;
    plat_clock_source_tag = source;
    plat_clock_sync_time_tag = t;
}

void plat_clock_sntp_ok(void) { sntp_ok = true; }

void plat_clock_scan(void) {
    bool set;
    uint32_t now = snapshot_unix(&set);
    plat_clock_unix = now;
    if (sntp_ok && set) { sntp_ok = false; synced_by(PLAT_CLK_SRC_SNTP, now); }
    plat_clock_synced_tag = set && sync_ms && (uint32_t)(bsp_ms() - sync_ms) / 1000u < PLAT_CLOCK_SYNC_MAX_S;
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
    static const uint8_t by_src[] = { PLAT_CLK_SRC_TOUCH, PLAT_CLK_SRC_WEB, PLAT_CLK_SRC_MODBUS, PLAT_CLK_SRC_NONE };
    uint8_t source = !strcmp(how, "sntp") ? PLAT_CLK_SRC_SNTP : by_src[src & 3u];
    if (source != PLAT_CLK_SRC_NONE) { synced_by(source, t); plat_clock_synced_tag = true; }
}

bool     plat_clock_synced(void)    { return plat_clock_synced_tag; }
uint32_t plat_clock_sync_time(void) { return plat_clock_sync_time_tag; }
uint8_t  plat_clock_source(void)    { return plat_clock_source_tag; }
const char *plat_clock_source_name(uint8_t s) {
    static const char *const n[] = { "", "sntp", "touchscreen", "web", "modbus" };
    return s < sizeof n / sizeof n[0] ? n[s] : "";
}
void plat_clock_reset_for_test(void) {
    sync_ms = 0; sntp_ok = false; req.pending = false;
    plat_clock_synced_tag = false; plat_clock_sync_time_tag = 0; plat_clock_source_tag = 0;
}
