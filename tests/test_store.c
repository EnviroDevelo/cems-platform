/* test_store.c - parameter store: boot, save, dual copies, power cuts, migration,
 * fail-safe, restore defaults, export/import. */
#include <string.h>
#include "tst.h"
#include "param.h"
#include "param_store.h"

static int16_t  s_i16;
static float    s_f;
static uint32_t s_cal;
static bool     s_bit;
static char     s_name[16];
static float    s_curve[4];
static const char  name_def[16] = "Inlet";
static const float curve_def[4] = { 1.0f, 2.0f, 3.0f, 4.0f };

static const param_def_t pdefs[] = {
    PARAM_INT (1001, "i16", s_i16, CT_I16, -100, 100, 5, PLAT_LVL_OPERATOR, PF_PERSIST, NULL),
    PARAM_REAL(1002, "f", s_f, 0.0f, 10.0f, 1.5f, NULL, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_UINT(1003, "cal", s_cal, CT_U32, 0, 1000000, 100, PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY | PF_ANY_MODE, NULL),
    PARAM_BIT (1004, "bit", s_bit, PLAT_LVL_OPERATOR, 0),                     /* not persisted */
};
static const retain_def_t rdefs[] = {
    RETAIN(1100, "name", s_name, 0, name_def),
    RETAIN(1101, "curve", s_curve, RF_FACTORY, curve_def),
};

static uint8_t   fram[128u * 1024u];
static nvm_ram_t ram;
static nvm_dev_t dev;
static uint32_t  now;

/* "power up": clobber RAM, then the normal start-up sequence */
static void boot_v(uint16_t version, param_migrate_fn mig) {
    s_i16 = 99; s_f = 9.9f; s_cal = 999; memset(s_name, 'x', sizeof s_name); memset(s_curve, 0x55, sizeof s_curve);
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK_EQ(param_init(pdefs, sizeof pdefs / sizeof pdefs[0]), PERR_OK);
    CHECK(param_store_init(&dev, NULL, rdefs, sizeof rdefs / sizeof rdefs[0], version, mig));
    param_store_boot();
    /* control mode is runtime and starts LOCAL */
}
static void boot(void) { boot_v(1, NULL); }

/* one full save cycle: request, scan services it, store compares and writes */
static void save_cycle(void) {
    now += STORE_SAVE_PERIOD_MS;
    param_store_poll(now);          /* request */
    param_store_scan_service();     /* scan builds the snapshot */
    param_store_poll(now);          /* compare + write */
}

static void apply(uint16_t id, uint8_t lvl, uint8_t kind, plat_val_t v) {
    plat_cmd_t c = { .id = id, .src = PLAT_SRC_LOCAL, .level = lvl, .kind = kind, .v = v };
    CHECK_EQ(param_apply(&c), PERR_OK);
}

static void fresh(void) { memset(fram, 0, sizeof fram); now = 0; }

static void t_blank_fram(void) {
    fresh(); boot();
    uint32_t st = param_store_status();
    CHECK(st & PSTAT_CAL_INVALID); CHECK(st & PSTAT_USR_DEFAULTED);
    CHECK(!param_store_calibration_valid());
    CHECK_EQ(s_i16, 5); CHECK(s_f == 1.5f); CHECK_EQ(s_cal, 100);
    CHECK(strcmp(s_name, "Inlet") == 0); CHECK(s_curve[3] == 4.0f);
    save_cycle();
    store_stats_t ss = param_store_stats();
    CHECK_EQ(ss.saves_usr, 1);              /* user defaults saved */
    CHECK_EQ(ss.saves_fac, 0);              /* fail safe: factory defaults NOT written */
    save_cycle();
    CHECK_EQ(param_store_stats().saves_fac, 0);
    CHECK(!param_store_calibration_valid());
    /* a deliberate factory change is saved and clears the alarm */
    plat_val_t v; v.u = 4242; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    save_cycle();
    CHECK_EQ(param_store_stats().saves_fac, 1);
    CHECK(param_store_calibration_valid());
}

static void t_roundtrip(void) {
    fresh(); boot();
    plat_val_t v;
    v.i = -77; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
    v.f = 2.25f; apply(1002, PLAT_LVL_OPERATOR, PLAT_VAL_F32, v);
    v.u = 31337; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    strcpy(s_name, "Stack 2");             /* the logic changes a retained block */
    s_curve[0] = 0.5f;
    s_bit = true;
    save_cycle();
    boot();
    CHECK_EQ(s_i16, -77); CHECK(s_f == 2.25f); CHECK_EQ(s_cal, 31337);
    CHECK(strcmp(s_name, "Stack 2") == 0); CHECK(s_curve[0] == 0.5f); CHECK(s_curve[1] == 2.0f);
    CHECK(param_store_status() & PSTAT_FAC_VALID); CHECK(param_store_status() & PSTAT_USR_VALID);
    CHECK(!(param_store_status() & (PSTAT_CAL_INVALID | PSTAT_USR_DEFAULTED | PSTAT_REC_SKIPPED)));
    /* readers see the loaded values straight after boot */
    plat_val_t r; CHECK(param_read(1001, &r)); CHECK_EQ(r.i, -77);
}

static void t_no_change_no_write(void) {
    fresh(); boot();
    plat_val_t v; v.u = 5; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    save_cycle();
    uint32_t w = ram.writes;
    save_cycle(); save_cycle(); save_cycle();
    CHECK_EQ(ram.writes, w);                /* nothing changed, nothing written */
    v.i = 1; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
    save_cycle();
    CHECK_EQ(ram.writes, w + 2);            /* user region only: payload + header */
}

static void t_alternates_copies(void) {
    fresh(); boot();
    uint32_t seq0 = param_store_stats().seq_usr;
    for (int k = 1; k <= 4; ++k) {
        plat_val_t v; v.i = k; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
        save_cycle();
    }
    CHECK_EQ(param_store_stats().seq_usr, seq0 + 4);   /* one save per change */
    /* both copies hold valid images: destroy the newest, the older one loads */
    const store_layout_t *L = &store_layout_default;
    uint32_t newest = ((param_store_stats().seq_usr & 1u) == 1u) ? L->usr_a : L->usr_b;   /* first write went to A */
    fram[newest + 40] ^= 0xFF;
    boot();
    CHECK_EQ(s_i16, 3);                     /* previous value, not defaults */
    CHECK(param_store_status() & PSTAT_USR_VALID);
    /* and the other way round: corrupt the older copy, the newest still loads */
    boot(); { plat_val_t v; v.i = 8; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v); } save_cycle();
    uint32_t older = ((param_store_stats().seq_usr & 1u) == 1u) ? L->usr_b : L->usr_a;
    fram[older + 40] ^= 0xFF;
    boot();
    CHECK_EQ(s_i16, 8);
}

