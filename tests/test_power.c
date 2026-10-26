/* test_power.c - the 24 V supply (CIP-07.3, CIP-07.5): the 16-reading average, failing and
 * restored with hysteresis, the events, the SUPPLY alarm, parameter 305, and a board that does
 * not measure it. */
#include <string.h>
#include "tst.h"
#include "plat_power.h"
#include "plat_alarm.h"
#include "plat_config.h"
#include "param.h"
#include "bsp_host.h"

static void ms(unsigned n) { for (unsigned k = 0; k < n; ++k) { bsp_host_advance_ms(1); plat_power_sample(); } }
static void scans(unsigned n) {
    for (unsigned k = 0; k < n; ++k) { ms(10); alarm_scan(bsp_ms()); plat_power_scan(bsp_ms()); }
    param_publish();
}

static void setup(void) {
    bsp_host_reset();
    bsp_host.ms = 1000;
    plat_power_reset_for_test();
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    param_bind(NULL, NULL);
    param_set_defaults(PARAM_ALL);
    CHECK_EQ(alarm_init(NULL, 0, 0), 0);
}

static void t_not_measured(void) {
    setup();
    scans(300);
    CHECK_EQ(plat_supply_mv(), 0u);
    CHECK(!plat_power_failing());
    CHECK(!alarm_active(ALARM_ID_SUPPLY));
}

static void t_supply(void) {
    setup();
    bsp_host.supply_mv = 24000;
    ms(15);
    CHECK_EQ(plat_supply_mv(), 0u);                        /* not before 16 readings */
    ms(1);
    CHECK_EQ(plat_supply_mv(), 24000u);
    scans(1);
    plat_val_t v;
    CHECK(param_read(PARAM_ID_SUPPLY_MV, &v) && v.u == 24000u);

    /* low for longer than 2 s: the alarm; back to normal: it clears after 2 s */
    bsp_host.supply_mv = 18500;
    scans(150);
    CHECK(!alarm_active(ALARM_ID_SUPPLY));
    scans(100);
    CHECK(alarm_active(ALARM_ID_SUPPLY));
    CHECK(!plat_power_failing());                          /* low is not failing */
    bsp_host.supply_mv = 31000;
    scans(250);
    CHECK(alarm_active(ALARM_ID_SUPPLY));                  /* high */
    bsp_host.supply_mv = 24000;
    scans(250);
    CHECK(!alarm_active(ALARM_ID_SUPPLY));

    /* one noisy reading does not trip it */
    bsp_host.supply_mv = 8000; ms(1);
    bsp_host.supply_mv = 24000; ms(1);
    CHECK(!plat_power_failing());

    /* failing: on the second raw reading below 16 V, not after the 16-reading average */
    bsp_host.supply_mv = 15000;
    ms(1);
    CHECK(!plat_power_failing());
    ms(1);
    CHECK(plat_power_failing());
    CHECK(plat_supply_mv() > 16000u);                      /* the average has hardly moved yet */
    scans(1);
    /* a dip, not a loss: back above 18 V for a whole second before writes resume */
    bsp_host.supply_mv = 24000;
    ms(500);
    bsp_host.supply_mv = 17000; ms(20);                    /* not quite: the second starts again */
    bsp_host.supply_mv = 24000;
    ms(990);
    CHECK(plat_power_failing());
    ms(30);
    CHECK(!plat_power_failing());
    scans(1);
}

void test_power_all(void) { RUN(t_not_measured); RUN(t_supply); plat_power_reset_for_test(); }
