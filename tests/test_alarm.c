/* test_alarm.c - the alarm manager: delays, latch, acknowledge, suppression,
 * range deadband, texts, ordering, the parameter interface and platform alarms. */
#include <math.h>
#include <string.h>
#include "tst.h"
#include "plat_alarm.h"
#include "param.h"
#include "bsp.h"

static bool     en1 = true;
static uint16_t settle_s = 0;
static uint16_t desc[24];                     /* UTF-16 text parameter */

static const param_def_t pdefs[] = {
    PARAM_BIT_D(1100, "al_en", en1, true, PLAT_LVL_ADMIN, PF_PERSIST),
    PARAM_UINT(1101, "al_settle", settle_s, CT_U16, 0, 600, 5, PLAT_LVL_ADMIN, PF_PERSIST, "s"),
    PARAM_TEXT16(1102, "al_desc", desc, NULL, PLAT_LVL_ADMIN, PF_PERSIST),
};

enum { A_DI = 100, A_FB = 130, A_RANGE = 160, A_FLOW = 170, A_STOP = 171, A_DESC = 101 };
static const alarm_def_t adefs[] = {
    { .id = A_DI, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .name = "DI_ALARM_01", .text = "Input 1" },
    { .id = A_DESC, .severity = AL_SEV_WARNING, .flags = AL_NOTIFY, .name = "DI_ALARM_02", .text = "Input 2", .text_param = 1102 },
    { .id = A_FB, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .name = "VALVE_FB_01", .text = "Valve 1 feedback",
      .on_delay_ms = 5000, .delay_param = 1101 },
    { .id = A_RANGE, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .name = "AI1_RANGE", .text = "AI1 out of range",
      .on_delay_ms = 10000, .off_delay_ms = 10000, .enable_param = 1100 },
    { .id = A_FLOW, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY | AL_LATCH, .name = "FLOW_VALIDATION", .text = "No flow" },
    { .id = A_STOP, .severity = AL_SEV_STATUS, .flags = AL_NOTIFY | AL_NO_ACK, .name = "DIGITAL_STOP", .text = "Digital stop" },
};

static uint32_t now = 1000;
static int tr_raise, tr_clear, tr_ack;
static uint16_t tr_last;
static void hook(const alarm_def_t *d, uint8_t what) {
    if (d->id < ALARM_ID_APP_FIRST) return;          /* platform alarms (no calibration here) */
    tr_last = d->id;
    if (what == ALARM_TR_RAISE) tr_raise++; else if (what == ALARM_TR_CLEAR) tr_clear++; else tr_ack++;
}
static void scan(uint32_t ms) {                 /* run scans for ms at 10 ms */
    for (uint32_t t = 0; t <= ms; t += 10) { now += 10; alarm_scan(now); }
    param_publish();
}
static alarm_view_t get(uint16_t id) { alarm_view_t v; memset(&v, 0, sizeof v); CHECK(alarm_get(id, &v)); return v; }
static param_err_t ack_param(uint16_t id, uint8_t src, uint8_t lvl) {
    plat_cmd_t c = { .id = PARAM_ID_ALARM_ACK, .src = src, .kind = PLAT_VAL_U32, .level = lvl, .v.u = id };
    param_err_t e = param_apply(&c);
    param_publish();
    return e;
}
static void apply(uint16_t id, uint32_t v) {
    plat_cmd_t c = { .id = id, .src = PLAT_SRC_INTERNAL, .kind = PLAT_VAL_U32, .v.u = v };
    CHECK_EQ(param_apply(&c), PERR_OK);
    param_publish();
}
static size_t app_list(alarm_view_t *v, size_t max) {    /* the list without platform alarms */
    alarm_view_t all[PLAT_ALARM_MAX];
    size_t n = alarm_list(all, 0, PLAT_ALARM_MAX), k = 0;
    for (size_t i = 0; i < n && k < max; ++i) if (all[i].id >= ALARM_ID_APP_FIRST) v[k++] = all[i];
    return k;
}

static void setup(uint8_t reset_cause) {
    CHECK_EQ(param_init(pdefs, sizeof pdefs / sizeof pdefs[0]), PERR_OK);
    param_bind(NULL, NULL);
    param_set_defaults(PARAM_ALL);
    param_publish();
    CHECK_EQ(alarm_init(adefs, sizeof adefs / sizeof adefs[0], reset_cause), 0);
    alarm_on_change = hook;
    tr_raise = tr_clear = tr_ack = 0;
}

