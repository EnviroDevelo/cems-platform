/* test_param.c - shared parameter table: definitions, checks, apply, arbitration.
 * Requirements proved here: CIP-05.2, 14.1, 14.6, 15.4, 07.4. */
#include <math.h>
#include <string.h>
#include "tst.h"
#include "param.h"

static int16_t  t_i16;
static uint8_t  t_u8;
static float    t_f;
static bool     t_bit, t_any;
static uint32_t t_cal, t_status;
static int32_t  t_lock;
static int interlock_calls;
static int no_odd(uint16_t id, plat_val_t v) { (void)id; interlock_calls++; return (v.i & 1) ? -1 : 0; }

static const param_def_t defs[] = {
    PARAM_INT (1001, "i16", t_i16, CT_I16, -100, 100, 5, PLAT_LVL_OPERATOR, PF_PERSIST, "mA"),
    PARAM_UINT(1002, "u8",  t_u8,  CT_U8, 0, 200, 7, PLAT_LVL_OPERATOR, 0, NULL),
    PARAM_REAL(1003, "f",   t_f, 0.0f, 10.0f, 1.5f, "bar", PLAT_LVL_ADMIN, PF_PERSIST),
    PARAM_BIT (1004, "bit", t_bit, PLAT_LVL_OPERATOR, 0),
    PARAM_UINT(1005, "cal", t_cal, CT_U32, 0, 1000000, 100, PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY | PF_ANY_MODE, NULL),
    PARAM_UINT(1006, "status", t_status, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_VIEW, PF_RO, NULL),
    PARAM_BIT (1007, "ack", t_any, PLAT_LVL_OPERATOR, PF_ANY_MODE),
    { .id = 1008, .ctype = CT_I32, .ptype = PT_INT, .level = PLAT_LVL_OPERATOR, .ptr = &t_lock,
      .vmin.i = -1000, .vmax.i = 1000, .vdef.i = 0, .name = "lock", .interlock = no_odd },
};
#define N (sizeof defs / sizeof defs[0])

static plat_cmd_t cmd(uint16_t id, uint8_t src, uint8_t lvl, uint8_t kind, plat_val_t v) {
    plat_cmd_t c = { .id = id, .src = src, .level = lvl, .kind = kind, .v = v };
    return c;
}
static plat_val_t I(int32_t x) { plat_val_t v; v.i = x; return v; }
static plat_val_t U(uint32_t x) { plat_val_t v; v.u = x; return v; }
static plat_val_t F(float x) { plat_val_t v; v.f = x; return v; }
static plat_val_t B(bool x) { plat_val_t v = { 0 }; v.b = x; return v; }

static void set_mode(uint8_t m) {
    plat_cmd_t c = cmd(PARAM_ID_CTRL_MODE, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, PLAT_VAL_U32, U(m));
    (void)param_apply(&c);
}

static void setup(void) {
    CHECK_EQ(param_init(defs, N), PERR_OK);
    param_set_defaults(PARAM_ALL);
    set_mode(PARAM_MODE_LOCAL);
}

static void t_init_and_defaults(void) {
    setup();
    CHECK_EQ(t_i16, 5); CHECK_EQ(t_u8, 7); CHECK(t_f == 1.5f); CHECK_EQ(t_cal, 100);
    CHECK(param_find(1003) != NULL); CHECK(param_find(999) == NULL);
    int sorted = 1;
    for (size_t i = 1; i < param_count(); ++i) if (param_at(i - 1)->id >= param_at(i)->id) sorted = 0;
    CHECK(sorted);
    CHECK(param_find(PARAM_ID_NET_IP) != NULL);           /* platform entries present */
}

static void t_table_errors(void) {
    static int16_t x;
    param_def_t dup[2] = { PARAM_INT(1001, "a", x, CT_I16, 0, 1, 0, 0, 0, NULL),
                           PARAM_INT(1001, "b", x, CT_I16, 0, 1, 0, 0, 0, NULL) };
    CHECK_EQ(param_init(dup, 2), PERR_TABLE); CHECK_EQ(param_table_error(), 1001);
    param_def_t low[1] = { PARAM_INT(5, "low", x, CT_I16, 0, 1, 0, 0, 0, NULL) };        /* platform range */
    CHECK_EQ(param_init(low, 1), PERR_TABLE);
    param_def_t wide[1] = { PARAM_INT(1010, "w", x, CT_I16, 0, 40000, 0, 0, 0, NULL) };   /* > INT16 */
    CHECK_EQ(param_init(wide, 1), PERR_TABLE);
    param_def_t baddef[1] = { PARAM_INT(1011, "d", x, CT_I16, 0, 10, 11, 0, 0, NULL) };   /* default out of range */
    CHECK_EQ(param_init(baddef, 1), PERR_TABLE);
    param_def_t inv[1] = { PARAM_INT(1012, "inv", x, CT_I16, 5, 1, 3, 0, 0, NULL) };      /* min > max */
    CHECK_EQ(param_init(inv, 1), PERR_TABLE);
    setup();
}

