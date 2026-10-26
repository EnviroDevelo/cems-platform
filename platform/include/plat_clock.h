/* plat_clock.h - setting the instrument's clock (decision D22).
 *
 * The RTC keeps the site's local standard time (no daylight saving: a CEMS record
 * must not jump an hour twice a year). parameter 82 time_utc_offset (minutes) says how
 * far that is from UTC; it is used by the SNTP client and in email Date headers.
 *
 * Ways to set it, every one logged (LOG_EV_TIME_SET: old -> new, who, from where):
 *   touchscreen  System > Set clock (admin)
 *   web          "Set to this computer's time" or a typed time (admin)
 *   Modbus       write local seconds-since-1970 to parameter 85 clock_set (operator,
 *                REMOTE mode, as every PLC write): how a DAHS keeps the clock in step
 *   SNTP         from the site's time server (parameter 83 sntp_server, 0 = off),
 *                every sntp_interval minutes; only steps the clock when it is 2 s or
 *                more out, so the log is not filled with tiny corrections
 *
 * Threading: plat_clock_request() from any task; the scan task applies it (it owns
 * the RTC snapshot), so a set never tears a scan. */
#ifndef PLAT_CLOCK_H
#define PLAT_CLOCK_H
#include <stdint.h>
#include <stdbool.h>

#define PLAT_CLOCK_MIN_UNIX 1577836800u   /* 2020-01-01: anything earlier is refused */
#define PLAT_CLOCK_MAX_UNIX 4102444799u   /* 2099-12-31 23:59:59 */

/* Ask for the clock to be set to local_unix (local standard time as seconds since
 * 1970). how: a short word for the event log ("web", "sntp" ...). 0 = queued,
 * -1 out of range. */
int  plat_clock_request(uint32_t local_unix, uint8_t src, uint8_t level, const char *how);
void plat_clock_scan(void);                 /* scan task, pre_logic: applies a request */
bool plat_clock_now(uint32_t *local_unix);  /* any task: the clock as of the last scan; false = never set */
int32_t plat_clock_utc_offset_s(void);      /* parameter 82, in seconds */

/* Clock quality (CIP-02.6). The source of the last setting or confirmation, when it was, and
 * whether the clock counts as synchronised: set or confirmed by any outside source (an SNTP
 * answer, whether or not it stepped the clock; a setting from the web, touchscreen or Modbus)
 * within PLAT_CLOCK_SYNC_MAX_S. Not kept across a restart: after power-up the clock runs on
 * the RTC battery and is "not synchronised" until the next SNTP answer or setting.
 * Parameters 316 clock_synced, 317 clock_sync_time, 318 clock_source; the Modbus block;
 * /api/time. */
#ifndef PLAT_CLOCK_SYNC_MAX_S
#define PLAT_CLOCK_SYNC_MAX_S 86400u
#endif
enum { PLAT_CLK_SRC_NONE = 0, PLAT_CLK_SRC_SNTP, PLAT_CLK_SRC_TOUCH, PLAT_CLK_SRC_WEB, PLAT_CLK_SRC_MODBUS };
void        plat_clock_sntp_ok(void);       /* net task: a good SNTP answer (sntp.c) */
bool        plat_clock_synced(void);        /* any task */
uint32_t    plat_clock_sync_time(void);     /* local s since 1970 of the last setting or confirmation; 0 = none since start */
uint8_t     plat_clock_source(void);        /* PLAT_CLK_SRC_* */
const char *plat_clock_source_name(uint8_t s);   /* "sntp", "touchscreen", "web", "modbus", "" */
void        plat_clock_reset_for_test(void);

/* SNTP client (platform/net/sntp.c), net task. */
typedef struct {
    uint32_t last_ok_ms;      /* uptime of the last good answer, 0 = none yet */
    int32_t  last_diff_s;     /* the clock was this far behind the server then (s; + = clock slow) */
    uint8_t  stratum;
    uint32_t tries, fails, steps;
    char     error[40];       /* the last failure, in words; "" if the last try worked */
} sntp_status_t;
void sntp_service(uint32_t now_ms);         /* net task, every few hundred ms; does a query when due */
void sntp_request_now(void);                /* query at the next service (web "Sync now") */
void sntp_status(sntp_status_t *s);
void sntp_reset_for_test(void);
extern uint16_t sntp_port;                  /* 123; tests use another */

#endif
