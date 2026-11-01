/* plat_boardtest.h - the factory board test (Common Instrument Platform Specification CIP-23.3):
 * one button at factory login runs every hardware check the unit can make on itself and saves
 * the result as the unit's test record (plat_service.h, station "board").
 *
 * Unlike the start-up self-test (plat_selftest.h, read-only, every start) it writes: the
 * settings memory is checked through every address line and chip, each byte put back straight
 * after (about 20 bytes, a few ms with nothing else writing: the store task runs it). A power
 * cut in that window can leave one of those bytes wrong; the settings copies have CRCs and a
 * second copy, so run it at the factory or on the bench, not on a unit in service.
 *
 * The tests, in the record's order (PLAT_BT_*):
 *   fram        every address line and chip of the settings memory holds its own value; the
 *               memory found is at least PLAT_FRAM_SIZE and not the RAM stand-in
 *   ram         March C- over 4 KB
 *   rtc         the clock is set and its seconds move on within 2.5 s
 *   image       the running firmware matches its header (start-up self-test result)
 *   log_flash   the log flash answered and is large enough (start-up result; skipped if none)
 *   net_phy     the plant-port PHY answered at start-up
 *   net_link    the plant port has a link (cable in) now
 *   io_port     the I/O-port W5500 answers (skipped if the I/O bus never used it)
 *   io_link     the I/O port has a link now (skipped as io_port)
 *   display     the touchscreen works (skipped if none fitted)
 *   supply      the 24 V supply reads 20.0-28.8 V (skipped if not measured)
 *   hw_rev      the board revision straps read (the value is in the detail)
 *
 * Threading: plat_boardtest_start from any task (it only asks); plat_boardtest_poll from the
 * store task; the readers from any task (copies). */
#ifndef PLAT_BOARDTEST_H
#define PLAT_BOARDTEST_H
#include <stdint.h>
#include <stdbool.h>

enum { PLAT_BT_FRAM = 0, PLAT_BT_RAM, PLAT_BT_RTC, PLAT_BT_IMAGE, PLAT_BT_LOG_FLASH, PLAT_BT_NET_PHY,
       PLAT_BT_NET_LINK, PLAT_BT_IO_PORT, PLAT_BT_IO_LINK, PLAT_BT_DISPLAY, PLAT_BT_SUPPLY, PLAT_BT_HW_REV,
       PLAT_BT_N };

typedef struct {
    bool     running, done;            /* done: results below are from the last run */
    uint32_t started_ms;
    uint8_t  result[PLAT_BT_N];        /* PLAT_TEST_* (plat_service.h): NOT_RUN = skipped */
    char     detail[PLAT_BT_N][24];    /* a measured value or why it failed, may be empty */
    int      saved;                    /* plat_service_test's answer when it was recorded (0 = ok) */
} plat_bt_status_t;

/* 0 = started, -1 = already running or the start-up self-test is not finished, -3 = below factory */
int  plat_boardtest_start(uint8_t src, uint8_t level);
void plat_boardtest_poll(uint32_t now_ms);
void plat_boardtest_status(plat_bt_status_t *out);
const char *plat_boardtest_name(unsigned i);     /* NULL past the end */

/* hooks, weak defaults: -1 = not known on this hardware */
int  plat_net_link_state(void);                  /* plant port: 1 up, 0 down */
void plat_boardtest_reset_for_test(void);

#endif