static void t_checks(void) {
    setup();
    CHECK_EQ(param_check(999, PLAT_VAL_I32, I(1), PLAT_SRC_LOCAL, PLAT_LVL_FACTORY), PERR_NO_ID);
    CHECK_EQ(param_check(1006, PLAT_VAL_U32, U(1), PLAT_SRC_LOCAL, PLAT_LVL_FACTORY), PERR_READONLY);
    CHECK_EQ(param_check(1003, PLAT_VAL_F32, F(2), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_ACCESS);
    CHECK_EQ(param_check(1003, PLAT_VAL_F32, F(2), PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);
    /* range edges are inclusive */
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(-100), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(100), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(101), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(-101), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);
    /* types */
    CHECK_EQ(param_check(1001, PLAT_VAL_F32, F(5), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_TYPE);  /* no float->int */
    CHECK_EQ(param_check(1003, PLAT_VAL_I32, I(3), PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);      /* int->float ok */
    CHECK_EQ(param_check(1003, PLAT_VAL_F32, F(NAN), PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_RANGE);
    CHECK_EQ(param_check(1003, PLAT_VAL_F32, F(INFINITY), PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_RANGE);
    CHECK_EQ(param_check(1004, PLAT_VAL_I32, I(1), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(param_check(1004, PLAT_VAL_I32, I(2), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);
    CHECK_EQ(param_check(1002, PLAT_VAL_I32, I(-1), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);  /* uint */
    CHECK_EQ(param_check(1002, PLAT_VAL_U32, U(0x80000005u), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);
}

static void t_arbitration(void) {
    setup();
    /* LOCAL mode: touchscreen yes, web/Modbus no */
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_LOCAL,  PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_WEB,    PLAT_LVL_OPERATOR), PERR_MODE);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR), PERR_MODE);
    /* only the touchscreen may change the mode */
    plat_cmd_t c = cmd(PARAM_ID_CTRL_MODE, PLAT_SRC_WEB, PLAT_LVL_FACTORY, PLAT_VAL_U32, U(1));
    CHECK_EQ(param_apply(&c), PERR_MODE);
    CHECK_EQ(param_ctrl_mode(), PARAM_MODE_LOCAL);
    set_mode(PARAM_MODE_REMOTE);
    CHECK_EQ(param_ctrl_mode(), PARAM_MODE_REMOTE);
    /* REMOTE: web/Modbus yes, touchscreen setpoints no ... */
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_WEB,    PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_LOCAL,  PLAT_LVL_OPERATOR), PERR_MODE);
    /* ... but the local operator can always take control back */
    c = cmd(PARAM_ID_CTRL_MODE, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, PLAT_VAL_U32, U(0));
    CHECK_EQ(param_apply(&c), PERR_OK);
    CHECK_EQ(param_ctrl_mode(), PARAM_MODE_LOCAL);
    /* ANY_MODE entries ignore the mode */
    CHECK_EQ(param_check(1007, PLAT_VAL_BOOL, B(1), PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_OK);
    /* Modbus can be made read-only; network settings never via Modbus */
    set_mode(PARAM_MODE_REMOTE);
    c = cmd(PARAM_ID_MODBUS_WRITE_EN, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN, PLAT_VAL_BOOL, B(0));
    CHECK_EQ(param_apply(&c), PERR_OK);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_MODBUS, PLAT_LVL_FACTORY), PERR_ACCESS);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_OK);
    c = cmd(PARAM_ID_MODBUS_WRITE_EN, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN, PLAT_VAL_BOOL, B(1));
    CHECK_EQ(param_apply(&c), PERR_OK);
    CHECK_EQ(param_check(PARAM_ID_NET_IP, PLAT_VAL_U32, U(1), PLAT_SRC_MODBUS, PLAT_LVL_FACTORY), PERR_ACCESS);
    CHECK_EQ(param_check(PARAM_ID_NET_IP, PLAT_VAL_U32, U(1), PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);  /* recoverable locally */
    /* internal source (store, restore) bypasses mode and level but not range */
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1), PLAT_SRC_INTERNAL, PLAT_LVL_VIEW), PERR_OK);
    CHECK_EQ(param_check(1001, PLAT_VAL_I32, I(1000), PLAT_SRC_INTERNAL, PLAT_LVL_VIEW), PERR_RANGE);
}

