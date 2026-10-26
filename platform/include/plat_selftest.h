/* plat_selftest.h - the start-up self-test list (Common Instrument Platform Specification
 * CIP-11.1): a named list of checks, each with one result (pass, fail, skipped), done once
 * after every start.
 *
 *   image        the running firmware against the SHA-256 in the header the update left in
 *                its flash bank (fw_running_check). Skipped if there is no header (programmed
 *                with the debugger).
 *   ram          a March C- test of a 1 KB block (the H573's SRAM also has ECC).
 *   rtc          the real-time clock is running: its seconds move on within 3 s. Skipped if
 *                the clock has never been set.
 *   settings     the settings memory (FRAM) answers and holds: not on the RAM fallback, no
 *                write or table error.
 *   calibration  a valid factory (calibration) image was loaded.
 *   log_flash    the log flash answers and is large enough. Skipped on the RAM stand-in
 *                (none fitted, e.g. the DK).
 *   net_phy      the plant-port PHY (LAN8742A) answered at start-up (reported by the
 *                Ethernet driver). Skipped if nothing reports it (the PC simulation).
 *   io_port      the I/O-port W5500 answers (reported by its driver when the I/O bus first
 *                uses it). Skipped if nothing used it: no such chip, or no I/O board enabled.
 *   display      the touchscreen answers. Skipped if the hardware has none.
 *   + up to PLAT_STEST_APP_MAX product checks (plat_app_t.selftests).
 *
 * The first six are worked out at the first poll (image over the next second); net_phy,
 * io_port, display and the product's checks PLAT_STEST_DEFER_MS after start, when those
 * devices have had their first try. When all are known: parameter 333 holds the failed bits
 * (bit i = platform check i, bit 16 + j = product check j), the event log gets one
 * "selftest" event (arg = the bits, text = "passed" or the failed names), alarm 23 SELFTEST
 * (warning) is on while any failed that has no alarm of its own (settings: PARAM_WRITE_FAIL,
 * calibration: CAL_INVALID, display: DISPLAY_FAULT), and /api/info "selftest" lists every result.
 *
 * Threading: plat_selftest_init before the tasks; plat_selftest_poll from the store task;
 * plat_selftest_report from any task; the readers from any task. */
#ifndef PLAT_SELFTEST_H
#define PLAT_SELFTEST_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef PLAT_STEST_DEFER_MS
#define PLAT_STEST_DEFER_MS 10000u
#endif
#define PLAT_STEST_APP_MAX  8u

enum { PLAT_STEST_IMAGE = 0, PLAT_STEST_RAM, PLAT_STEST_RTC, PLAT_STEST_SETTINGS, PLAT_STEST_CALIBRATION,
       PLAT_STEST_LOG_FLASH, PLAT_STEST_NET_PHY, PLAT_STEST_IO_PORT, PLAT_STEST_DISPLAY, PLAT_STEST_N };
/* failed bits that already raise an alarm of their own, so SELFTEST leaves them out */
#define PLAT_STEST_OWN_ALARM ((1u << PLAT_STEST_SETTINGS) | (1u << PLAT_STEST_CALIBRATION) | (1u << PLAT_STEST_DISPLAY))
enum { PLAT_STR_PENDING = 0, PLAT_STR_PASS, PLAT_STR_FAIL, PLAT_STR_SKIP };

/* A product's own start-up check, run once from the store task PLAT_STEST_DEFER_MS after
 * start. Keep it short (it holds up the store task): 1 = pass, 0 = fail, -1 = skipped. */
typedef struct {
    const char *name;               /* short, lower case, e.g. "pump" */
    int (*run)(void);
} plat_selftest_def_t;

int  plat_selftest_init(const plat_selftest_def_t *app, size_t n, uint32_t now_ms);   /* -1 = more than PLAT_STEST_APP_MAX */
void plat_selftest_poll(uint32_t now_ms);
void plat_selftest_report(unsigned id, bool pass);    /* a driver's own check (PLAT_STEST_NET_PHY, _IO_PORT) */

bool        plat_selftest_done(void);
uint32_t    plat_selftest_failed(void);               /* the bits above; 0 until done */
unsigned    plat_selftest_count(void);                /* PLAT_STEST_N + the product's */
bool        plat_selftest_get(unsigned i, const char **name, uint8_t *result);
const char *plat_selftest_result_name(uint8_t r);     /* "pending", "pass", "fail", "skipped" */

bool plat_ram_march(volatile uint32_t *p, unsigned words);   /* March C-, restores nothing */
void plat_selftest_reset_for_test(void);

#endif