/* Power cut at every byte offset of a save: afterwards the unit must come up
 * with either the old or the new values, never a mixture or defaults. */
static void t_power_cut_every_byte(void) {
    fresh(); boot();
    plat_val_t v; v.u = 1; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    v.i = 10; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
    strcpy(s_name, "OLD");
    save_cycle();
    static uint8_t good[sizeof fram];
    memcpy(good, fram, sizeof fram);
    uint32_t total = param_store_stats().payload_usr + STORE_HDR_SIZE;
    int bad = 0, olds = 0, news = 0;
    for (uint32_t cut = 0; cut <= total; ++cut) {
        memcpy(fram, good, sizeof fram);
        boot();
        v.i = 20; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
        strcpy(s_name, "NEW");
        ram.fail_after = (long)cut;
        save_cycle();
        ram.fail_after = -1;
        boot();
        bool is_old = s_i16 == 10 && strcmp(s_name, "OLD") == 0;
        bool is_new = s_i16 == 20 && strcmp(s_name, "NEW") == 0;
        if (is_old) olds++; else if (is_new) news++; else bad++;
        if (!(param_store_status() & PSTAT_USR_VALID)) bad++;
        if (s_cal != 1) bad++;              /* factory region never disturbed */
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(news, 1);                      /* only the uncut write completes */
    CHECK_EQ(olds, (int)total);
}

static void t_both_factory_copies_bad(void) {
    fresh(); boot();
    plat_val_t v; v.u = 777; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    save_cycle();
    v.u = 778; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    save_cycle();                                       /* both copies now hold data */
    const store_layout_t *L = &store_layout_default;
    fram[L->fac_a + 36] ^= 1; fram[L->fac_b + 36] ^= 1;
    static uint8_t snap[8192];
    memcpy(snap, fram + L->fac_a, sizeof snap);
    boot();
    CHECK(!param_store_calibration_valid());
    CHECK_EQ(s_cal, 100);                               /* default in RAM ... */
    save_cycle(); save_cycle();
    CHECK(memcmp(snap, fram + L->fac_a, sizeof snap) == 0);   /* ... but the damaged images are left alone */
    CHECK(!param_store_calibration_valid());
}

static bool mig_v1_to_v2(uint16_t from, uint16_t id, uint8_t *data, uint16_t *len, uint16_t cap) {
    (void)cap;
    if (from == 1 && id == 1001 && *len == 2) {         /* v2 stores i16 in tenths: rescale */
        int16_t x; memcpy(&x, data, 2); x = (int16_t)(x * 10); if (x > 100) x = 100; memcpy(data, &x, 2);
    }
    return true;
}

static void t_migration(void) {
    fresh(); boot_v(1, NULL);
    plat_val_t v; v.i = 7; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
    v.u = 5; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    save_cycle();
    boot_v(2, mig_v1_to_v2);
    CHECK_EQ(s_i16, 70);
    CHECK(param_store_status() & PSTAT_MIGRATED);
    CHECK_EQ(s_cal, 5);                                 /* calibration survives the upgrade */
    save_cycle();                                       /* rewritten in the new layout */
    boot_v(2, mig_v1_to_v2);
    CHECK_EQ(s_i16, 70);                                /* not migrated twice */
    CHECK(!(param_store_status() & PSTAT_MIGRATED));
    boot_v(1, NULL);                                    /* firmware downgrade: still loads by ID */
    CHECK(param_store_status() & PSTAT_NEWER_LAYOUT);
    CHECK_EQ(s_i16, 70);
}

/* An image written by a firmware with a different item set. */
static void t_unknown_and_resized_records(void) {
    static int16_t extra; static char longname[24];
    static const param_def_t p2[] = {
        PARAM_INT (1001, "i16", s_i16, CT_I16, -100, 100, 5, PLAT_LVL_OPERATOR, PF_PERSIST, NULL),
        PARAM_REAL(1002, "f", s_f, 0.0f, 10.0f, 1.5f, NULL, PLAT_LVL_OPERATOR, PF_PERSIST),
        PARAM_UINT(1003, "cal", s_cal, CT_U32, 0, 1000000, 100, PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY | PF_ANY_MODE, NULL),
        PARAM_INT (1009, "extra", extra, CT_I16, -5, 5, 0, PLAT_LVL_OPERATOR, PF_PERSIST, NULL),
    };
    static const retain_def_t r2[] = { RETAIN(1100, "name", longname, 0, NULL), RETAIN(1101, "curve", s_curve, RF_FACTORY, curve_def) };
    fresh();
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK_EQ(param_init(p2, 4), PERR_OK);
    CHECK(param_store_init(&dev, NULL, r2, 2, 1, NULL));
    param_store_boot();
    extra = 3; strcpy(longname, "TWENTY-THREE CHARACTERS"); s_i16 = 9;
    save_cycle();
    boot();                                             /* back to the normal item set */
    CHECK_EQ(s_i16, 9);                                 /* known record loads */
    CHECK(strcmp(s_name, "Inlet") == 0);                /* size changed: default kept */
    CHECK(param_store_status() & PSTAT_REC_SKIPPED);    /* 1009 unknown, 1100 resized */
    CHECK_EQ(param_store_stats().skipped_records, 2);
}

static void t_out_of_range_stored(void) {
    fresh(); boot();
    plat_val_t v; v.i = 50; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
    save_cycle();
    /* tighten the range in a "new firmware": 50 is now out of range */
    static const param_def_t p3[] = {
        PARAM_INT (1001, "i16", s_i16, CT_I16, -10, 10, 5, PLAT_LVL_OPERATOR, PF_PERSIST, NULL),
        PARAM_REAL(1002, "f", s_f, 0.0f, 10.0f, 1.5f, NULL, PLAT_LVL_OPERATOR, PF_PERSIST),
        PARAM_UINT(1003, "cal", s_cal, CT_U32, 0, 1000000, 100, PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY | PF_ANY_MODE, NULL),
    };
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK_EQ(param_init(p3, 3), PERR_OK);
    CHECK(param_store_init(&dev, NULL, rdefs, 2, 1, NULL));
    param_store_boot();
    CHECK_EQ(s_i16, 5);                                 /* default, not the illegal 50 */
    CHECK(param_store_status() & PSTAT_REC_SKIPPED);
}

static void t_restore_defaults_cmd(void) {
    fresh(); boot();
    plat_val_t v;
    v.i = 33; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
    v.u = 9; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    strcpy(s_name, "Changed"); s_curve[2] = 9.0f;
    save_cycle();
    v.b = true; apply(PARAM_ID_RESTORE_DEFAULTS, PLAT_LVL_ADMIN, PLAT_VAL_BOOL, v);
    CHECK_EQ(s_i16, 5); CHECK(strcmp(s_name, "Inlet") == 0);        /* user data back to defaults */
    CHECK_EQ(s_cal, 9); CHECK(s_curve[2] == 9.0f);                    /* factory data untouched */
    save_cycle(); boot();
    CHECK_EQ(s_i16, 5); CHECK_EQ(s_cal, 9); CHECK(s_curve[2] == 9.0f);
}

static void t_export_import(void) {
    fresh(); boot();
    plat_val_t v;
    v.i = 12; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
    v.u = 600; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    strcpy(s_name, "Backup");
    save_cycle();
    static uint8_t file[64 * 1024];
    uint32_t n = param_store_export(file, sizeof file);
    CHECK(n > 40);
    CHECK_EQ(param_store_export(file, 30), 0);          /* too small: refused */
    n = param_store_export(file, sizeof file);

    v.i = -1; apply(1001, PLAT_LVL_OPERATOR, PLAT_VAL_I32, v);
    v.u = 1; apply(1003, PLAT_LVL_FACTORY, PLAT_VAL_U32, v);
    strcpy(s_name, "Later");
    save_cycle();

    /* tampered file: rejected, nothing applied */
    file[n - 3] ^= 0x40;
    CHECK(param_store_import(file, n, PLAT_LVL_FACTORY) < 0);
    file[n - 3] ^= 0x40;
    CHECK(param_store_import(file, n - 1, PLAT_LVL_FACTORY) < 0);   /* truncated */

    /* admin restore: user part only */
    CHECK_EQ(param_store_import(file, n, PLAT_LVL_ADMIN), 0);
    CHECK(param_store_import_busy());
    CHECK(param_store_import(file, n, PLAT_LVL_ADMIN) < 0);          /* one at a time */
    param_store_scan_service();                                      /* applied at end of scan */
    CHECK(!param_store_import_busy());
    CHECK_EQ(s_i16, 12); CHECK(strcmp(s_name, "Backup") == 0);
    CHECK_EQ(s_cal, 1);                                              /* factory not restored at admin level */
    save_cycle(); boot();
    CHECK_EQ(s_i16, 12); CHECK_EQ(s_cal, 1);

    /* factory restore */
    CHECK_EQ(param_store_import(file, n, PLAT_LVL_FACTORY), 0);
    param_store_scan_service();
    CHECK_EQ(s_cal, 600);
    save_cycle(); boot();
    CHECK_EQ(s_cal, 600);
}

static void t_volatile_and_table_errors(void) {
    fresh();
    nvm_ram_init(&dev, &ram, fram, sizeof fram, false);              /* RAM fallback */
    CHECK_EQ(param_init(pdefs, 4), PERR_OK);
    CHECK(param_store_init(&dev, NULL, rdefs, 2, 1, NULL));
    param_store_boot();
    CHECK(param_store_status() & PSTAT_NVM_VOLATILE);
    CHECK(!param_store_calibration_valid());

    /* FRAM configured but not answering: no device at all */
    CHECK_EQ(param_init(pdefs, 4), PERR_OK);
    CHECK(param_store_init(NULL, NULL, rdefs, 2, 1, NULL));
    param_store_boot();
    CHECK(param_store_status() & PSTAT_NVM_VOLATILE); CHECK(param_store_status() & PSTAT_CAL_INVALID);
    CHECK_EQ(s_i16, 5);                                              /* runs on defaults */
    save_cycle();                                                    /* no crash, nothing to write */
    { static uint8_t f[256]; CHECK_EQ(param_store_export(f, sizeof f), 0); }

    static const retain_def_t clash[] = { RETAIN(1001, "clash", s_name, 0, NULL) };  /* same ID as a param */
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK_EQ(param_init(pdefs, 4), PERR_OK);
    CHECK(!param_store_init(&dev, NULL, clash, 1, 1, NULL));
    CHECK(param_store_status() & PSTAT_TABLE_ERROR);

    static uint8_t huge[STORE_USR_SLOT];
    static const retain_def_t toobig[] = { RETAIN(1200, "huge", huge, 0, NULL) };
    CHECK_EQ(param_init(pdefs, 4), PERR_OK);
    CHECK(!param_store_init(&dev, NULL, toobig, 1, 1, NULL));       /* does not fit the slot */
    boot();                                                          /* leave a sane state */
}

static void t_crc_reference(void) {
    CHECK_EQ(plat_crc32(0, "123456789", 9), 0xCBF43926u);
    CHECK_EQ(plat_crc32(plat_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);   /* incremental */
}

void test_store_all(void) {
    RUN(t_crc_reference);
    RUN(t_blank_fram);
    RUN(t_roundtrip);
    RUN(t_no_change_no_write);
    RUN(t_alternates_copies);
    RUN(t_power_cut_every_byte);
    RUN(t_both_factory_copies_bad);
    RUN(t_migration);
    RUN(t_unknown_and_resized_records);
    RUN(t_out_of_range_stored);
    RUN(t_restore_defaults_cmd);
    RUN(t_export_import);
    RUN(t_volatile_and_table_errors);
}
