/* test_contract.c - the start-up contract check, plat_app_check (spec section 9.3, R5): a
 * well-formed application passes, and each broken rule is refused with its reason. */
#include <string.h>
#include "tst.h"
#include "plat_tasks.h"

static void logic(void) {}
static const scan_app_t prog = { .logic = logic };
static int32_t sp;
static const param_def_t pars[] = {
    PARAM_INT(1000, "sp", sp, CT_I32, 0, 10, 1, PLAT_LVL_OPERATOR, PF_PERSIST, NULL),
};
static const param_def_t core_par[] = {
    PARAM_INT(500, "sp", sp, CT_I32, 0, 10, 1, PLAT_LVL_OPERATOR, PF_PERSIST, NULL),
};
static const alarm_def_t al_ok[] = { { .id = 100, .name = "A", .text = "a" } };
static const alarm_def_t al_core[] = { { .id = 42, .name = "A", .text = "a" } };
static const plat_log_chan_t ch_long[] = { { "a_name_longer_than_16", NULL, 1 } };
static int run_ok(void) { return 1; }
static const plat_selftest_def_t st_noname[] = { { "", run_ok } };

static plat_app_t good(void) {
    plat_app_t a;
    memset(&a, 0, sizeof a);
    a.scan = &prog;
    a.params = pars; a.n_params = 1;
    a.alarms = al_ok; a.n_alarms = 1;
    a.layout_version = 1;
    a.ident = (mb_ident_t){ .vendor = "V", .product = "P", .revision = "1.2.3-dev" };
    return a;
}

static void t_contract(void) {
    char why[64];
    plat_app_t a = good();
    CHECK_EQ(plat_app_check(&a, why, sizeof why), 0);
    CHECK_EQ(why[0], 0);
    CHECK_EQ(plat_app_check(NULL, why, sizeof why), 1);
    a = good(); a.scan = NULL;                     CHECK_EQ(plat_app_check(&a, why, sizeof why), 2);
    a = good(); a.ident.product = "";              CHECK_EQ(plat_app_check(&a, why, sizeof why), 3);
    a = good(); a.ident.revision = "1.2";          CHECK_EQ(plat_app_check(&a, why, sizeof why), 4);
    CHECK(strstr(why, "MAJOR.MINOR.PATCH") != NULL);
    a = good(); a.ident.revision = NULL;           CHECK_EQ(plat_app_check(&a, why, sizeof why), 4);
    a = good(); a.layout_version = 0;              CHECK_EQ(plat_app_check(&a, why, sizeof why), 5);
    a = good(); a.n_params = 0;                    CHECK_EQ(plat_app_check(&a, why, sizeof why), 10);
    a = good(); a.n_meas = 2;                      CHECK_EQ(plat_app_check(&a, why, sizeof why), 16);
    a = good(); a.params = core_par;               CHECK_EQ(plat_app_check(&a, why, sizeof why), 20);
    CHECK(!strcmp(why, "parameter 500 is in the core's range (1-999)"));
    a = good(); a.alarms = al_core;                CHECK_EQ(plat_app_check(&a, why, sizeof why), 21);
    a = good(); a.log_chans = ch_long; a.n_log_chans = 1; CHECK_EQ(plat_app_check(&a, why, sizeof why), 23);
    static const uint16_t home[7] = { 1000 };
    a = good(); a.ui_home_ids = home; a.n_ui_home = 7;    CHECK_EQ(plat_app_check(&a, why, sizeof why), 24);
    a = good(); a.selftests = st_noname; a.n_selftests = 1; CHECK_EQ(plat_app_check(&a, why, sizeof why), 26);
    a = good(); a.selftests = st_noname; a.n_selftests = 9; CHECK_EQ(plat_app_check(&a, why, sizeof why), 25);
}

void test_contract_all(void) { RUN(t_contract); }
