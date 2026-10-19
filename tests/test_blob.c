/* test_blob.c - block entries in the parameter table: text, UTF-16 text, bytes,
 * arrays; the block write queue; reader copies; persistence. */
#include <string.h>
#include "tst.h"
#include "param.h"
#include "param_store.h"
#include "plat_cmdq.h"

static char     b_name[11];                 /* like ValveName: 10 chars + NUL */
static uint16_t b_desc[31];                 /* like "Alarm0 Description": UTF-16, 30 units + NUL */
static uint8_t  b_buf[20];                  /* like a Modbus name BUFFER */
static bool     b_ack[9];                   /* like acknowledge bits */
static int16_t  b_vals[4];
static int16_t  b_scalar;
static const char name_def[11] = "Valve 1";

static const param_def_t defs[] = {
    PARAM_TEXT  (2001, "name", b_name, name_def, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_TEXT16(2002, "desc", b_desc, NULL, PLAT_LVL_ADMIN, PF_PERSIST | PF_ANY_MODE),
    PARAM_BYTES (2003, "buf",  b_buf, PLAT_LVL_OPERATOR, 0),
    PARAM_ARRAY_BOOL(2004, "ack", b_ack, PLAT_LVL_OPERATOR, PF_ANY_MODE),
    PARAM_ARRAY_INT (2005, "vals", b_vals, CT_I16, -1000, 1000, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_INT(2006, "scalar", b_scalar, CT_I16, 0, 10, 0, PLAT_LVL_OPERATOR, 0, NULL),
};
#define N (sizeof defs / sizeof defs[0])

static plat_cmdq_t q;
static plat_seqlock_t lk;

static void mode(uint8_t m) {
    plat_cmd_t c = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR, .kind = PLAT_VAL_U32, .v.u = m };
    (void)param_apply(&c);
}
static void setup(void) {
    CHECK_EQ(param_init(defs, N), PERR_OK);
    param_set_defaults(PARAM_ALL);
    plat_cmdq_init(&q); lk.seq = 0;
    param_bind(&q, &lk);
    mode(PARAM_MODE_LOCAL);
    param_publish();
}
static void scan(void) {                      /* what the scan does with queued writes */
    plat_cmd_t c; while (plat_cmdq_pop(&q, &c)) (void)param_apply(&c);
    param_blob_drain();
    param_publish();
}

static void t_blob_defs(void) {
    static char odd[3]; static int16_t arr3[3]; static uint16_t u16odd[1];
    param_def_t bad1[1] = { PARAM_TEXT16(2100, "x", odd, NULL, 0, 0) };      /* odd size UTF-16 */
    CHECK_EQ(param_init(bad1, 1), PERR_TABLE);
    param_def_t bad2[1] = { PARAM_ARRAY_INT(2101, "x", arr3, CT_I16, 5, 1, 0, 0) };   /* min > max */
    CHECK_EQ(param_init(bad2, 1), PERR_TABLE);
    param_def_t ok3[1] = { PARAM_ARRAY_UINT(2102, "x", u16odd, CT_U16, 0, 9, 0, 0) };
    CHECK_EQ(param_init(ok3, 1), PERR_OK);
    static uint8_t huge[20000];
    param_def_t big[1] = { PARAM_BYTES(2103, "huge", huge, 0, 0) };            /* over the reader pool */
    CHECK_EQ(param_init(big, 1), PERR_TABLE);
    setup();
    CHECK(strcmp(b_name, "Valve 1") == 0);
    char r[11]; CHECK_EQ(param_read_blob(2001, 0, r, sizeof r), PERR_OK); CHECK(strcmp(r, "Valve 1") == 0);
    plat_val_t v; CHECK(!param_read(2001, &v));                               /* not a scalar */
    CHECK_EQ(param_check(2001, PLAT_VAL_I32, v, PLAT_SRC_LOCAL, PLAT_LVL_FACTORY), PERR_TYPE);
}

static void t_text(void) {
    setup();
    CHECK_EQ(param_blob_post(2001, 0, "Stack A", 7, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    CHECK(strcmp(b_name, "Valve 1") == 0);                 /* applied by the scan, not before */
    scan();
    CHECK(strcmp(b_name, "Stack A") == 0);
    CHECK_EQ(b_name[7], 0); CHECK_EQ(b_name[10], 0);       /* rest zero-filled */
    CHECK_EQ(param_blob_check(2001, 0, "0123456789", 10, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(param_blob_check(2001, 0, "0123456789X", 11, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);  /* no room for NUL */
    CHECK_EQ(param_blob_check(2001, 0, "a\tb", 3, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);        /* control char */
    CHECK_EQ(param_blob_check(2001, 0, "\xC3\xA9", 2, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);    /* not ASCII */
    CHECK_EQ(param_blob_check(2001, 2, "ab", 2, false, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_TYPE);          /* partial text */
    CHECK_EQ(param_blob_check(2001, 0, "ab", 2, true, PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_MODE);             /* same rules as scalars */
    CHECK_EQ(param_blob_check(2001, 0, "ab", 2, true, PLAT_SRC_LOCAL, PLAT_LVL_VIEW), PERR_ACCESS);
    /* empty string is allowed */
    CHECK_EQ(param_blob_post(2001, 0, "", 0, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    scan(); CHECK_EQ(b_name[0], 0);
}

static void t_text16(void) {
    setup();
    uint8_t s[6] = { 'O', 0, 0xB0, 0x00, 'C', 0 };        /* "O°C" (U+00B0) */
    CHECK_EQ(param_blob_post(2002, 0, s, 6, true, PLAT_SRC_WEB, PLAT_LVL_ADMIN), PERR_OK);   /* ANY_MODE */
    scan();
    CHECK_EQ(b_desc[0], 'O'); CHECK_EQ(b_desc[1], 0xB0); CHECK_EQ(b_desc[2], 'C'); CHECK_EQ(b_desc[3], 0);
    uint8_t sur[2] = { 0x00, 0xD8 };                       /* lone surrogate */
    CHECK_EQ(param_blob_check(2002, 0, sur, 2, true, PLAT_SRC_WEB, PLAT_LVL_ADMIN), PERR_RANGE);
    CHECK_EQ(param_blob_check(2002, 0, s, 5, true, PLAT_SRC_WEB, PLAT_LVL_ADMIN), PERR_RANGE);   /* odd length */
    static uint8_t full[62]; for (int k = 0; k < 62; k += 2) { full[k] = 'x'; full[k + 1] = 0; }
    CHECK_EQ(param_blob_check(2002, 0, full, 60, true, PLAT_SRC_WEB, PLAT_LVL_ADMIN), PERR_OK);  /* 30 units */
    CHECK_EQ(param_blob_check(2002, 0, full, 62, true, PLAT_SRC_WEB, PLAT_LVL_ADMIN), PERR_RANGE);
}

static void t_bytes_and_arrays(void) {
    setup();
    mode(PARAM_MODE_REMOTE);
    uint8_t two[2] = { 0x41, 0x42 };
    CHECK_EQ(param_blob_post(2003, 18, two, 2, false, PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR), PERR_OK);   /* partial */
    CHECK_EQ(param_blob_check(2003, 19, two, 2, false, PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR), PERR_RANGE);
    bool t = true;
    CHECK_EQ(param_blob_post(2004, 8, &t, 1, false, PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR), PERR_OK);
    uint8_t two_b = 2;
    CHECK_EQ(param_blob_check(2004, 0, &two_b, 1, false, PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR), PERR_RANGE); /* bool must be 0/1 */
    int16_t v[2] = { -5, 999 };
    CHECK_EQ(param_blob_post(2005, 2, v, 4, false, PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_OK);
    CHECK_EQ(param_blob_check(2005, 1, v, 4, false, PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_RANGE);   /* misaligned */
    int16_t badv = 1001;
    CHECK_EQ(param_blob_check(2005, 0, &badv, 2, false, PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_RANGE);
    scan();
    CHECK_EQ(b_buf[18], 0x41); CHECK_EQ(b_buf[19], 0x42); CHECK_EQ(b_buf[0], 0);
    CHECK(b_ack[8]);
    CHECK_EQ(b_vals[0], 0); CHECK_EQ(b_vals[1], -5); CHECK_EQ(b_vals[2], 999);
    int16_t rb[2]; CHECK_EQ(param_read_blob(2005, 2, rb, 4), PERR_OK); CHECK_EQ(rb[0], -5);
    CHECK_EQ(param_read_blob(2005, 6, rb, 4), PERR_RANGE);
    CHECK_EQ(param_read_blob(2006, 0, rb, 2), PERR_TYPE);
}

static void t_recheck_at_drain(void) {
    setup();
    mode(PARAM_MODE_REMOTE);
    uint8_t one[1] = { 7 };
    CHECK_EQ(param_blob_post(2003, 0, one, 1, false, PLAT_SRC_WEB, PLAT_LVL_OPERATOR), PERR_OK);
    mode(PARAM_MODE_LOCAL);                                /* operator takes control before the scan */
    scan();
    CHECK_EQ(b_buf[0], 0);                                 /* refused at apply time */
}

static void t_queue_full(void) {
    setup();
    int ok = 0, busy = 0;
    for (int k = 0; k < 20; ++k) {
        param_err_t e = param_blob_post(2001, 0, "x", 1, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR);
        if (e == PERR_OK) ok++; else if (e == PERR_BUSY) busy++;
    }
    CHECK_EQ(ok, 16); CHECK_EQ(busy, 4);
    scan();
    /* block batches: one bad item refuses the lot; too many refuses the lot */
    param_blob_write_t w[3] = { { 2001, 0, 1, true, "a" }, { 2003, 0, 1, false, "b" }, { 2001, 0, 3, true, "a\tb" } };
    CHECK_EQ(param_blob_post_many(w, 3, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);
    CHECK_EQ(param_blob_post_many(w, 2, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    scan(); CHECK(strcmp(b_name, "a") == 0); CHECK_EQ(b_buf[0], 'b');
    static param_blob_write_t w17[17];
    for (int k = 0; k < 17; ++k) w17[k] = (param_blob_write_t){ 2003, (uint16_t)k, 1, false, "c" };
    CHECK_EQ(param_blob_post_many(w17, 17, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_BUSY);
    scan(); CHECK_EQ(b_buf[1], 0);                         /* none of them applied */
    /* scalar batches */
    plat_cmd_t sc[2] = { { .id = 2006, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR, .kind = PLAT_VAL_I32, .v.i = 3 },
                         { .id = 2006, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR, .kind = PLAT_VAL_I32, .v.i = 99 } };
    CHECK_EQ(param_post_many(sc, 2), PERR_RANGE);
    scan(); CHECK_EQ(b_scalar, 0);                         /* the valid one wasn't queued either */
    CHECK_EQ(param_blob_post(2001, 0, "y", 1, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    /* scalar post */
    plat_val_t v; v.i = 4;
    CHECK_EQ(param_post(2006, PLAT_VAL_I32, v, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    v.i = 11;
    CHECK_EQ(param_post(2006, PLAT_VAL_I32, v, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_RANGE);
    scan(); CHECK_EQ(b_scalar, 4);
    for (int k = 0; k < (int)PLAT_CMDQ_DEPTH; ++k) { v.i = 1; (void)param_post(2006, PLAT_VAL_I32, v, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR); }
    CHECK_EQ(param_post(2006, PLAT_VAL_I32, v, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_BUSY);
    scan();
}

static void t_reader_sees_published_only(void) {
    setup();
    strcpy(b_name, "Changed");                             /* the logic writes the tag ... */
    char r[11]; (void)param_read_blob(2001, 0, r, sizeof r);
    CHECK(strcmp(r, "Valve 1") == 0);                      /* ... readers see it at the next publish */
    param_publish();
    (void)param_read_blob(2001, 0, r, sizeof r);
    CHECK(strcmp(r, "Changed") == 0);
    lk.seq = 1;                                            /* scan "stuck" mid-publish */
    CHECK_EQ(param_read_blob(2001, 0, r, sizeof r), PERR_BUSY);
    lk.seq = 2;
}

/* persistence of blocks and validation on load */
static uint8_t fram[128 * 1024]; static nvm_ram_t ram; static nvm_dev_t dev;
static void boot(void) {
    memset(b_name, 'z', sizeof b_name); memset(b_vals, 0x7F, sizeof b_vals);
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK_EQ(param_init(defs, N), PERR_OK);
    CHECK(param_store_init(&dev, NULL, NULL, 0, 1, NULL));
    param_store_boot();
}
static void save(uint32_t *now) {
    *now += 2000; param_store_poll(*now); param_store_scan_service(); param_store_poll(*now);
}
static void t_blob_persistence(void) {
    uint32_t now = 0;
    memset(fram, 0, sizeof fram);
    boot();
    CHECK(strcmp(b_name, "Valve 1") == 0);
    strcpy(b_name, "Inlet 3"); b_vals[3] = -77; b_desc[0] = 'A';
    save(&now);
    boot();
    CHECK(strcmp(b_name, "Inlet 3") == 0); CHECK_EQ(b_vals[3], -77); CHECK_EQ(b_desc[0], 'A');
    CHECK(!(param_store_status() & PSTAT_REC_SKIPPED));
    /* a stored text with no terminator is rejected on load (default kept) */
    memset(b_name, 'Q', sizeof b_name);                    /* 11 x 'Q', no NUL: as if memory corrupted */
    save(&now);
    boot();
    CHECK(strcmp(b_name, "Valve 1") == 0);
    CHECK(param_store_status() & PSTAT_REC_SKIPPED);
    /* an out-of-range array element is rejected on load */
    b_vals[0] = 5000; save(&now); boot();
    CHECK_EQ(b_vals[0], 0); CHECK_EQ(b_vals[3], 0);        /* the whole record falls back to its default */
}

void test_blob_all(void) {
    RUN(t_blob_defs);
    RUN(t_text);
    RUN(t_text16);
    RUN(t_bytes_and_arrays);
    RUN(t_recheck_at_drain);
    RUN(t_queue_full);
    RUN(t_reader_sees_published_only);
    RUN(t_blob_persistence);
}
