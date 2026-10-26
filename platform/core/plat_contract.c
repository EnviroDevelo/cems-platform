/* plat_contract.c - the start-up contract check (see plat_tasks.h, plat_app_check). */
#include <stdio.h>
#include <string.h>
#include "plat_tasks.h"

static int bad(char *why, size_t cap, int code, const char *what) {
    if (why && cap) snprintf(why, cap, "%s", what);
    return code;
}

static bool version_ok(const char *s) {      /* MAJOR.MINOR.PATCH, anything after it allowed ("-dev") */
    for (int part = 0; part < 3; ++part) {
        if (!s || *s < '0' || *s > '9') return false;
        while (*s >= '0' && *s <= '9') s++;
        if (part < 2 && *s++ != '.') return false;
    }
    return true;
}

/* a list and its length agree: both empty, or both given */
#define LIST(P, N, CODE, WHAT) do { if (((P) == NULL) != ((N) == 0)) return bad(why, cap, CODE, WHAT " and its count disagree"); } while (0)

int plat_app_check(const plat_app_t *app, char *why, size_t cap) {
    if (!app) return bad(why, cap, 1, "no application");
    if (!app->scan || !app->scan->logic) return bad(why, cap, 2, "no scan program (scan->logic)");
    if (!app->ident.vendor || !app->ident.vendor[0] || !app->ident.product || !app->ident.product[0])
        return bad(why, cap, 3, "ident: vendor and product are needed");
    if (!version_ok(app->ident.revision)) return bad(why, cap, 4, "ident.revision is not MAJOR.MINOR.PATCH");
    if (app->layout_version == 0) return bad(why, cap, 5, "layout_version starts at 1");
    LIST(app->params, app->n_params, 10, "params");
    LIST(app->retain, app->n_retain, 11, "retain");
    LIST(app->modbus_map, app->n_modbus_map, 12, "modbus_map");
    LIST(app->log_chans, app->n_log_chans, 13, "log_chans");
    LIST(app->ui_home_ids, app->n_ui_home, 14, "ui_home_ids");
    LIST(app->alarms, app->n_alarms, 15, "alarms");
    LIST(app->meas, app->n_meas, 16, "meas");
    LIST(app->cal, app->n_cal, 17, "cal");
    LIST(app->selftests, app->n_selftests, 18, "selftests");
    for (size_t k = 0; k < app->n_params; ++k)            /* R5: 1-999 are the core's */
        if (app->params[k].id < PARAM_ID_APP_FIRST) {
            if (why && cap) snprintf(why, cap, "parameter %u is in the core's range (1-999)", app->params[k].id);
            return 20;
        }
    for (size_t k = 0; k < app->n_alarms; ++k)            /* R5: alarms 1-99 are the core's */
        if (app->alarms[k].id < ALARM_ID_APP_FIRST || app->alarms[k].id > ALARM_ID_MAX) {
            if (why && cap) snprintf(why, cap, "alarm %u is outside 100-255", app->alarms[k].id);
            return 21;
        }
    if (app->n_log_chans > LOG_MAX_CHANS) return bad(why, cap, 22, "more than 32 log channels");
    for (size_t k = 0; k < app->n_log_chans; ++k) {
        const plat_log_chan_t *c = &app->log_chans[k];
        if (!c->name || !c->name[0] || strlen(c->name) > 16u || (c->unit && strlen(c->unit) > 6u)) {
            if (why && cap) snprintf(why, cap, "log channel %u: name 1-16 characters, unit up to 6", (unsigned)k);
            return 23;
        }
    }
    if (app->n_ui_home > 6u) return bad(why, cap, 24, "more than 6 home-screen tiles");
    if (app->n_selftests > PLAT_STEST_APP_MAX) return bad(why, cap, 25, "more than 8 product self-tests");
    for (size_t k = 0; k < app->n_selftests; ++k)
        if (!app->selftests[k].name || !app->selftests[k].name[0] || strlen(app->selftests[k].name) > 15u || !app->selftests[k].run) {
            if (why && cap) snprintf(why, cap, "self-test %u: a name (1-15 characters) and a run function", (unsigned)k);
            return 26;
        }
    if (why && cap) why[0] = 0;
    return 0;
}