static void t_table(void) {
    CHECK_EQ(param_init(pdefs, sizeof pdefs / sizeof pdefs[0]), PERR_OK);
    alarm_def_t bad[2] = { adefs[0], adefs[0] };
    CHECK_EQ(alarm_init(bad, 2, 0), A_DI);                         /* duplicate */
    bad[0].id = 50;
    CHECK_EQ(alarm_init(bad, 1, 0), 50);                           /* application ID in the platform range */
    bad[0].id = 300;
    CHECK_EQ(alarm_init(bad, 1, 0), 300);                          /* beyond 255 */
    bad[0] = adefs[0]; bad[0].enable_param = 4242;
    CHECK_EQ(alarm_init(bad, 1, 0), A_DI);                         /* no such parameter */
    bad[0] = adefs[0]; bad[0].text_param = 1100;
    CHECK_EQ(alarm_init(bad, 1, 0), A_DI);                         /* text parameter is not text */
    CHECK_EQ(alarm_init(adefs, sizeof adefs / sizeof adefs[0], 0), 0);
    CHECK(alarm_count() >= 6 + 8);                                 /* the platform's own as well */
    CHECK(alarm_def_at(0)->id == ALARM_ID_CAL_INVALID);
}

static void t_raise_ack_clear(void) {
    setup(BSP_RESET_POWER_ON);
    alarm_set(A_DI, true);
    scan(20);
    alarm_view_t v = get(A_DI);
    CHECK(v.active && v.unacked);
    CHECK_EQ(tr_raise, 1); CHECK_EQ(tr_last, A_DI);
    plat_val_t pv;
    uint16_t bits[16];
    CHECK(param_read(PARAM_ID_ALARM_ACTIVE, &pv) && pv.u >= 1);
    CHECK_EQ(param_read_blob(PARAM_ID_ALARM_ACTIVE_BITS, 0, bits, sizeof bits), PERR_OK);
    CHECK(bits[A_DI / 16] & (1u << (A_DI % 16)));
    CHECK_EQ(param_read_blob(PARAM_ID_ALARM_UNACK_BITS, 0, bits, sizeof bits), PERR_OK);
    CHECK(bits[A_DI / 16] & (1u << (A_DI % 16)));
    /* acknowledge through the parameter, from the web, not logged in, in LOCAL mode (any mode) */
    CHECK_EQ(ack_param(A_DI, PLAT_SRC_WEB, PLAT_LVL_VIEW), PERR_OK);
    v = get(A_DI);
    CHECK(v.active && !v.unacked);                                 /* acknowledge does not clear */
    CHECK_EQ(tr_ack, 1);
    CHECK(param_read(PARAM_ID_ALARM_ACK, &pv) && pv.u == 0);       /* momentary */
    CHECK_EQ(param_read_blob(PARAM_ID_ALARM_UNACK_BITS, 0, bits, sizeof bits), PERR_OK);
    CHECK(!(bits[A_DI / 16] & (1u << (A_DI % 16))));
    CHECK_EQ(ack_param(A_DI, PLAT_SRC_LOCAL, 0), PERR_OK);          /* again: nothing to do */
    CHECK_EQ(tr_ack, 1);
    alarm_set(A_DI, false);
    scan(20);
    v = get(A_DI);
    CHECK(!v.active && !v.unacked);
    CHECK_EQ(tr_clear, 1);
    /* a short alarm nobody acknowledged stays in the list until acknowledged */
    alarm_set(A_DI, true); scan(20); alarm_set(A_DI, false); scan(20);
    v = get(A_DI);
    CHECK(!v.active && v.unacked);
    alarm_view_t l[8];
    CHECK_EQ(app_list(l, 8), 1);
    CHECK_EQ(ack_param(A_DI, PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(app_list(l, 8), 0);
    CHECK_EQ(alarm_ack(999, PLAT_SRC_LOCAL, 0), -1);
    CHECK(alarm_ack(4242, PLAT_SRC_LOCAL, 0) == -1);
    alarm_set(4242, true);                                         /* unknown IDs are ignored */
}

static void t_delays(void) {
    setup(BSP_RESET_POWER_ON);
    /* valve feedback: the settling-time parameter, 5 s by default */
    alarm_set(A_FB, true); scan(4000);
    CHECK(!get(A_FB).active);
    alarm_set(A_FB, false); scan(50);                              /* a blip restarts the timer */
    alarm_set(A_FB, true); scan(4000);
    CHECK(!get(A_FB).active);
    scan(1100);
    CHECK(get(A_FB).active);
    alarm_set(A_FB, false); scan(20);
    CHECK(!get(A_FB).active);                                      /* no off-delay */
    alarm_ack(A_FB, PLAT_SRC_LOCAL, 0);
    /* the settling-time parameter replaces it: 1 s */
    apply(1101, 1);
    alarm_set(A_FB, true); scan(900);
    CHECK(!get(A_FB).active);
    scan(200);
    CHECK(get(A_FB).active);
    alarm_set(A_FB, false); scan(20); alarm_ack(A_FB, PLAT_SRC_LOCAL, 0);
    /* range: 10 s on, 10 s off */
    alarm_set(A_RANGE, true); scan(9900);
    CHECK(!get(A_RANGE).active);
    scan(200);
    CHECK(get(A_RANGE).active);
    alarm_set(A_RANGE, false); scan(9900);
    CHECK(get(A_RANGE).active);
    alarm_set(A_RANGE, true); scan(5000);                          /* came back: the off timer restarts */
    alarm_set(A_RANGE, false); scan(9900);
    CHECK(get(A_RANGE).active);
    scan(200);
    CHECK(!get(A_RANGE).active);
}

static void t_latch_and_status(void) {
    setup(BSP_RESET_POWER_ON);
    /* latching: stays active after the condition goes, until acknowledged */
    alarm_set(A_FLOW, true); scan(20);
    alarm_set(A_FLOW, false); scan(1000);
    alarm_view_t v = get(A_FLOW);
    CHECK(v.active && v.unacked && !v.cond);
    CHECK_EQ(tr_clear, 0);
    alarm_ack(A_FLOW, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR);
    v = get(A_FLOW);
    CHECK(!v.active && !v.unacked);
    CHECK_EQ(tr_clear, 1);
    /* acknowledged while the condition is still there: clears when it goes */
    alarm_set(A_FLOW, true); scan(20);
    alarm_ack(A_FLOW, PLAT_SRC_LOCAL, 0);
    CHECK(get(A_FLOW).active);
    alarm_set(A_FLOW, false); scan(20);
    CHECK(!get(A_FLOW).active);
    /* status alarm: no acknowledge, leaves the list when it clears */
    alarm_set(A_STOP, true); scan(20);
    v = get(A_STOP);
    CHECK(v.active && !v.unacked);
    alarm_set(A_STOP, false); scan(20);
    v = get(A_STOP);
    CHECK(!v.active && !v.unacked);
}

static void t_suppress(void) {
    setup(BSP_RESET_POWER_ON);
    alarm_set(A_RANGE, true); scan(10100);
    CHECK(get(A_RANGE).active);
    int c0 = tr_clear;
    apply(1100, 0);                                                /* alarm switched off */
    scan(20);
    alarm_view_t v = get(A_RANGE);
    CHECK(!v.active && !v.unacked && !v.enabled);
    CHECK_EQ(tr_clear, c0 + 1);
    scan(20000);
    CHECK(!get(A_RANGE).active);                                   /* condition still there: stays off */
    apply(1100, 1);
    scan(10100);
    CHECK(get(A_RANGE).active);                                    /* back on: delays apply again */
}

static void t_range(void) {
    setup(BSP_RESET_POWER_ON);
    apply(1100, 1);
    /* lo 10, hi 90, deadband 2: raised above 90, stays until below 88 */
    alarm_set_range(A_RANGE, 50, 10, 90, 2); CHECK(!get(A_RANGE).cond);
    alarm_set_range(A_RANGE, 90.5f, 10, 90, 2); CHECK(get(A_RANGE).cond);
    alarm_set_range(A_RANGE, 89, 10, 90, 2); CHECK(get(A_RANGE).cond);          /* within the deadband */
    alarm_set_range(A_RANGE, NAN, 10, 90, 2); CHECK(get(A_RANGE).cond);         /* NaN: unchanged */
    alarm_set_range(A_RANGE, 87.9f, 10, 90, 2); CHECK(!get(A_RANGE).cond);
    alarm_set_range(A_RANGE, 9, 10, 90, 2); CHECK(get(A_RANGE).cond);           /* low side */
    alarm_set_range(A_RANGE, 11, 10, 90, 2); CHECK(get(A_RANGE).cond);
    alarm_set_range(A_RANGE, 95, 10, 90, 2); CHECK(get(A_RANGE).cond);          /* jumped across */
    alarm_set_range(A_RANGE, 11, 10, 90, 2); CHECK(!get(A_RANGE).cond);         /* now judged on the high side */
    /* a reading sitting on a limit no longer chatters (K5) */
    int r0 = tr_raise;
    for (int k = 0; k < 200; ++k) { alarm_set_range(A_RANGE, k & 1 ? 90.1f : 89.9f, 10, 90, 2); scan(100); }
    CHECK_EQ(tr_raise, r0 + 1);
}

static void t_text_order(void) {
    setup(BSP_RESET_POWER_ON);
    /* editable description: UTF-16 "Pump Ø" */
    static const uint16_t w[] = { 'P', 'u', 'm', 'p', ' ', 0x00D8, 0 };
    memset(desc, 0, sizeof desc); memcpy(desc, w, sizeof w);
    param_publish();
    CHECK(!strcmp(get(A_DESC).text, "Pump \xC3\x98"));
    CHECK(!strcmp(get(A_DI).text, "Input 1"));
    memset(desc, 0, sizeof desc); param_publish();
    CHECK(!strcmp(get(A_DESC).text, "Input 2"));                   /* empty: the default text */
    /* order: unacked active (worst first, newest first), acked active, cleared unacked */
    alarm_set(A_DESC, true); scan(20);                             /* warning */
    alarm_set(A_DI, true); scan(1000);                             /* alarm */
    alarm_set(A_STOP, true); scan(20);                             /* status, no ack: counts as acked */
    alarm_set(A_FLOW, true); scan(20); alarm_ack(A_FLOW, 0, 0);    /* fault, acked */
    alarm_set(4242, false);
    alarm_set(A_FB, true); apply(1101, 0); scan(5100); alarm_set(A_FB, false); scan(20);   /* cleared, unacked */
    alarm_view_t l[8];
    size_t n = app_list(l, 8);
    CHECK_EQ(n, 5);
    CHECK_EQ(l[0].id, A_DI); CHECK_EQ(l[1].id, A_DESC);
    CHECK_EQ(l[2].id, A_FLOW); CHECK_EQ(l[3].id, A_STOP);
    CHECK_EQ(l[4].id, A_FB);
    {   /* paging gives the same order */
        alarm_view_t all[PLAT_ALARM_MAX], pg[2];
        size_t na = alarm_list(all, 0, PLAT_ALARM_MAX);
        CHECK(na >= 5);
        CHECK_EQ(alarm_list(pg, 2, 2), 2);
        CHECK_EQ(pg[0].id, all[2].id); CHECK_EQ(pg[1].id, all[3].id);
        CHECK_EQ(alarm_list(pg, na, 2), 0);
    }
    alarm_summary_t s; alarm_summary(&s);
    CHECK(s.worst == AL_SEV_FAULT || s.worst == AL_SEV_ALARM);
    CHECK(s.unacked >= 3);
    /* acknowledge all */
    CHECK_EQ(ack_param(ALARM_ACK_ALL, PLAT_SRC_LOCAL, 0), PERR_OK);
    alarm_summary(&s);
    CHECK_EQ(s.unacked, 0);
    n = app_list(l, 8);
    CHECK_EQ(n, 4);                                                /* the cleared one has gone */
    CHECK(!strcmp(alarm_sev_name(AL_SEV_FAULT), "fault"));
}

extern uint32_t plat_net_status_word;
static void t_platform(void) {
    setup(BSP_RESET_IWDG);
    scan(300);
    alarm_view_t v = get(ALARM_ID_UNEXPECTED_RESET);
    CHECK(v.active && v.unacked);                                  /* latched after a watchdog reset */
    alarm_ack(ALARM_ID_UNEXPECTED_RESET, PLAT_SRC_LOCAL, 0);
    scan(300);
    CHECK(!get(ALARM_ID_UNEXPECTED_RESET).active);
    plat_net_status_word |= NETST_CONFLICT;
    scan(300);
    CHECK(get(ALARM_ID_IP_CONFLICT).active);
    plat_net_status_word &= ~NETST_CONFLICT;
    scan(300);
    v = get(ALARM_ID_IP_CONFLICT);
    CHECK(!v.active && v.unacked);
    alarm_ack(ALARM_ACK_ALL, PLAT_SRC_LOCAL, 0);
    setup(BSP_RESET_POWER_ON);
    scan(300);
    CHECK(!get(ALARM_ID_UNEXPECTED_RESET).active);
    alarm_on_change = NULL;
}

void test_alarm_all(void) {
    RUN(t_table);
    RUN(t_raise_ack_clear);
    RUN(t_delays);
    RUN(t_latch_and_status);
    RUN(t_suppress);
    RUN(t_range);
    RUN(t_text_order);
    RUN(t_platform);
}