static void t_apply_and_read(void) {
    setup();
    plat_cmd_t c = cmd(1001, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, PLAT_VAL_I32, I(-42));
    CHECK_EQ(param_apply(&c), PERR_OK); CHECK_EQ(t_i16, -42);
    c = cmd(1002, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, PLAT_VAL_U32, U(200));
    CHECK_EQ(param_apply(&c), PERR_OK); CHECK_EQ(t_u8, 200);
    c = cmd(1003, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN, PLAT_VAL_I32, I(7));
    CHECK_EQ(param_apply(&c), PERR_OK); CHECK(t_f == 7.0f);
    c = cmd(1001, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, PLAT_VAL_I32, I(500));
    CHECK_EQ(param_apply(&c), PERR_RANGE); CHECK_EQ(t_i16, -42);         /* refused: tag unchanged */
    /* interlock runs at apply only */
    CHECK_EQ(param_check(1008, PLAT_VAL_I32, I(3), PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    interlock_calls = 0;
    c = cmd(1008, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, PLAT_VAL_I32, I(3));
    CHECK_EQ(param_apply(&c), PERR_INTERLOCK); CHECK_EQ(t_lock, 0);
    c.v = I(4); CHECK_EQ(param_apply(&c), PERR_OK); CHECK_EQ(t_lock, 4);
    CHECK_EQ(interlock_calls, 2);
    /* readers see values as of the last publish */
    plat_val_t v;
    CHECK(param_read(1001, &v)); CHECK_EQ(v.i, 5);      /* not yet published */
    param_publish();
    CHECK(param_read(1001, &v)); CHECK_EQ(v.i, -42);
    CHECK(param_read(1003, &v)); CHECK(v.f == 7.0f);
    CHECK(!param_read(4242, &v));
    /* logic writes to a tag also reach readers at publish */
    t_status = 77; param_publish();
    CHECK(param_read(1006, &v)); CHECK_EQ(v.u, 77);
}

static void t_restore_defaults(void) {
    setup();
    t_i16 = 50; t_f = 9.0f; t_cal = 123456;
    param_on_restore_defaults = NULL;                      /* plain table behaviour */
    plat_cmd_t c = cmd(PARAM_ID_RESTORE_DEFAULTS, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR, PLAT_VAL_BOOL, B(1));
    CHECK_EQ(param_apply(&c), PERR_ACCESS);                /* admin only */
    c.level = PLAT_LVL_ADMIN;
    CHECK_EQ(param_apply(&c), PERR_OK);
    CHECK_EQ(t_i16, 5); CHECK(t_f == 1.5f);
    CHECK_EQ(t_cal, 123456);                               /* factory value kept */
    plat_val_t v; param_publish(); CHECK(param_read(PARAM_ID_RESTORE_DEFAULTS, &v)); CHECK(!v.b);  /* momentary */
}

static void t_comms_watchdog(void) {
    setup();
    param_scan_service(1000); CHECK(!param_remote_link_lost());     /* local mode: never lost */
    set_mode(PARAM_MODE_REMOTE);                                    /* default timeout 10 s */
    param_scan_service(50000); CHECK(!param_remote_link_lost());    /* grace from mode change */
    param_scan_service(60000); CHECK(!param_remote_link_lost());
    param_scan_service(60001); CHECK(param_remote_link_lost());
    param_comms_activity(PLAT_SRC_MODBUS, 60005);
    param_scan_service(60010); CHECK(!param_remote_link_lost());
    param_comms_activity(PLAT_SRC_LOCAL, 80000);                    /* local traffic doesn't count */
    param_scan_service(80000); CHECK(param_remote_link_lost());
    plat_cmd_t c = cmd(PARAM_ID_COMMS_TIMEOUT_S, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN, PLAT_VAL_U32, U(0));
    CHECK_EQ(param_apply(&c), PERR_OK);
    param_scan_service(90000); CHECK(!param_remote_link_lost());    /* 0 = watchdog off */
    c.v = U(10); (void)param_apply(&c);
    set_mode(PARAM_MODE_LOCAL);
    param_scan_service(200000); CHECK(!param_remote_link_lost());
}

void test_param_all(void) {
    RUN(t_init_and_defaults);
    RUN(t_table_errors);
    RUN(t_checks);
    RUN(t_arbitration);
    RUN(t_apply_and_read);
    RUN(t_restore_defaults);
    RUN(t_comms_watchdog);
}
