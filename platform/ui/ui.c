/* ui.c - see ui.h */
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "param.h"
#include "param_store.h"
#include "plat_log.h"
#include "log_csv.h"
#include "fw_update.h"
#include "web_auth.h"
#include "http_util.h"
#include "plat_diag.h"
#include "ul_system.h"
#include "tls.h"
#include "plat_alarm.h"
#include "plat_clock.h"
#include "plat_health.h"
#include "ui_app.h"
#include "iobus.h"

/* ---------------- layout and colours (1024 x 600) ---------------- */
#define W 1024
#define HEAD_H 64
#define BODY_Y 76
#define NAV_Y 528
#define NAV_H 64
#define ROWS 6
#define ROW_H 64
#define TOAST_Y 474           /* message line between the body and the buttons */
#define C_BG     0xF4F5F7u
#define C_HEAD   0x14213Du
#define C_CARD   0xFFFFFFu
#define C_INK    0x1D2330u
#define C_MUTE   0x667085u
#define C_ACCENT 0x1F5FAEu
#define C_PRESS  0x0E3A73u
#define C_OK     0x16794Au
#define C_WARN   0xA15C00u
#define C_BAD    0xB42318u
#define C_LINE   0xD9DDE4u
#define C_GREY   0x8A94A6u
#define F_SMALL 26
#define F_TEXT  28
#define F_BIG   30
#define F_HUGE  31
#define IDLE_DIM_MS (10u * 60u * 1000u)
#define LOCK_MS 30000u

/* tags: 32..126 are keyboard keys (CMD_KEYS uses the character) */
enum {
    T_NAV_HOME = 128, T_NAV_PARAMS, T_NAV_EVENTS, T_NAV_SYSTEM, T_NAV_LOGIN,
    T_MODE, T_PGUP, T_PGDN, T_OK, T_CANCEL, T_BKSP, T_CLEAR, T_SHIFT, T_ON, T_OFF,
    T_LVL1, T_LVL2, T_LVL3, T_YES, T_NO, T_FW_ACT, T_BRIGHT_DN, T_BRIGHT_UP, T_SPACE,
    T_ALARMS, T_ACK_ALL, T_ACTIONS, T_BACK, T_CLOCK, T_IOB,
    T_CFG0 = 160,           /* .. T_CFG0 + ROWS * 6 - 1: alarm action toggles */
    /* application screens and record editors (no keyboard there, so 1..126 are free):
     * 1..8 tabs, T_W0.. widgets and cells (looked up in U.tmap), T_DLG_* the dialog */
    T_TAB0 = 1, T_W0 = 10, T_WMAX = 118, T_DLG_YES = 120, T_DLG_NO = 121,
    T_ROW0 = 200            /* .. T_ROW0 + ROWS - 1 */
};

typedef enum { KB_NUM, KB_IP, KB_TEXT, KB_BOOL } kb_t;
typedef enum { ACT_NONE, ACT_POST, ACT_BLOB, ACT_FW, ACT_FW_LOCAL } act_t;

static struct {
    const ui_cfg_t *cfg;
    int      scr, back;
    uint8_t  last_tag, pressed;
    bool     swallow;
    uint32_t activity;
    bool     dirty;
    uint32_t last_draw;
    uint8_t  bright;
    /* login */
    uint8_t  level, login_level;
    char     pw[AUTH_PW_MAX + 1];
    uint8_t  fails;
    uint32_t lock_until;
    /* settings list and edit */
    size_t   first;
    const param_def_t *ed;
    kb_t     kb;
    bool     shift;
    char     entry[48];
    /* confirm dialog */
    bool     confirm;
    char     confirm_msg[160];
    act_t    act;
    plat_cmd_t pend;
    uint8_t  pend_blob[64];
    uint16_t pend_len;
    /* message line */
    char     toast[96];
    bool     toast_bad;
    uint32_t toast_until;
    /* events */
    uint32_t ev_off;
    uint8_t  ev_buf[ROWS * 64];
    unsigned ev_n;
    bool     ev_busy, ev_want;
    uint32_t ev_t;
    /* alarms */
    size_t   al_first;
    uint16_t al_ids[ROWS];
    size_t   cfg_first;
    /* application screens */
    int      app_scr;
    uint8_t  app_first, app_next, app_hist[8], app_nh;
    uint8_t  tmap_w[128];          /* tag -> widget (or field) index + 1 */
    uint16_t tmap_i[128];          /* tag -> grid item / cell (row << 6 | element) */
    char     ok_msg[48];
    /* record editor */
    const ui_record_t *rec;
    uint8_t  rec_buf[PARAM_RECORD_MAX];
    uint16_t rec_first;            /* table: first row shown; form: first field */
    uint16_t rec_next;
    bool     rec_wait, rec_changed;
    uint32_t rec_ticket, rec_wait_t;
    const ui_field_t *cell_f;      /* UI_EDIT is changing this cell of the copy */
    unsigned cell_row, cell_elem;
    int      rec_back;
} U;

/* ---------------- helpers ---------------- */
static void toast(bool bad, uint32_t now, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void toast(bool bad, uint32_t now, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(U.toast, sizeof U.toast, fmt, ap);
    va_end(ap);
    U.toast_bad = bad;
    U.toast_until = now + 4000u;
    U.dirty = true;
}

static const char *lvl_name(uint8_t l) {
    static const char *const n[] = { "view", "operator", "admin", "factory" };
    return l <= 3 ? n[l] : "?";
}

static bool is_ip(const param_def_t *d) {
    if (d->ptype != PT_UINT || d->ctype != CT_U32 || !d->name) return false;
    return strstr(d->name, "_ip") != NULL || !strcmp(d->name, "net_mask") || !strcmp(d->name, "net_gw");
}

static void fmt_ip(char *b, size_t cap, uint32_t v) {
    snprintf(b, cap, "%u.%u.%u.%u", (unsigned)(v >> 24), (unsigned)((v >> 16) & 255u), (unsigned)((v >> 8) & 255u), (unsigned)(v & 255u));
}

static bool parse_ip(const char *s, uint32_t *out) {
    uint32_t v = 0; int parts = 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return false;
        unsigned long p = strtoul(s, (char **)&s, 10);
        if (p > 255) return false;
        v = (v << 8) | (uint32_t)p; parts++;
        if (*s == '.') s++; else if (*s) return false;
    }
    if (parts != 4) return false;
    *out = v;
    return true;
}

static void fmt_value(const param_def_t *d, char *b, size_t cap) {
    plat_val_t v;
    b[0] = 0;
    if (!PARAM_IS_BLOB(d)) {
        if (!param_read(d->id, &v)) { snprintf(b, cap, "-"); return; }
        switch (d->ptype) {
        case PT_BOOL:  snprintf(b, cap, "%s", v.b ? "on" : "off"); break;
        case PT_INT:   snprintf(b, cap, "%ld", (long)v.i); break;
        case PT_UINT:
            if (is_ip(d)) fmt_ip(b, cap, v.u);
            else if (d->id == PARAM_ID_CTRL_MODE) snprintf(b, cap, "%s", v.u ? "REMOTE" : "LOCAL");
            else snprintf(b, cap, "%lu", (unsigned long)v.u);
            break;
        default:       fmt_float(b, cap, (double)v.f); break;
        }
    } else {
        uint8_t raw[64];
        uint16_t n = d->size < sizeof raw ? d->size : (uint16_t)sizeof raw;
        if (param_read_blob(d->id, 0, raw, n) != PERR_OK) { snprintf(b, cap, "-"); return; }
        if (d->ptype == PT_TEXT) {
            size_t k = 0;
            for (; k < n && raw[k] && k + 1 < cap; ++k) b[k] = (raw[k] >= 32 && raw[k] < 127) ? (char)raw[k] : '?';
            b[k] = 0;
        } else if (d->ptype == PT_TEXT16) {
            size_t k = 0;
            for (uint16_t i = 0; i + 1 < n && k + 1 < cap; i += 2) {
                uint16_t c = (uint16_t)(raw[i] | (raw[i + 1] << 8));
                if (!c) break;
                b[k++] = (c >= 32 && c < 127) ? (char)c : '?';
            }
            b[k] = 0;
        } else if (d->ptype == PT_ARRAY) {
            size_t el = param_ctype_size(d->ctype), k = 0;
            for (uint16_t i = 0; i + el <= n && k + 12 < cap; i = (uint16_t)(i + el)) {
                long x = 0;
                switch (d->ctype) {
                case CT_BOOL: case CT_U8: x = raw[i]; break;
                case CT_I8: x = (int8_t)raw[i]; break;
                case CT_U16: x = (uint16_t)(raw[i] | (raw[i + 1] << 8)); break;
                case CT_I16: x = (int16_t)(raw[i] | (raw[i + 1] << 8)); break;
                default: { uint32_t u; memcpy(&u, raw + i, 4); x = d->ctype == CT_I32 ? (long)(int32_t)u : (long)u; }
                }
                k += (size_t)snprintf(b + k, cap - k, "%s%ld", i ? ", " : "", x);
            }
            if (d->size > n || k + 12 >= cap) snprintf(b + k, cap - k, " ...");
        } else {
            size_t k = 0;
            for (uint16_t i = 0; i < n && i < 12 && k + 3 < cap; ++i) k += (size_t)snprintf(b + k, cap - k, "%02X", raw[i]);
            if (d->size > 12) snprintf(b + k, cap - k, "...");
        }
    }
    if (d->unit && d->ptype != PT_BOOL && strlen(b) + strlen(d->unit) + 2 < cap) { strcat(b, " "); strcat(b, d->unit); }
}

static uint8_t val_kind(const param_def_t *d) {
    switch (d->ptype) {
    case PT_BOOL: return PLAT_VAL_BOOL;
    case PT_INT:  return PLAT_VAL_I32;
    case PT_FLOAT: return PLAT_VAL_F32;
    default: return PLAT_VAL_U32;
    }
}

static const char *perr_text(param_err_t e) {
    switch (e) {
    case PERR_RANGE: return "Outside the allowed range";
    case PERR_MODE: return "Not allowed in REMOTE mode: the PLC or web is in control";
    case PERR_ACCESS: return "Log in at a higher level first";
    case PERR_READONLY: return "Read only";
    case PERR_TYPE: return "Not a valid value";
    case PERR_BUSY: return "Instrument busy, try again";
    case PERR_INTERLOCK: return "Refused by an interlock";
    default: return "Refused";
    }
}

/* why a row can't be changed from here, or NULL if it can */
static const char *why_not(const param_def_t *d) {
    if (d->flags & PF_RO) return "Read only";
    if (d->flags & PF_DEV) return "Changed only with a developer token (web page)";
    if (d->ptype == PT_BYTES || d->ptype == PT_ARRAY) return "Change this one on the web page";
    param_err_t e;
    if (PARAM_IS_BLOB(d)) {                      /* text: may this level write it in this mode? */
        static const uint8_t empty[2];
        e = param_blob_check(d->id, 0, empty, 0, true, PLAT_SRC_LOCAL, U.level);
    } else {
        plat_val_t v;
        if (!param_read(d->id, &v)) return "Not available";
        e = param_check(d->id, val_kind(d), v, PLAT_SRC_LOCAL, U.level);
    }
    if (e == PERR_ACCESS) {
        static char m[48];
        snprintf(m, sizeof m, "Log in as %s to change this", lvl_name(d->level));
        return m;
    }
    return e == PERR_OK ? NULL : perr_text(e);
}

static void goto_scr(int s) { U.scr = s; U.dirty = true; U.confirm = false; }

static void ask(act_t act, const char *msg) {
    U.act = act;
    snprintf(U.confirm_msg, sizeof U.confirm_msg, "%s", msg);
    U.confirm = true;
    U.dirty = true;
}

static void do_action(uint32_t now) {
    param_err_t e = PERR_OK;
    if (U.act == ACT_POST) {
        e = param_post(U.pend.id, U.pend.kind, U.pend.v, PLAT_SRC_LOCAL, U.pend.level);
    } else if (U.act == ACT_BLOB) {
        e = param_blob_post(U.pend.id, 0, U.pend_blob, U.pend_len, true, PLAT_SRC_LOCAL, U.pend.level);
    } else if (U.act == ACT_FW) {
        fw_err_t f = fw_request_activate_from(PLAT_SRC_LOCAL, U.level);
        fw_status_t st; fw_status(&st);
        if (f == FW_E_NOT_SAFE) toast(true, now, "Not now: %s", st.not_safe);
        else if (f != FW_OK) toast(true, now, "Cannot restart: %s", fw_err_name(f));
        else toast(false, now, "Restarting on the new firmware...");
        U.act = ACT_NONE;
        return;
    } else if (U.act == ACT_FW_LOCAL) {
        fw_local_answer(true, U.level);
        fw_status_t st; fw_status(&st);
        if (st.not_safe[0]) toast(true, now, "Not now: %s", st.not_safe);
        else toast(false, now, "Restarting on the new firmware...");
        U.act = ACT_NONE;
        return;
    }
    U.act = ACT_NONE;
    if (e != PERR_OK) { U.ok_msg[0] = 0; toast(true, now, "%s", perr_text(e)); return; }
    toast(false, now, "%s", U.ok_msg[0] ? U.ok_msg : "Saved");
    U.ok_msg[0] = 0;
    if (U.scr == UI_EDIT) goto_scr(U.back);
}

/* ---------------- edit ---------------- */
static bool rec_open(const ui_record_t *r, uint32_t now);
static void edit_begin(const param_def_t *d, uint32_t now) {
    if ((d->flags & PF_RECORD) && ui_app_record(d->id)) { (void)rec_open(ui_app_record(d->id), now); return; }
    const char *why = why_not(d);
    if (why) { toast(true, now, "%s", why); return; }
    U.ed = d;
    U.shift = false;
    U.entry[0] = 0;
    U.kb = d->ptype == PT_BOOL ? KB_BOOL : is_ip(d) ? KB_IP : PARAM_IS_BLOB(d) ? KB_TEXT : KB_NUM;
    if (U.kb == KB_TEXT || U.kb == KB_IP) {               /* start from the current value */
        if (U.kb == KB_IP || !d->unit) fmt_value(d, U.entry, sizeof U.entry);
    }
    U.back = U.scr;
    goto_scr(UI_EDIT);
}

static bool cell_submit(bool on, uint32_t now);
static void edit_submit(bool on, uint32_t now) {
    if (U.cell_f) { if (cell_submit(on, now)) goto_scr(UI_REC); return; }
    const param_def_t *d = U.ed;
    plat_cmd_t c = { .id = d->id, .src = PLAT_SRC_LOCAL, .level = U.level, .kind = val_kind(d) };
    if (PARAM_IS_BLOB(d)) {
        memset(U.pend_blob, 0, sizeof U.pend_blob);
        if (d->ptype == PT_TEXT16) {
            int n = utf8_to_utf16le(U.entry, U.pend_blob, sizeof U.pend_blob);
            if (n < 0 || (uint16_t)n + 2u > d->size) { toast(true, now, "Too long"); return; }
            U.pend_len = (uint16_t)n;
        } else {
            size_t n = strlen(U.entry);
            if (n + 1u > d->size || n > sizeof U.pend_blob) { toast(true, now, "Too long (at most %u characters)", (unsigned)d->size - 1u); return; }
            memcpy(U.pend_blob, U.entry, n);
            U.pend_len = (uint16_t)n;
        }
        param_err_t e = param_blob_check(d->id, 0, U.pend_blob, U.pend_len, true, PLAT_SRC_LOCAL, U.level);
        if (e != PERR_OK) { toast(true, now, "%s", perr_text(e)); return; }
        U.pend = c;
        U.act = ACT_BLOB;
    } else {
        char *end = NULL;
        switch (d->ptype) {
        case PT_BOOL: c.v.b = on; break;
        case PT_INT: { long x = strtol(U.entry, &end, 10); if (!U.entry[0] || *end) { toast(true, now, "Not a valid number"); return; } c.v.i = (int32_t)x; break; }
        case PT_FLOAT: { double x = strtod(U.entry, &end); if (!U.entry[0] || *end || !isfinite(x)) { toast(true, now, "Not a valid number"); return; } c.v.f = (float)x; break; }
        default:
            if (U.kb == KB_IP) { if (!parse_ip(U.entry, &c.v.u)) { toast(true, now, "Enter an address like 192.168.1.50"); return; } }
            else { unsigned long x = strtoul(U.entry, &end, 10); if (!U.entry[0] || *end || U.entry[0] == '-') { toast(true, now, "Not a valid number"); return; } c.v.u = (uint32_t)x; }
        }
        param_err_t e = param_check(d->id, c.kind, c.v, PLAT_SRC_LOCAL, U.level);
        if (e != PERR_OK) { toast(true, now, "%s", perr_text(e)); return; }
        U.pend = c;
        U.act = ACT_POST;
    }
    if (d->id == PARAM_ID_RESTORE_DEFAULTS) {
        if (!c.v.b) { goto_scr(U.back); return; }
        ask(U.act, "Put every user setting back to its default? Calibration is kept.");
    } else if (d->flags & PF_NO_MODBUS && (d->id >= PARAM_ID_NET_DHCP && d->id <= PARAM_ID_NET_GW)) {
        ask(U.act, "Change the network settings now? Connected PLCs and browsers will lose the connection.");
    } else if (d->flags & PF_CONFIRM) {
        char m[96]; snprintf(m, sizeof m, "Change %s?", d->name);
        ask(U.act, m);
    } else {
        do_action(now);
    }
}

static void edit_key(uint8_t k) {
    size_t n = strlen(U.entry);
    if (n + 1 >= sizeof U.entry) return;
    char ch = (char)k;
    if (U.kb == KB_TEXT && U.shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    U.entry[n] = ch;
    U.entry[n + 1] = 0;
}

/* ---------------- login ---------------- */
static void login_submit(uint32_t now) {
    if (now < U.lock_until) { toast(true, now, "Too many attempts: wait %u s", (unsigned)((U.lock_until - now + 999u) / 1000u)); return; }
    if (!U.login_level) { toast(true, now, "Choose operator, admin or factory"); return; }
    if (auth_check(U.login_level, U.pw)) {
        U.level = U.login_level;
        U.fails = 0;
        plat_log_event(LOG_EV_LOGIN, PLAT_SRC_LOCAL, U.level, 0, NAN, NAN, NULL);
        memset(U.pw, 0, sizeof U.pw);
        toast(false, now, "Logged in as %s", lvl_name(U.level));
        goto_scr(U.back);
    } else {
        plat_log_event(LOG_EV_LOGIN_FAIL, PLAT_SRC_LOCAL, U.login_level, 0, NAN, NAN, NULL);
        memset(U.pw, 0, sizeof U.pw);
        if (++U.fails >= 5) { U.fails = 0; U.lock_until = now + LOCK_MS; toast(true, now, "Wrong password. Locked for 30 s"); }
        else toast(true, now, "Wrong password");
    }
}

static void logout(uint32_t now, const char *why) {
    if (!U.level) return;
    plat_log_event(LOG_EV_LOGOUT, PLAT_SRC_LOCAL, U.level, 0, NAN, NAN, why);
    U.level = 0;
    toast(false, now, "Logged out%s%s", why ? ": " : "", why ? why : "");
    if (U.scr == UI_EDIT) goto_scr(UI_PARAMS);
}

/* ---------------- mode ---------------- */
static void mode_tap(uint32_t now) {
    bool remote = param_ctrl_mode() == PARAM_MODE_REMOTE;
    plat_cmd_t c = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .kind = PLAT_VAL_U32 };
    if (remote) {                       /* the local operator can always take control back */
        c.v.u = PARAM_MODE_LOCAL;
        c.level = U.level > PLAT_LVL_OPERATOR ? U.level : PLAT_LVL_OPERATOR;
        U.pend = c; U.act = ACT_POST;
        ask(ACT_POST, "Take LOCAL control? The PLC and web can then only read.");
    } else {
        if (U.level < PLAT_LVL_OPERATOR) { toast(true, now, "Log in as operator to hand control to the PLC/web"); return; }
        c.v.u = PARAM_MODE_REMOTE;
        c.level = U.level;
        U.pend = c; U.act = ACT_POST;
        ask(ACT_POST, "Hand control to the PLC and web (REMOTE)? Changes here are then refused.");
    }
}

/* ---------------- input ---------------- */
static size_t n_params(void) { return param_count(); }

static bool app_tap(uint8_t t, uint32_t now);
static void rec_tap(uint8_t t, uint32_t now);
static void tap(uint8_t t, uint32_t now) {
    if (U.scr == UI_APP && !U.confirm && app_tap(t, now)) return;     /* its dialog comes first */
    if (U.confirm) {
        if (t == T_YES) { U.confirm = false; do_action(now); }
        else if (t == T_NO) {
            if (U.act == ACT_FW_LOCAL) { fw_local_answer(false, U.level); toast(false, now, "Firmware restart refused"); }
            U.confirm = false; U.act = ACT_NONE;
        }
        U.dirty = true;
        return;
    }
    switch (t) {
    case T_NAV_HOME: goto_scr(ui_app() && ui_app()->n_screens ? UI_APP : UI_HOME); return;
    case T_NAV_PARAMS: goto_scr(UI_PARAMS); return;
    case T_NAV_EVENTS: U.ev_off = 0; U.ev_want = true; goto_scr(UI_EVENTS); return;
    case T_NAV_SYSTEM: goto_scr(UI_SYSTEM); return;
    case T_NAV_LOGIN:
        if (U.level) { logout(now, NULL); return; }
        U.back = U.scr == UI_LOGIN || U.scr == UI_EDIT ? UI_HOME : U.scr;
        U.login_level = PLAT_LVL_OPERATOR; U.pw[0] = 0; U.shift = false;
        goto_scr(UI_LOGIN);
        return;
    case T_MODE: mode_tap(now); return;
    case T_ALARMS: U.al_first = 0; goto_scr(UI_ALARMS); return;
    case T_ACTIONS: U.cfg_first = 0; goto_scr(UI_ALARM_CFG); return;
    default: break;
    }
    switch (U.scr) {
    case UI_PARAMS:
        if (t == T_PGUP) { U.first = U.first >= ROWS ? U.first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { if (U.first + ROWS < n_params()) U.first += ROWS; U.dirty = true; }
        else if (t >= T_ROW0 && t < T_ROW0 + ROWS && U.first + (size_t)(t - T_ROW0) < n_params())
            edit_begin(param_at(U.first + (size_t)(t - T_ROW0)), now);
        break;
    case UI_REC: rec_tap(t, now); break;
    case UI_EDIT:
        if (t == T_CANCEL) { if (U.cell_f) { U.cell_f = NULL; goto_scr(UI_REC); } else goto_scr(U.back); }
        else if (t == T_OK) edit_submit(true, now);
        else if (t == T_ON || t == T_OFF) { strcpy(U.entry, t == T_ON ? "1" : "0"); edit_submit(t == T_ON, now); }
        else if (t == T_BKSP) { size_t n = strlen(U.entry); if (n) U.entry[n - 1] = 0; }
        else if (t == T_CLEAR) U.entry[0] = 0;
        else if (t == T_SHIFT) U.shift = !U.shift;
        else if (t == T_SPACE) edit_key(' ');
        else if (t >= 32 && t < 127) edit_key(t);
        U.dirty = true;
        break;
    case UI_LOGIN:
        if (t >= T_LVL1 && t <= T_LVL3) U.login_level = (uint8_t)(t - T_LVL1 + 1);
        else if (t == T_OK) login_submit(now);
        else if (t == T_CANCEL) { memset(U.pw, 0, sizeof U.pw); goto_scr(U.back); }
        else if (t == T_BKSP) { size_t n = strlen(U.pw); if (n) U.pw[n - 1] = 0; }
        else if (t == T_SHIFT) U.shift = !U.shift;
        else if (t >= 33 && t < 127) {
            size_t n = strlen(U.pw);
            char ch = (char)t;
            if (U.shift && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            if (n + 1 < sizeof U.pw) { U.pw[n] = ch; U.pw[n + 1] = 0; }
        }
        U.dirty = true;
        break;
    case UI_EVENTS:
        if (t == T_PGUP && U.ev_off >= ROWS) { U.ev_off -= ROWS; U.ev_want = true; }
        else if (t == T_PGDN) { U.ev_off += ROWS; U.ev_want = true; }
        U.dirty = true;
        break;
    case UI_ALARMS:
        if (t == T_PGUP) { U.al_first = U.al_first >= ROWS ? U.al_first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { U.al_first += ROWS; U.dirty = true; }
        else if (t == T_ACK_ALL || (t >= T_ROW0 && t < T_ROW0 + ROWS && U.al_ids[t - T_ROW0])) {
            uint16_t id = t == T_ACK_ALL ? ALARM_ACK_ALL : U.al_ids[t - T_ROW0];
            param_err_t e = param_post(PARAM_ID_ALARM_ACK, PLAT_VAL_U32, (plat_val_t){ .u = id }, PLAT_SRC_LOCAL, U.level);
            if (e != PERR_OK) toast(true, now, "%s", perr_text(e));
            else toast(false, now, t == T_ACK_ALL ? "All alarms acknowledged" : "Acknowledged");
        }
        break;
    case UI_CLOCK:
        if (t == T_CANCEL) goto_scr(UI_SYSTEM);
        else if (t == T_BKSP) { size_t n = strlen(U.entry); if (n) U.entry[n - 1] = 0; }
        else if (t == T_CLEAR) U.entry[0] = 0;
        else if (t >= '0' && t <= '9') { if (strlen(U.entry) < 14) edit_key(t); }
        else if (t == T_OK) {
            unsigned v[6] = { 0 }, len[6] = { 4, 2, 2, 2, 2, 2 };
            size_t n = strlen(U.entry), p = 0;
            for (int k = 0; k < 6; ++k) for (unsigned j = 0; j < len[k]; ++j, ++p) v[k] = v[k] * 10u + (p < n ? (unsigned)(U.entry[p] - '0') : 0u);
            static const uint8_t mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            if (n < 12 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > mdays[v[1] - 1] || v[3] > 23 || v[4] > 59 || v[5] > 59) {
                toast(true, now, "Enter year, month, day, hour, minute (and seconds)"); break;
            }
            uint32_t tt = plat_unix_from_civil(v[0], v[1], v[2], v[3], v[4], v[5]);
            if (plat_clock_request(tt, PLAT_SRC_LOCAL, U.level, "touchscreen") != 0) { toast(true, now, "Year must be 2020 to 2099"); break; }
            toast(false, now, "Clock set");
            goto_scr(UI_SYSTEM);
        }
        U.dirty = true;
        break;
    case UI_ALARM_CFG:
        if (t == T_BACK) goto_scr(UI_ALARMS);
        else if (t == T_PGUP) { U.cfg_first = U.cfg_first >= ROWS ? U.cfg_first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { if (U.cfg_first + ROWS < alarm_count()) U.cfg_first += ROWS; U.dirty = true; }
        else if (t >= T_CFG0 && t < T_CFG0 + ROWS * 6) {
            unsigned r = (unsigned)(t - T_CFG0) / 6u, b = (unsigned)(t - T_CFG0) % 6u;
            const alarm_def_t *d = alarm_def_at(U.cfg_first + r);
            if (!d) break;
            if (U.level < PLAT_LVL_ADMIN) { toast(true, now, "Log in as admin to change alarm actions"); break; }
            static const uint8_t bit[6] = { AR_EMAIL, AR_MODBUS, AR_RELAY(0), AR_RELAY(1), AR_RELAY(2), AR_RELAY(3) };
            uint8_t v = (uint8_t)(AR_SET | (alarm_route(d->id) ^ bit[b]));
            param_err_t e = param_blob_post(PARAM_ID_ALARM_ROUTE, d->id, &v, 1, false, PLAT_SRC_LOCAL, U.level);
            if (e != PERR_OK) toast(true, now, "%s", perr_text(e));
            U.dirty = true;
        }
        break;
    case UI_SYSTEM:
        if (t == T_CLOCK) {
            if (U.level < PLAT_LVL_ADMIN) { toast(true, now, "Log in as admin to set the clock"); break; }
            uint32_t cur;
            if (plat_clock_now(&cur)) {
                unsigned y, mo, d, h, mi, s;
                plat_civil_from_unix(cur, &y, &mo, &d, &h, &mi, &s);
                snprintf(U.entry, sizeof U.entry, "%04u%02u%02u%02u%02u%02u", y % 10000u, mo % 100u, d % 100u, h % 100u, mi % 100u, s % 100u);
            } else U.entry[0] = 0;
            U.kb = KB_IP;
            goto_scr(UI_CLOCK);
        } else if (t == T_FW_ACT) {
            fw_status_t st; fw_status(&st);
            if (U.level < PLAT_LVL_FACTORY) { toast(true, now, "Log in at factory (tech) level to change firmware"); break; }
            char m[96]; snprintf(m, sizeof m, "Restart on firmware %s now? Measurement stops during the restart.", st.staged_version);
            ask(ACT_FW, m);
        } else if (t == T_IOB) goto_scr(UI_IOB);
        else if (t == T_BRIGHT_DN) { U.bright = U.bright > 20 ? (uint8_t)(U.bright - 20) : 10; U.dirty = true; }
        else if (t == T_BRIGHT_UP) { U.bright = U.bright < 80 ? (uint8_t)(U.bright + 20) : 100; U.dirty = true; }
        break;
    default:
        break;
    }
}

void ui_input(uint8_t tag, uint32_t now) {
    if (U.level && now - U.activity > AUTH_IDLE_MS) logout(now, "idle");
    if (tag && !U.last_tag) {                          /* finger down */
        bool dimmed = now - U.activity > IDLE_DIM_MS;
        U.activity = now;
        ui_last_touch_ms = now ? now : 1u;
        U.pressed = tag;
        U.swallow = dimmed;                            /* the first touch only wakes the screen */
        U.dirty = true;
    } else if (!tag && U.last_tag) {                   /* finger up on the same thing it went down on */
        uint8_t t = U.pressed;
        U.pressed = 0;
        U.dirty = true;
        if (!U.swallow && t && t == U.last_tag && t != 255) tap(t, now);
        U.swallow = false;
    } else if (tag && tag != U.pressed) {
        U.pressed = 0;                                 /* slid off: no tap */
        U.dirty = true;
    }
    U.last_tag = tag;
}

/* ---------------- drawing ---------------- */
static eve_t *E;
static void color(uint32_t c) { eve_color(E, c); }
/* the font to use for a ROM font size: our own (with ° ² ³ µ ...) when it loaded */
static int16_t fnt(int16_t rom, bool *own) {
    for (unsigned i = 0; i < EVE_FONTS; ++i)
        if (eve_fonts[i].rom == rom && (E->fonts_ok & (1u << i))) { *own = true; return (int16_t)(EVE_FONT_HANDLE0 + i); }
    *own = false;
    return rom;
}
static void text(int16_t x, int16_t y, int16_t f, uint16_t o, const char *s) {
    bool own; int16_t h = fnt(f, &own);
    char b[160];
    eve_text(E, x, y, h, o, eve_font_encode(s, b, sizeof b, own));
}
static void ebutton(int16_t x, int16_t y, int16_t w, int16_t h, int16_t f, uint16_t o, const char *s) {
    bool own; int16_t fh = fnt(f, &own);
    char b[96];
    eve_button(E, x, y, w, h, fh, o, eve_font_encode(s, b, sizeof b, own));
}
static void ekeys(int16_t x, int16_t y, int16_t w, int16_t h, int16_t f, uint16_t o, const char *s) {
    bool own; eve_keys(E, x, y, w, h, fnt(f, &own), o, s);
}
static void box(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t c, uint16_t r) { color(c); eve_rect(E, x, y, w, h, r); }

static void button(uint8_t tag, int16_t x, int16_t y, int16_t w, int16_t h, const char *label, uint32_t fg, bool enabled) {
    eve_tag(E, enabled ? tag : 0);
    eve_fgcolor(E, !enabled ? C_GREY : (U.pressed == tag ? C_PRESS : fg));
    color(0xFFFFFFu);
    ebutton(x, y, w, h, F_TEXT, OPT_FLAT, label);
    eve_tag(E, 0);
}

static void header(uint32_t now) {
    box(0, 0, W, HEAD_H, C_HEAD, 1);
    color(0xFFFFFFu);
    text(16, HEAD_H / 2, 29, OPT_CENTERY, U.cfg && U.cfg->product ? U.cfg->product : "CEMS Supervisor");
    bool remote = param_ctrl_mode() == PARAM_MODE_REMOTE;
    int16_t x = 520;
    box(x, 14, 130, 36, remote ? C_OK : 0x5B6577u, 18);
    color(0xFFFFFFu);
    text((int16_t)(x + 65), 32, F_TEXT, OPT_CENTER, remote ? "REMOTE" : "LOCAL");
    x = (int16_t)(x + 140);
    alarm_summary_t as;
    alarm_summary(&as);
    if (as.shown) {                                   /* one pill for every alarm: tap for the list */
        char a[32];
        if (as.unacked) snprintf(a, sizeof a, "%u ALARM%s  %u NEW", as.active, as.active == 1 ? "" : "S", as.unacked);
        else snprintf(a, sizeof a, "%u ALARM%s", as.active, as.active == 1 ? "" : "S");
        uint32_t c = !as.worst_unacked ? 0x5B6577u : as.worst >= AL_SEV_ALARM ? C_BAD : as.worst == AL_SEV_WARNING ? C_WARN : 0x5B6577u;
        eve_tag(E, T_ALARMS);
        box(x, 10, 230, 44, U.pressed == T_ALARMS ? C_PRESS : c, 22);
        color(0xFFFFFFu); text((int16_t)(x + 115), 32, F_TEXT, OPT_CENTER, a);
        eve_tag(E, 0);
    }
    char t[16];
    snprintf(t, sizeof t, "%02u:%02u:%02u", RTC_Time.Hours, RTC_Time.Minutes, RTC_Time.Seconds);
    color(0xC9D2E3u);
    text(W - 16, HEAD_H / 2, F_TEXT, OPT_CENTERY | OPT_RIGHTX, RTC_Date.Year >= 2000 ? t : "--:--:--");
    (void)now;
}

static void nav(void) {
    static const char *const lab[] = { "Home", "Settings", "Events", "System" };
    static const int scr_of[] = { UI_HOME, UI_PARAMS, UI_EVENTS, UI_SYSTEM };
    box(0, NAV_Y - 4, W, NAV_H + 8, 0xE4E7ECu, 1);
    for (int k = 0; k < 4; ++k) {
        bool cur = U.scr == scr_of[k] || (k == 1 && U.scr == UI_EDIT && !U.cell_f) || (k == 0 && (U.scr == UI_APP || U.scr == UI_REC)) ||
                   (k == 3 && (U.scr == UI_IOB || U.scr == UI_CLOCK));
        button((uint8_t)(T_NAV_HOME + k), (int16_t)(8 + k * 202), NAV_Y, 194, NAV_H, lab[k], cur ? C_HEAD : C_ACCENT, true);
    }
    char b[32];
    if (U.level) snprintf(b, sizeof b, "Log out %s", lvl_name(U.level));
    else snprintf(b, sizeof b, "Log in");
    button(T_NAV_LOGIN, 8 + 4 * 202, NAV_Y, 200, NAV_H, b, U.level ? C_WARN : C_ACCENT, true);
}

static void draw_home(void) {
    const ui_cfg_t *c = U.cfg;
    unsigned n = c && c->home_ids ? (c->n_home > 6 ? 6 : c->n_home) : 0;
    for (unsigned k = 0; k < n; ++k) {
        const param_def_t *d = param_find(c->home_ids[k]);
        int16_t x = (int16_t)(16 + (k % 3) * 336), y = (int16_t)(BODY_Y + (k / 3) * 140);
        box(x, y, 320, 130, C_CARD, 8);
        color(C_MUTE);
        text((int16_t)(x + 16), (int16_t)(y + 12), F_TEXT, 0, d ? d->name : "?");
        char v[64] = "-";
        if (d) fmt_value(d, v, sizeof v);
        color(C_INK);
        text((int16_t)(x + 16), (int16_t)(y + 56), F_HUGE, 0, v);
    }
    int16_t y = (int16_t)(BODY_Y + (n > 3 ? 280 : n ? 140 : 10));
    box(16, y, 992, 110, C_CARD, 8);
    bool remote = param_ctrl_mode() == PARAM_MODE_REMOTE;
    color(C_INK);
    text(36, (int16_t)(y + 18), F_BIG, 0, remote ? "Control: REMOTE (PLC and web)" : "Control: LOCAL (this touchscreen)");
    color(C_MUTE);
    text(36, (int16_t)(y + 64), F_TEXT, 0, remote ? "Changes here are refused until you take control back."
                                                  : "The PLC and web can read but not change settings.");
    button(T_MODE, 724, (int16_t)(y + 23), 264, 64, remote ? "Take LOCAL control" : "Hand to REMOTE", remote ? C_WARN : C_ACCENT, true);
}

static void draw_params(void) {
    size_t n = n_params();
    box(16, BODY_Y, 880, ROWS * ROW_H + 8, C_CARD, 8);
    for (int r = 0; r < ROWS; ++r) {
        size_t i = U.first + (size_t)r;
        if (i >= n) break;
        const param_def_t *d = param_at(i);
        int16_t y = (int16_t)(BODY_Y + 4 + r * ROW_H);
        bool can = why_not(d) == NULL;
        eve_tag(E, (uint8_t)(T_ROW0 + r));
        if (U.pressed == T_ROW0 + r) box(20, y, 872, ROW_H - 2, 0xDCE6F5u, 4);
        else box(20, y, 872, ROW_H - 2, C_CARD, 1);           /* touch area of the row */
        char b[64];
        snprintf(b, sizeof b, "%u", d->id);
        color(C_MUTE); text(32, (int16_t)(y + ROW_H / 2), F_SMALL, OPT_CENTERY, b);
        color(C_INK);  text(110, (int16_t)(y + ROW_H / 2), F_TEXT, OPT_CENTERY, d->name);
        fmt_value(d, b, sizeof b);
        if (strlen(b) > 22) strcpy(b + 19, "...");
        text(440, (int16_t)(y + ROW_H / 2), F_TEXT, OPT_CENTERY, b);
        color(can ? C_ACCENT : C_GREY);
        text(880, (int16_t)(y + ROW_H / 2), F_SMALL, OPT_CENTERY | OPT_RIGHTX, can ? "Change >" : (d->flags & PF_RO) ? "read only" : (d->flags & PF_DEV) ? "developer" : lvl_name(d->level));
        eve_tag(E, 0);
        if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
    }
    char pg[32];
    snprintf(pg, sizeof pg, "%u-%u of %u", (unsigned)(U.first + 1), (unsigned)(U.first + ROWS < n ? U.first + ROWS : n), (unsigned)n);
    button(T_PGUP, 908, BODY_Y, 100, 170, "Up", C_ACCENT, U.first > 0);
    color(C_MUTE); text(958, BODY_Y + 196, F_SMALL, OPT_CENTER, pg);
    button(T_PGDN, 908, BODY_Y + 222, 100, 170, "Down", C_ACCENT, U.first + ROWS < n);
}

static void draw_keyboard(int16_t y, bool full) {
    eve_fgcolor(E, C_ACCENT);
    eve_bgcolor(E, 0xE4E7ECu);
    color(0xFFFFFFu);
    uint16_t o = U.pressed >= 32 && U.pressed < 127 ? U.pressed : 0;
    if (full) {
        const char *rows[] = { "1234567890", U.shift ? "QWERTYUIOP" : "qwertyuiop", U.shift ? "ASDFGHJKL" : "asdfghjkl",
                               U.shift ? "ZXCVBNM-._" : "zxcvbnm-._" };
        for (int r = 0; r < 4; ++r) ekeys(16, (int16_t)(y + r * 61), 760, 58, F_TEXT, (uint16_t)(OPT_FLAT | o), rows[r]);
        button(T_SHIFT, 784, y, 224, 58, U.shift ? "abc" : "ABC", C_ACCENT, true);
        button(T_SPACE, 784, (int16_t)(y + 61), 224, 58, "Space", C_ACCENT, true);
        button(T_BKSP, 784, (int16_t)(y + 122), 224, 58, "Delete", C_ACCENT, true);
        button(T_CLEAR, 784, (int16_t)(y + 183), 224, 58, "Clear", C_ACCENT, true);
    } else {
        const char *rows[] = { "789", "456", "123", U.kb == KB_IP ? "0." : "-0." };
        for (int r = 0; r < 4; ++r) ekeys(16, (int16_t)(y + r * 61), 420, 58, F_BIG, (uint16_t)(OPT_FLAT | o), rows[r]);
        button(T_BKSP, 444, y, 200, 58, "Delete", C_ACCENT, true);
        button(T_CLEAR, 444, (int16_t)(y + 61), 200, 58, "Clear", C_ACCENT, true);
    }
}

static void draw_cell_edit(void);
static void draw_edit(void) {
    if (U.cell_f) { draw_cell_edit(); return; }
    const param_def_t *d = U.ed;
    if (!d) return;
    char b[96], r[64];
    box(16, BODY_Y, 992, 88, C_CARD, 8);
    color(C_INK);
    snprintf(b, sizeof b, "%s  (%u)", d->name, d->id);
    text(32, BODY_Y + 10, F_BIG, 0, b);
    fmt_value(d, r, sizeof r);
    snprintf(b, sizeof b, "Now: %s", r);
    color(C_MUTE);
    text(992, BODY_Y + 16, F_TEXT, OPT_RIGHTX, b);
    if (d->ptype == PT_INT) snprintf(r, sizeof r, "Range %ld .. %ld", (long)d->vmin.i, (long)d->vmax.i);
    else if (d->ptype == PT_UINT && !is_ip(d)) snprintf(r, sizeof r, "Range %lu .. %lu", (unsigned long)d->vmin.u, (unsigned long)d->vmax.u);
    else if (d->ptype == PT_FLOAT) { char a[24], z[24]; fmt_float(a, sizeof a, (double)d->vmin.f); fmt_float(z, sizeof z, (double)d->vmax.f); snprintf(r, sizeof r, "Range %s .. %s", a, z); }
    else if (PARAM_IS_BLOB(d)) snprintf(r, sizeof r, "Up to %u characters", (unsigned)(d->ptype == PT_TEXT16 ? d->size / 2u - 1u : d->size - 1u));
    else r[0] = 0;
    if (d->unit && r[0]) { strncat(r, " ", sizeof r - strlen(r) - 1); strncat(r, d->unit, sizeof r - strlen(r) - 1); }
    text(32, BODY_Y + 56, F_TEXT, 0, r);
    if (U.kb == KB_BOOL) {
        button(T_ON, 120, BODY_Y + 120, 300, 120, "ON", C_OK, true);
        button(T_OFF, 600, BODY_Y + 120, 300, 120, "OFF", 0x5B6577u, true);
        button(T_CANCEL, 360, BODY_Y + 280, 300, 70, "Cancel", 0x5B6577u, true);
        return;
    }
    int16_t y = BODY_Y + 96;
    box(16, y, U.kb == KB_TEXT ? 760 : 628, 48, C_CARD, 6);
    color(C_INK);
    snprintf(b, sizeof b, "%s_", U.entry);
    text(28, (int16_t)(y + 24), F_BIG, OPT_CENTERY, b);
    draw_keyboard((int16_t)(y + 54), U.kb == KB_TEXT);
    int16_t bx = U.kb == KB_TEXT ? 784 : 664;
    if (U.kb == KB_TEXT) { button(T_OK, bx, y, 108, 48, "OK", C_OK, true); button(T_CANCEL, bx + 116, y, 108, 48, "Cancel", 0x5B6577u, true); }
    else { button(T_OK, bx, (int16_t)(y + 54), 344, 119, "OK", C_OK, true); button(T_CANCEL, bx, (int16_t)(y + 176), 344, 119, "Cancel", 0x5B6577u, true); }
}

static void draw_login(uint32_t now) {
    box(16, BODY_Y, 992, 128, C_CARD, 8);
    color(C_INK);
    text(32, BODY_Y + 12, F_BIG, 0, "Log in");
    static const char *const lv[] = { "Operator", "Admin", "Factory" };
    for (int k = 0; k < 3; ++k)
        button((uint8_t)(T_LVL1 + k), (int16_t)(220 + k * 180), BODY_Y + 8, 170, 60, lv[k], U.login_level == k + 1 ? C_HEAD : C_GREY, true);
    char m[40]; size_t n = strlen(U.pw), k = 0;
    for (; k < n && k < 32; ++k) m[k] = '*';
    m[k++] = '_'; m[k] = 0;
    box(32, BODY_Y + 76, 540, 40, 0xF4F5F7u, 6);
    color(C_INK);
    text(44, BODY_Y + 96, F_BIG, OPT_CENTERY, m);
    if (now < U.lock_until) { color(C_BAD); text(584, BODY_Y + 96, F_SMALL, OPT_CENTERY, "Too many attempts: wait"); }
    /* full card height: 40 px (6 mm on the RVT70) was the smallest target on any screen */
    button(T_OK, 776, BODY_Y + 8, 112, 112, "Enter", C_OK, now >= U.lock_until);
    button(T_CANCEL, 896, BODY_Y + 8, 104, 112, "Cancel", 0x5B6577u, true);
    draw_keyboard(BODY_Y + 136, true);
}

static void events_poll(uint32_t now) {
    int32_t got;
    if (U.ev_busy && plat_log_read_done(&got)) {
        U.ev_busy = false;
        U.ev_n = got > 0 ? (unsigned)got : 0;
        U.dirty = true;
    }
    if (U.scr != UI_EVENTS || U.ev_busy) return;
    if (!U.ev_want && now - U.ev_t < 3000u) return;
    log_ring_info_t inf;
    if (!plat_log_info(LOG_RING_EVENTS, &inf)) { U.ev_n = 0; U.ev_want = false; return; }
    if (U.ev_off >= inf.used) U.ev_off = inf.used > ROWS ? (inf.used - 1u) / ROWS * ROWS : 0;
    uint32_t avail = inf.used - U.ev_off, n = avail < ROWS ? avail : ROWS;
    if (n == 0) { U.ev_n = 0; U.ev_want = false; U.dirty = true; return; }
    uint32_t pos = (inf.wr + inf.nslots - U.ev_off - n) % inf.nslots;
    if (plat_log_read_start(LOG_RING_EVENTS, pos, n, U.ev_buf) == 0) { U.ev_busy = true; U.ev_want = false; U.ev_t = now; }
}

static void draw_events(void) {
    box(16, BODY_Y, 880, ROWS * ROW_H + 8, C_CARD, 8);
    int r = 0;
    for (unsigned k = U.ev_n; k-- > 0 && r < ROWS; ) {               /* newest first */
        log_event_t e;
        if (!plat_log_decode_event(U.ev_buf + k * 64u, &e)) continue;
        int16_t y = (int16_t)(BODY_Y + 4 + r * ROW_H);
        char t[32], what[32], det[64], who[40];
        log_fmt_time(t, sizeof t, e.time, e.clock_set);
        log_event_text(&e, what, sizeof what, det, sizeof det);
        snprintf(who, sizeof who, "%s%s%s", log_src_name(e.src), e.level ? " / " : "", log_level_name(e.level));
        color(C_MUTE); text(28, (int16_t)(y + 18), F_SMALL, OPT_CENTERY, t);
        text(28, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, who);
        color(C_INK); text(300, (int16_t)(y + 18), F_TEXT, OPT_CENTERY, what);
        text(300, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, det);
        if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
        r++;
    }
    if (!r) { color(C_MUTE); text(456, BODY_Y + 200, F_TEXT, OPT_CENTER, U.ev_busy ? "Loading..." : "No events"); }
    button(T_PGUP, 908, BODY_Y, 100, 170, "Newer", C_ACCENT, U.ev_off > 0);
    button(T_PGDN, 908, BODY_Y + 222, 100, 170, "Older", C_ACCENT, U.ev_n == ROWS);
}

__attribute__((weak)) bool ui_net_status(uint32_t *ip, bool *link_up, uint8_t mac[6]) {
    (void)ip; (void)link_up; (void)mac;
    return false;
}

static void kv(int16_t x, int16_t y, const char *k, const char *v) {
    color(C_MUTE); text(x, y, F_SMALL, 0, k);
    color(C_INK);  text((int16_t)(x + 170), y, F_SMALL, 0, v);
}

static void draw_system(uint32_t now) {
    char b[96], c[48];
    box(16, BODY_Y, 496, 392, C_CARD, 8);
    box(528, BODY_Y, 480, 390, C_CARD, 8);
    /* firmware */
    fw_status_t st; fw_status(&st);
    color(C_INK); text(32, BODY_Y + 10, F_TEXT, 0, "Firmware");
    kv(32, BODY_Y + 44, "Running", st.running_version);
    if (st.state == FW_ST_TRIAL) { snprintf(b, sizeof b, "on trial, keeps itself in %u s", (unsigned)((st.trial_left_ms + 999u) / 1000u)); kv(32, BODY_Y + 72, "State", b); }
    else {
        fw_hist_t h;
        if (fw_history(&h, 1) && h.kind == FWH_UPDATED) {        /* when the running firmware was put in */
            char t[24];
            bool known = (h.flags & FWH_INST_CLOCK) || (h.flags & FWH_DONE_CLOCK);
            if (known) log_fmt_time(t, sizeof t, (h.flags & FWH_INST_CLOCK) ? h.t_installed : h.t_done, true);
            snprintf(b, sizeof b, "%.16s, was %.11s", known ? t : "clock not set", h.from);
            kv(32, BODY_Y + 72, "Updated", b);
        } else kv(32, BODY_Y + 72, "State", st.rolled_back || (fw_history(&h, 1) && h.kind == FWH_ROLLED_BACK) ? "last update rolled back" : "normal");
    }
    kv(32, BODY_Y + 100, "Spare bank", st.staged ? st.staged_version : "empty");
    if (st.uploading) {
        snprintf(b, sizeof b, "receiving %lu of %lu bytes", (unsigned long)st.upload_bytes, (unsigned long)st.upload_total);
        kv(32, BODY_Y + 128, "Update", b);
        eve_fgcolor(E, C_ACCENT); eve_bgcolor(E, C_LINE);
        eve_progress(E, 32, BODY_Y + 158, 460, 16, 0, (uint16_t)(st.upload_total ? (uint64_t)st.upload_bytes * 1000u / st.upload_total : 0), 1000);
    } else if (st.last_error != FW_OK) {
        kv(32, BODY_Y + 128, "Last upload", fw_err_name(st.last_error));
    }
    if (!st.uploading && !st.signing_required) {
        color(C_WARN); text(32, BODY_Y + 156, F_SMALL, 0, "Development build: unsigned firmware is accepted");
    }
    if (st.staged && st.state != FW_ST_TRIAL) {
        snprintf(b, sizeof b, "Restart on %s", st.staged_version);
        button(T_FW_ACT, 32, BODY_Y + 184, 460, 64, b, C_WARN, U.level >= PLAT_LVL_FACTORY);
    }
    /* brightness */
    color(C_INK); text(32, BODY_Y + 266, F_TEXT, 0, "Screen brightness");
    snprintf(b, sizeof b, "%u %%", U.bright);
    button(T_BRIGHT_DN, 32, BODY_Y + 306, 140, 64, "-", C_ACCENT, true);
    color(C_INK); text(262, BODY_Y + 338, F_BIG, OPT_CENTER, b);
    button(T_BRIGHT_UP, 352, BODY_Y + 306, 140, 64, "+", C_ACCENT, true);
    /* HTTPS fingerprint: a technician compares it with the browser's to know it reached this unit */
    const char *fp = tls_fingerprint();
    color(C_MUTE); text(32, BODY_Y + 378, F_SMALL, 0, fp[0] ? "HTTPS certificate fingerprint (SHA-256)" : "HTTPS not available");
    if (fp[0]) {
        char l1[49], l2[48];
        memcpy(l1, fp, 47); l1[47] = 0;                     /* 16 bytes per line */
        memcpy(l2, fp + 48, 47); l2[47] = 0;
        color(C_INK); text(32, BODY_Y + 400, F_SMALL, 0, l1); text(32, BODY_Y + 420, F_SMALL, 0, l2);
    }
    /* unit */
    color(C_INK); text(544, BODY_Y + 10, F_TEXT, 0, "Unit");
    plat_val_t v;
    snprintf(b, sizeof b, "%lu", (unsigned long)(param_read(PARAM_ID_SERIAL, &v) ? v.u : 0)); kv(544, BODY_Y + 44, "Serial", b);
    snprintf(b, sizeof b, "%lu", (unsigned long)(param_read(PARAM_ID_HW_REV, &v) ? v.u : 0)); kv(544, BODY_Y + 70, "Hardware", b);
    snprintf(b, sizeof b, "%lu h %lu min", (unsigned long)(now / 3600000u), (unsigned long)((now / 60000u) % 60u)); kv(544, BODY_Y + 96, "Up", b);
    snprintf(b, sizeof b, "%lu%s%.15s", (unsigned long)plat_diag.boots, plat_diag.fault ? ", last fault: " : "", plat_diag.fault ? plat_diag.fault_name : "");
    kv(544, BODY_Y + 122, "Restarts", b);
    color(C_INK); text(544, BODY_Y + 156, F_TEXT, 0, "Network");
    uint32_t ip = 0; bool link = false; uint8_t mac[6] = { 0 };
    bool have = ui_net_status(&ip, &link, mac);
    if (!have && param_read(PARAM_ID_NET_IP, &v)) ip = v.u;
    fmt_ip(c, sizeof c, ip);
    if (plat_net_status_word & NETST_CONFLICT) snprintf(b, sizeof b, "%s IN USE by another device", c);
    else if (plat_net_status_word & NETST_PROBING) snprintf(b, sizeof b, "%s (checking...)", c);
    else snprintf(b, sizeof b, "%s%s", c, have ? (link ? "  (link up)" : "  (no link)") : "");
    kv(544, BODY_Y + 190, "Address", b);
    if (have) { snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); kv(544, BODY_Y + 216, "MAC", b); }
    color(C_INK); text(544, BODY_Y + 252, F_TEXT, 0, "Storage");
    kv(544, BODY_Y + 286, "Calibration", param_store_calibration_valid() ? "valid" : "INVALID");
    uint32_t ls = plat_log_status();
    snprintf(b, sizeof b, "%s%s%s", ls & LSTAT_NOR_FAULT ? "FAULT " : "", ls & LSTAT_VOLATILE ? "RAM only (no flash) " : "",
             (ls & (LSTAT_NOR_FAULT | LSTAT_VOLATILE)) ? "" : "ok");
    kv(544, BODY_Y + 312, "Log", b);
    snprintf(b, sizeof b, "0x%04lx", (unsigned long)param_store_status());
    kv(544, BODY_Y + 338, "Store status", b);
    if (plat_health.valid) {
        unsigned lo = 0xFFFFu; const char *ln = "";
        for (unsigned i = 0; i < plat_health.n_tasks && i < PLAT_HEALTH_TASKS; ++i)
            if (plat_health.task[i].free_words < lo) { lo = plat_health.task[i].free_words; ln = plat_health.task[i].name; }
        snprintf(b, sizeof b, "heap %lu KB free at worst, stack %.11s %u words", (unsigned long)(plat_health.heap_min / 1024u), ln, lo);
        kv(544, BODY_Y + 390, "Memory", b);
    }
    uint32_t ct;
    if (plat_clock_now(&ct)) log_fmt_time(b, sizeof b, ct, true); else snprintf(b, sizeof b, "NOT SET");
    kv(544, BODY_Y + 364, "Clock", b);
    button(T_CLOCK, 868, BODY_Y + 318, 128, 64, "Set clock", C_ACCENT, true);
    button(T_IOB, 868, BODY_Y + 244, 128, 64, "I/O boards", C_ACCENT, true);
}

/* I/O boards (iobus.h): one card per board, outputs and inputs as lit squares, faults in words */
static void draw_iob(void) {
    char b[64], c[20];
    for (unsigned k = 0; k < IOB_BOARDS; ++k) {
        int16_t x = (int16_t)(16 + k * 334), y = BODY_Y;
        const iob_board_tags_t *t = &iob_tags[k];
        iob_view_t v; memset(&v, 0, sizeof v); (void)iob_board_view(k, &v);
        box(x, y, 324, 438, C_CARD, 8);
        snprintf(b, sizeof b, "Board %u", k + 1u);
        color(C_INK); text((int16_t)(x + 14), (int16_t)(y + 10), F_TEXT, 0, b);
        uint32_t sc = !t->enable ? C_GREY : v.state == IOB_S_RUN ? C_OK : v.state == IOB_S_CONNECTING ? C_WARN : C_BAD;
        color(sc); text((int16_t)(x + 310), (int16_t)(y + 14), F_SMALL, OPT_RIGHTX, t->enable ? iob_state_name(v.state) : "not fitted");
        if (!t->enable) {
            color(C_MUTE);
            text((int16_t)(x + 14), (int16_t)(y + 50), F_SMALL, 0, "Enable it in Settings:");
            snprintf(b, sizeof b, "iob%u_enable (%u)", k + 1u, (unsigned)(PARAM_ID_IOB_FIRST + 10u * k));
            text((int16_t)(x + 14), (int16_t)(y + 72), F_SMALL, 0, b);
            continue;
        }
        /* how the board is reached: its IP and Modbus address on any IP link (D27), the address on RS-485 */
        bool by_ip = iob_link != IOB_LINK_RS485;
        if (by_ip) {
            if (t->ip) { fmt_ip(c, sizeof c, t->ip); snprintf(b, sizeof b, "%s, address %u", c, t->address); }
            else snprintf(b, sizeof b, "No IP set (iob%u_ip)", k + 1u);
        } else snprintf(b, sizeof b, "RS-485, address %u", t->address);
        color(t->ip || !by_ip ? C_MUTE : C_BAD); text((int16_t)(x + 14), (int16_t)(y + 42), F_SMALL, 0, b);
        /* outputs: bank A, bank B and the relays, one row each */
        snprintf(b, sizeof b, "Outputs %u-%u", t->first_do, t->first_do + IOB_OUTS - 1u);
        color(C_INK); text((int16_t)(x + 14), (int16_t)(y + 66), F_SMALL, 0, b);
        static const char *const bank[] = { "0.6 A", "1 A", "relay" };
        color(C_MUTE);
        for (unsigned r = 0; r < 3; ++r) text((int16_t)(x + 310), (int16_t)(y + 103 + r * 34), F_SMALL, OPT_CENTERY | OPT_RIGHTX, bank[r]);
        for (unsigned i = 0; i < IOB_OUTS; ++i) {
            int16_t bx = (int16_t)(x + 14 + (i % 8) * 31), by = (int16_t)(y + 89 + (i / 8) * 34);
            bool on = (v.outputs >> i) & 1u, f = (v.out_faults >> i) & 1u;
            box(bx, by, 28, 28, f ? C_BAD : on ? C_OK : C_LINE, 4);
            snprintf(b, sizeof b, "%u", i + 1u);
            color(f || on ? 0xFFFFFFu : C_INK); text((int16_t)(bx + 14), (int16_t)(by + 14), F_SMALL, OPT_CENTER, b);
        }
        snprintf(b, sizeof b, "Inputs %u-%u", t->first_di, t->first_di + IOB_INS - 1u);
        color(C_INK); text((int16_t)(x + 14), (int16_t)(y + 196), F_SMALL, 0, b);
        for (unsigned i = 0; i < IOB_INS; ++i) {
            int16_t bx = (int16_t)(x + 14 + i * 31), by = (int16_t)(y + 219);
            bool on = (v.inputs >> i) & 1u, f = (v.in_faults >> i) & 1u;
            box(bx, by, 28, 28, f ? C_BAD : on ? C_ACCENT : C_LINE, 4);
            snprintf(b, sizeof b, "%u", i + 1u);
            color(f || on ? 0xFFFFFFu : C_INK); text((int16_t)(bx + 14), (int16_t)(by + 14), F_SMALL, OPT_CENTER, b);
        }
        /* map 2: the configured temperature channels (calibrated, as the web page shows them), two
         * columns, and the pump */
        int16_t ly = (int16_t)(y + 258);
        unsigned nt = 0;
        for (unsigned i = 0; i < IOB_TEMPS; ++i) {
            if (t->temp_state[i] == IOB_TS_OFF) continue;
            bool ok = t->temp_state[i] == IOB_TS_OK && t->temps[i] != IOB_TEMP_INVALID;
            bool wait = t->temp_state[i] == IOB_TS_WAIT;
            int32_t d = t->temps[i];                         /* 0.1 degC, without float printf */
            if (ok) snprintf(b, sizeof b, "T%u %s%ld.%ld \u00b0C", i + 1u, d < 0 ? "-" : "", (long)(labs(d) / 10), (long)(labs(d) % 10));
            else snprintf(b, sizeof b, "T%u %s", i + 1u, wait ? "starting" : "fault");   /* why: in the fault lines */
            color(ok ? C_INK : wait ? C_MUTE : C_BAD);
            text((int16_t)(x + 14 + (nt % 2u) * 155), (int16_t)(ly + (nt / 2u) * 18), F_SMALL, 0, b);
            ++nt;
        }
        ly = (int16_t)(ly + ((nt + 1u) / 2u) * 18);
        if (v.n_pwm) {
            snprintf(b, sizeof b, "Pump %u.%u %%, %u rpm", v.pwm_applied / 10u, v.pwm_applied % 10u, (unsigned)v.rpm);
            color(C_INK); text((int16_t)(x + 14), ly, F_SMALL, 0, b);
            ly = (int16_t)(ly + 18);
        }
        if (nt || v.n_pwm) ly = (int16_t)(ly + 6);
        char lines[4][IOB_LINE];
        int room = (y + 398 - ly) / 18;
        int n = iob_fault_lines(&v, t->ol_mask, t->wb_mask, lines, room < 4 ? (room < 1 ? 1 : room) : 4);
        color(n ? C_BAD : C_OK);
        if (!n) text((int16_t)(x + 14), ly, F_SMALL, 0, v.state == IOB_S_RUN ? "No faults" : "");
        for (int i = 0; i < n; ++i) text((int16_t)(x + 14), (int16_t)(ly + i * 18), F_SMALL, 0, lines[i]);
        color(C_MUTE);
        snprintf(b, sizeof b, "Firmware %u.%u, hardware %u", (unsigned)(v.fw_version >> 8), (unsigned)(v.fw_version & 255u), (unsigned)v.hw_rev);
        if (v.fw_version) text((int16_t)(x + 14), (int16_t)(y + 398), F_SMALL, 0, b);
        snprintf(b, sizeof b, "Requests %lu, failed %lu", (unsigned long)v.requests, (unsigned long)v.errors);
        text((int16_t)(x + 14), (int16_t)(y + 418), F_SMALL, 0, b);
    }
}

/* set the clock: digits in the order year, month, day, hour, minute, second */
static void draw_clock(void) {
    box(16, BODY_Y, 992, 70, C_CARD, 8);
    color(C_INK); text(32, BODY_Y + 10, F_TEXT, 0, "Set the clock (local standard time)");
    char shown[24] = "____-__-__ __:__:__";
    static const uint8_t pos[14] = { 0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18 };
    for (size_t k = 0; k < strlen(U.entry) && k < 14; ++k) shown[pos[k]] = U.entry[k];
    color(C_ACCENT); text(560, BODY_Y + 36, F_BIG, OPT_CENTERY, shown);
    color(C_MUTE); text(32, BODY_Y + 44, F_SMALL, 0, "Type year, month, day, hour, minute, second");
    int16_t y = BODY_Y + 84;
    draw_keyboard(y, false);
    button(T_OK, 664, y, 344, 119, "OK", C_OK, true);
    button(T_CANCEL, 664, (int16_t)(y + 122), 344, 119, "Cancel", 0x5B6577u, true);
}

static uint32_t sev_colour(uint8_t s) {
    return s >= AL_SEV_ALARM ? C_BAD : s == AL_SEV_WARNING ? C_WARN : C_GREY;
}

static void draw_alarms(void) {
    alarm_view_t v[ROWS];
    alarm_summary_t as;
    alarm_summary(&as);
    if (U.al_first >= as.shown) U.al_first = as.shown > ROWS ? (as.shown - 1u) / ROWS * ROWS : 0;
    size_t n = alarm_list(v, U.al_first, ROWS);
    box(16, BODY_Y, 880, ROWS * ROW_H + 8, C_CARD, 8);
    memset(U.al_ids, 0, sizeof U.al_ids);
    for (size_t r = 0; r < n; ++r) {
        const alarm_view_t *a = &v[r];
        int16_t y = (int16_t)(BODY_Y + 4 + (int)r * ROW_H);
        if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
        box(24, (int16_t)(y + 8), 8, ROW_H - 16, sev_colour(a->severity), 2);
        char t[48], line[112];
        if (a->t_raise) {
            unsigned yy, mo, d, h, mi, s;
            plat_civil_from_unix(a->t_raise, &yy, &mo, &d, &h, &mi, &s);
            snprintf(t, sizeof t, "%02u/%02u %02u:%02u:%02u", d, mo, h, mi, s);
        } else {
            snprintf(t, sizeof t, "uptime %lu min", (unsigned long)(a->up_raise / 60u));
        }
        snprintf(line, sizeof line, "%s  -  %s%s", t,
                 a->active ? (a->unacked ? "active, not acknowledged" : "active") : "cleared, not acknowledged",
                 a->active && a->unacked && (a->flags & AL_LATCH) ? " (latched)" : "");
        color(C_INK); text(44, (int16_t)(y + 18), F_TEXT, OPT_CENTERY, a->text);
        color(a->unacked ? sev_colour(a->severity) : C_MUTE); text(44, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, line);
        if (a->unacked) {
            U.al_ids[r] = a->id;
            button((uint8_t)(T_ROW0 + r), 724, (int16_t)(y + 4), 164, ROW_H - 8, "Acknowledge", C_ACCENT, true);
        }
    }
    if (!n) { color(C_MUTE); text(456, BODY_Y + 200, F_TEXT, OPT_CENTER, "No alarms"); }
    button(T_PGUP, 908, BODY_Y, 100, 96, "Up", C_ACCENT, U.al_first > 0);
    button(T_ACK_ALL, 908, BODY_Y + 104, 100, 96, "Ack all", C_WARN, as.unacked > 0);
    button(T_PGDN, 908, BODY_Y + 208, 100, 96, "Down", C_ACCENT, U.al_first + ROWS < as.shown);
    button(T_ACTIONS, 908, BODY_Y + 312, 100, 80, "Actions", 0x5B6577u, true);
}

/* what each alarm does: email, Modbus, relays 1-4 (admin) */
static void draw_alarm_cfg(void) {
    static const char *const lab[6] = { "Email", "Modbus", "R1", "R2", "R3", "R4" };
    static const uint8_t bit[6] = { AR_EMAIL, AR_MODBUS, AR_RELAY(0), AR_RELAY(1), AR_RELAY(2), AR_RELAY(3) };
    size_t n = alarm_count();
    box(16, BODY_Y, 880, ROWS * ROW_H + 8, C_CARD, 8);
    bool can = U.level >= PLAT_LVL_ADMIN;
    for (int r = 0; r < ROWS; ++r) {
        const alarm_def_t *d = alarm_def_at(U.cfg_first + (size_t)r);
        if (!d) break;
        int16_t y = (int16_t)(BODY_Y + 4 + r * ROW_H);
        if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
        alarm_view_t v;
        char d7[64]; size_t k = 0;
        if (alarm_get(d->id, &v))
            for (const unsigned char *p = (const unsigned char *)v.text; *p && k + 1 < sizeof d7; ++p) {
                d7[k++] = (char)*p;                    /* UTF-8 kept: text() shows what the font has */
            }
        d7[k] = 0;
        if (k > 42) {                                  /* the column holds ~42 characters: cut at a word */
            size_t c = 39;
            while (c > 20 && d7[c] != ' ') c--;
            strcpy(d7 + c, "...");
        }
        box(24, (int16_t)(y + 8), 8, ROW_H - 16, sev_colour(d->severity), 2);
        color(C_INK); text(40, (int16_t)(y + 20), F_SMALL, OPT_CENTERY, d7);
        color(C_MUTE); text(40, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, d->name);
        uint8_t rt = alarm_route(d->id);
        for (int b = 0; b < 6; ++b) {
            bool on = (rt & bit[b]) != 0;
            int16_t x = (int16_t)(440 + b * 75);
            uint8_t tg = (uint8_t)(T_CFG0 + r * 6 + b);
            eve_tag(E, can ? tg : 0);              /* the state shows either way; only admin can change it */
            eve_fgcolor(E, U.pressed == tg ? C_PRESS : on ? C_OK : 0xB8C0CCu);
            color(0xFFFFFFu);
            ebutton(x, (int16_t)(y + 6), 70, ROW_H - 12, F_SMALL, OPT_FLAT, lab[b]);
            eve_tag(E, 0);
        }
    }
    button(T_PGUP, 908, BODY_Y, 100, 120, "Up", C_ACCENT, U.cfg_first > 0);
    button(T_BACK, 908, BODY_Y + 132, 100, 120, "Back", 0x5B6577u, true);
    button(T_PGDN, 908, BODY_Y + 264, 100, 128, "Down", C_ACCENT, U.cfg_first + ROWS < n);
    if (!can) { color(C_MUTE); text(456, TOAST_Y + 22, F_SMALL, OPT_CENTER, "Green = on. Log in as admin to change."); }
}


/* ================= application screens and record editors (ui_app.h, D26) ================= */
#define APP_Y   (BODY_Y + 54)          /* below the tabs */
#define APP_BOT (TOAST_Y - 4)
#define APP_R   896                    /* right edge of the widgets; the pager is beyond */

static bool is_arr(uint16_t id) { const param_def_t *d = id ? param_find(id) : NULL; return d && d->ptype == PT_ARRAY; }
static int64_t pv(uint16_t id, uint16_t idx, int64_t dflt) { int64_t v; return id && ui_param_value(id, idx, &v) ? v : dflt; }
static const ui_screen_t *cur_screen(void) {
    const ui_app_t *a = ui_app();
    return a && U.app_scr >= 0 && (size_t)U.app_scr < a->n_screens ? &a->screens[U.app_scr] : NULL;
}
static bool w_visible(const ui_widget_t *w) {
    if (!w->show_id || (w->kind == UW_GRID && is_arr(w->show_id))) return true;
    return pv(w->show_id, UW_NO_INDEX, 0) >= (w->show_min ? w->show_min : 1);
}
static unsigned grid_count(const ui_widget_t *w) {
    int64_t n = 0;
    if (w->count_id) n = pv(w->count_id, UW_NO_INDEX, 0);
    else if (is_arr(w->show_id)) { const param_def_t *d = param_find(w->show_id); n = (int64_t)(d->size / param_ctype_size(d->ctype)); }
    return n < 0 ? 0u : n > 32 ? 32u : (unsigned)n;
}
static bool item_visible(const ui_widget_t *w, unsigned i) { return !is_arr(w->show_id) || pv(w->show_id, (uint16_t)(i - 1u), 0) != 0; }
static bool item_enabled(const ui_widget_t *w, unsigned i) {
    if (!w->enable_id) return true;
    return pv(w->enable_id, is_arr(w->enable_id) ? (uint16_t)(i - 1u) : UW_NO_INDEX, 0) != 0;
}
static bool item_lit(const ui_widget_t *w, unsigned i) {
    if (!w->state_id) return false;
    if (is_arr(w->state_id)) return pv(w->state_id, (uint16_t)(i - 1u), 0) != 0;
    return pv(w->state_id, UW_NO_INDEX, 0) == (int64_t)i;
}
static void item_label(const ui_widget_t *w, unsigned i, char *b, size_t cap) {
    const param_def_t *d = w->names_id ? param_find((uint16_t)(w->names_id + i - 1u)) : NULL;
    if (d && PARAM_IS_BLOB(d)) fmt_value(d, b, cap);
    if (!d || !b[0]) snprintf(b, cap, "%u", i);
}

/* one element of an array parameter, formatted */
static void fmt_elem(const param_def_t *d, uint16_t idx, char *b, size_t cap) {
    size_t es = param_ctype_size(d->ctype);
    uint8_t raw[4] = { 0 };
    if (d->ptype != PT_ARRAY || (size_t)(idx + 1u) * es > d->size || param_read_blob(d->id, (uint16_t)(idx * es), raw, (uint16_t)es) != PERR_OK) { snprintf(b, cap, "-"); return; }
    if (d->ctype == CT_F32) { float f; memcpy(&f, raw, 4); fmt_float(b, cap, (double)f); }
    else { int64_t v; ui_param_value(d->id, idx, &v); snprintf(b, cap, "%lld", (long long)v); }
    if (d->unit && strlen(b) + strlen(d->unit) + 2 < cap) { strcat(b, " "); strcat(b, d->unit); }
}

/* a command from a button, grid item or dialog: through the table like any write */
static void app_write(uint16_t id, int32_t value, const char *label, uint32_t now) {
    const param_def_t *d = param_find(id);
    if (!d) return;
    plat_cmd_t c = { .id = id, .src = PLAT_SRC_LOCAL, .level = U.level, .kind = val_kind(d) };
    switch (d->ptype) {
    case PT_BOOL: c.v.b = value != 0; break;
    case PT_INT: c.v.i = value; break;
    case PT_FLOAT: c.v.f = (float)value; break;
    default: c.v.u = (uint32_t)value; break;
    }
    param_err_t e = param_check(id, c.kind, c.v, PLAT_SRC_LOCAL, U.level);
    if (e != PERR_OK) { toast(true, now, "%s", perr_text(e)); return; }
    U.pend = c;
    U.act = ACT_POST;
    snprintf(U.ok_msg, sizeof U.ok_msg, "%s", label && label[0] ? label : "Done");
    if (d->flags & PF_CONFIRM) {
        char m[96];
        snprintf(m, sizeof m, "%s?", label && label[0] ? label : d->name);
        ask(ACT_POST, m);
    } else do_action(now);
}

static const ui_widget_t *app_dialog(void) {
    const ui_screen_t *s = cur_screen();
    for (unsigned i = 0; s && i < s->n; ++i) if (s->w[i].kind == UW_DIALOG && w_visible(&s->w[i])) return &s->w[i];
    return NULL;
}

static bool app_tap(uint8_t t, uint32_t now) {
    const ui_app_t *a = ui_app();
    const ui_screen_t *s = cur_screen();
    if (!a || !s) return false;
    const ui_widget_t *dlg = app_dialog();
    if (dlg) {                                    /* a question on the screen: only its buttons */
        if (t == T_DLG_YES) app_write(dlg->cmd_id, ui_cmd_value(dlg->cmd_value), dlg->cmd2_id ? "Yes" : "OK", now);
        else if (t == T_DLG_NO && dlg->cmd2_id) app_write(dlg->cmd2_id, 1, "No", now);
        else if (t >= T_NAV_HOME && t <= T_NAV_LOGIN) return false;   /* the bottom bar still works */
        U.dirty = true;
        return true;
    }
    unsigned ntab = a->n_screens > 7 ? 7u : (unsigned)a->n_screens;
    if (t >= T_TAB0 && t < T_TAB0 + ntab) { U.app_scr = t - T_TAB0; U.app_first = 0; U.app_nh = 0; U.dirty = true; return true; }
    if (t == T_TAB0 + ntab) { goto_scr(UI_HOME); return true; }
    if (t == T_PGDN && U.app_next < s->n) { if (U.app_nh < sizeof U.app_hist) U.app_hist[U.app_nh++] = U.app_first; U.app_first = U.app_next; U.dirty = true; return true; }
    if (t == T_PGUP && U.app_nh) { U.app_first = U.app_hist[--U.app_nh]; U.dirty = true; return true; }
    if (t < T_W0 || t > T_WMAX || !U.tmap_w[t]) return false;
    const ui_widget_t *w = &s->w[U.tmap_w[t] - 1u];
    switch (w->kind) {
    case UW_BUTTON: app_write(w->cmd_id, ui_cmd_value(w->cmd_value), w->label, now); break;
    case UW_GRID: {
        char b[32]; item_label(w, U.tmap_i[t], b, sizeof b);
        app_write(w->cmd_id, (int32_t)U.tmap_i[t], b, now);
        break;
    }
    case UW_RECORD: (void)rec_open(ui_app_record(w->record_id), now); break;
    case UW_VALUE: {
        const param_def_t *d = param_find(w->value_id);
        if (d && w->index == UW_NO_INDEX) edit_begin(d, now);
        break;
    }
    default: break;
    }
    return true;
}

static void draw_tabs(void) {
    const ui_app_t *a = ui_app();
    unsigned ntab = a->n_screens > 7 ? 7u : (unsigned)a->n_screens;
    int16_t tw = (int16_t)(992 / (ntab + 1u)) ;
    for (unsigned k = 0; k <= ntab; ++k) {
        bool cur = (k < ntab && U.scr == UI_APP && U.app_scr == (int)k) || (k == ntab && U.scr == UI_HOME);
        button((uint8_t)(T_TAB0 + k), (int16_t)(16 + k * tw), BODY_Y, (int16_t)(tw - 6), 44,
               k < ntab ? a->screens[k].title : "Status", cur ? C_HEAD : 0x5B6577u, true);
    }
}

static void draw_app(void) {
    const ui_screen_t *s = cur_screen();
    memset(U.tmap_w, 0, sizeof U.tmap_w);
    if (!s) { goto_scr(UI_HOME); return; }
    draw_tabs();
    int16_t x = 16, y = APP_Y, row_h = 0;
    unsigned tag = T_W0, i = U.app_first;
    bool any = false;
    for (; i < s->n; ++i) {
        const ui_widget_t *w = &s->w[i];
        if (w->kind == UW_DIALOG || !w_visible(w)) continue;
        if (w->kind == UW_SECTION || w->kind == UW_GRID) {             /* across the screen */
            if (x > 16) { y = (int16_t)(y + row_h + 8); x = 16; row_h = 0; }
        }
        if (w->kind == UW_SECTION) {
            if (y + 34 > APP_BOT) break;
            color(C_MUTE); text(20, (int16_t)(y + 17), F_TEXT, OPT_CENTERY, w->label ? w->label : "");
            color(C_LINE); eve_line(E, 20, (int16_t)(y + 33), APP_R, (int16_t)(y + 33), 1);
            y = (int16_t)(y + 40); any = true;
            continue;
        }
        if (w->kind == UW_GRID) {
            unsigned n = grid_count(w), vis = 0;
            for (unsigned k = 1; k <= n; ++k) vis += item_visible(w, k);
            int16_t lines = (int16_t)((vis + 7u) / 8u), h = (int16_t)(28 + (lines ? lines : 1) * 62);
            if ((y + h > APP_BOT && any) || tag + vis > T_WMAX) break;
            color(C_INK); text(20, (int16_t)(y + 12), F_SMALL, OPT_CENTERY, w->label ? w->label : "");
            unsigned c = 0;
            for (unsigned k = 1; k <= n; ++k) {
                if (!item_visible(w, k)) continue;
                int16_t ix = (int16_t)(16 + (c % 8u) * 110), iy = (int16_t)(y + 28 + (c / 8u) * 62);
                char b[24]; item_label(w, k, b, sizeof b);
                if (strlen(b) > 10) b[10] = 0;
                bool en = item_enabled(w, k);
                U.tmap_w[tag] = (uint8_t)(i + 1u); U.tmap_i[tag] = (uint16_t)k;
                eve_tag(E, en ? (uint8_t)tag : 0);
                eve_fgcolor(E, !en ? C_GREY : U.pressed == tag ? C_PRESS : item_lit(w, k) ? C_OK : 0x5B6577u);
                color(0xFFFFFFu);
                ebutton(ix, iy, 104, 56, F_SMALL, OPT_FLAT, b);
                eve_tag(E, 0);
                tag++; c++;
            }
            y = (int16_t)(y + h + 8); any = true;
            continue;
        }
        int16_t wdt = 212, h = w->kind == UW_VALUE ? 84 : 64;
        if (x + wdt > APP_R) { y = (int16_t)(y + row_h + 8); x = 16; row_h = 0; }
        if ((y + h > APP_BOT && any) || tag > T_WMAX) break;
        U.tmap_w[tag] = (uint8_t)(i + 1u); U.tmap_i[tag] = 0;
        if (w->kind == UW_VALUE) {
            const param_def_t *d = param_find(w->value_id);
            bool can = d && w->index == UW_NO_INDEX && why_not(d) == NULL;
            eve_tag(E, can ? (uint8_t)tag : 0);
            box(x, y, wdt, h, U.pressed == tag ? 0xDCE6F5u : C_CARD, 8);
            color(C_MUTE); text((int16_t)(x + 12), (int16_t)(y + 8), F_SMALL, 0, w->label ? w->label : d ? d->name : "?");
            char v[64] = "-";
            if (d) { if (w->index != UW_NO_INDEX) fmt_elem(d, w->index, v, sizeof v); else fmt_value(d, v, sizeof v); }
            if (strlen(v) > 14) strcpy(v + 11, "...");
            color(C_INK); text((int16_t)(x + 12), (int16_t)(y + 36), F_BIG, 0, v);
            if (can) { color(C_ACCENT); text((int16_t)(x + wdt - 10), (int16_t)(y + 8), F_SMALL, OPT_RIGHTX, ">"); }
            eve_tag(E, 0);
        } else if (w->kind == UW_RECORD) {
            button((uint8_t)tag, x, y, wdt, h, w->label ? w->label : "Edit", C_ACCENT, true);
        } else {                                                 /* UW_BUTTON */
            bool en = !w->enable_id || pv(w->enable_id, UW_NO_INDEX, 0) != 0;
            bool lit = w->state_id && pv(w->state_id, UW_NO_INDEX, 0) != 0;
            button((uint8_t)tag, x, y, wdt, h, w->label ? w->label : "?", lit ? C_OK : C_ACCENT, en);
        }
        tag++;
        x = (int16_t)(x + wdt + 8);
        if (h > row_h) row_h = h;
        any = true;
    }
    U.app_next = (uint8_t)i;
    if (U.app_nh || U.app_next < s->n) {
        button(T_PGUP, 908, APP_Y, 100, 160, "Up", C_ACCENT, U.app_nh > 0);
        button(T_PGDN, 908, (int16_t)(APP_Y + 176), 100, 160, "Down", C_ACCENT, U.app_next < s->n);
    }
}

static void draw_app_dialog(void) {
    const ui_widget_t *w = app_dialog();
    if (!w) return;
    snprintf(U.confirm_msg, sizeof U.confirm_msg, "%s", w->label ? w->label : "");
    color(0x000000u); eve_cmd(E, DL_COLOR_A(140));
    eve_rect(E, 0, 0, W, 600, 1);
    eve_cmd(E, DL_COLOR_A(255));
    box(162, 150, 700, 300, C_CARD, 12);
    color(C_INK);
    const char *s = U.confirm_msg;
    int16_t y = 180;
    while (*s && y < 330) {
        char line[48]; size_t n = strlen(s);
        if (n > 44) { n = 44; while (n > 20 && s[n] != ' ') n--; }
        memcpy(line, s, n); line[n] = 0;
        text(512, y, F_TEXT, OPT_CENTERX, line);
        s += n; while (*s == ' ') s++;
        y = (int16_t)(y + 36);
    }
    if (w->cmd2_id) {
        button(T_DLG_YES, 202, 350, 290, 70, "Yes", C_OK, true);
        button(T_DLG_NO, 532, 350, 290, 70, "No", 0x5B6577u, true);
    } else button(T_DLG_YES, 367, 350, 290, 70, "OK", C_OK, true);
}

/* ---------------- record editor ---------------- */
static const char *rec_why_not(void) {
    param_err_t e = U.rec ? param_record_check(U.rec->param_id, PLAT_SRC_LOCAL, U.level) : PERR_NO_ID;
    if (e == PERR_ACCESS) {
        static char m[48];
        const param_def_t *d = param_find(U.rec->param_id);
        snprintf(m, sizeof m, "Log in as %s to change this", lvl_name(d ? d->level : 1));
        return m;
    }
    return e == PERR_OK ? NULL : perr_text(e);
}

static bool rec_open(const ui_record_t *r, uint32_t now) {
    const param_def_t *d = r ? param_find(r->param_id) : NULL;
    if (!d || d->size > sizeof U.rec_buf || param_read_blob(d->id, 0, U.rec_buf, d->size) != PERR_OK) {
        toast(true, now, "Not available"); return false;
    }
    U.rec = r;
    U.rec_first = 0; U.rec_next = 0;
    U.rec_changed = false; U.rec_wait = false;
    U.cell_f = NULL;
    if (U.scr != UI_REC && U.scr != UI_EDIT) U.rec_back = U.scr;
    goto_scr(UI_REC);
    return true;
}

static void rec_leave(void) { U.rec = NULL; U.cell_f = NULL; goto_scr(U.rec_back == UI_REC || U.rec_back == UI_EDIT ? UI_HOME : U.rec_back); }

static void rec_poll(uint32_t now) {
    if (!U.rec_wait) return;
    param_err_t e;
    char why[80];
    if (param_record_result(U.rec_ticket, &e, why, sizeof why)) {
        U.rec_wait = false;
        if (e == PERR_OK) { toast(false, now, "%s saved", U.rec ? U.rec->title : "Record"); U.rec_changed = false; if (U.scr == UI_REC) rec_leave(); }
        else toast(true, now, "%s", why[0] ? why : perr_text(e));
        U.dirty = true;
    } else if (now - U.rec_wait_t > 3000u) {
        U.rec_wait = false;
        toast(true, now, "No answer from the instrument: not saved");
    }
}

static void fmt_cell(const ui_field_t *f, unsigned row, unsigned elem, char *b, size_t cap) {
    const uint8_t *p = U.rec_buf + ui_field_at(U.rec, f, row, elem);
    int64_t v = 0;
    if (f->kind == RF_TEXT) {
        size_t k = 0;
        for (; k < f->size && p[k] && k + 1 < cap; ++k) b[k] = p[k] >= 32 && p[k] < 127 ? (char)p[k] : '?';
        b[k] = 0;
        return;
    }
    if (!ui_field_get(f, U.rec_buf, row, elem, &v)) { snprintf(b, cap, "?"); return; }
    switch (f->kind) {
    case RF_BOOL: snprintf(b, cap, "%s", v ? "Yes" : "No"); break;
    case RF_MS_AS_S: { char t[24]; fmt_float(t, sizeof t, (double)v / 1000.0); snprintf(b, cap, "%s s", t); break; }
    case RF_HMS: snprintf(b, cap, "%02u:%02u:%02u", (unsigned)(v / 3600), (unsigned)(v / 60 % 60), (unsigned)(v % 60)); break;
    case RF_ENUM: if (v >= 0 && v < f->n_items) snprintf(b, cap, "%s", f->items[v]); else snprintf(b, cap, "%lld", (long long)v); break;
    default: snprintf(b, cap, "%lld%s%s", (long long)v, f->unit ? " " : "", f->unit ? f->unit : ""); break;
    }
}

static void rec_tap(uint8_t t, uint32_t now) {
    const ui_record_t *r = U.rec;
    if (!r) { goto_scr(UI_HOME); return; }
    const char *why = rec_why_not();
    unsigned used = ui_record_rows_used(r, U.rec_buf);
    if (t == T_CANCEL) { if (U.rec_changed) toast(false, now, "Not saved"); rec_leave(); return; }
    if (t == T_OK) {
        if (why) { toast(true, now, "%s", why); return; }
        const param_def_t *d = param_find(r->param_id);
        param_err_t e = param_record_post(r->param_id, U.rec_buf, d->size, PLAT_SRC_LOCAL, U.level, &U.rec_ticket);
        if (e != PERR_OK) { toast(true, now, "%s", perr_text(e)); return; }
        U.rec_wait = true; U.rec_wait_t = now;
        toast(false, now, "Saving...");
        return;
    }
    if (t == T_PGUP) { U.rec_first = U.rec_first >= (r->rows ? 5u : 1u) ? (uint16_t)(U.rec_first - (r->rows ? 5u : 1u)) : 0; U.dirty = true; return; }
    if (t == T_PGDN) { if (U.rec_next > U.rec_first) U.rec_first = U.rec_next; U.dirty = true; return; }
    if (why && (t == T_ON || t == T_OFF || (t >= T_W0 && t <= T_WMAX))) { toast(true, now, "%s", why); return; }
    if (r->rows && r->rows_off != UI_ROWS_ALL && (t == T_ON || t == T_OFF)) {
        if (t == T_ON && used < r->rows) used++;
        else if (t == T_OFF && used > 0) used--;
        ui_record_set_rows(r, U.rec_buf, used);
        U.rec_changed = true; U.dirty = true;
        return;
    }
    if (t < T_W0 || t > T_WMAX || !U.tmap_w[t]) return;
    const ui_field_t *f = &r->fields[U.tmap_w[t] - 1u];
    unsigned row = U.tmap_i[t] >> 6, elem = U.tmap_i[t] & 63u;
    int64_t v = 0;
    if (f->kind == RF_BOOL) { ui_field_get(f, U.rec_buf, row, elem, &v); ui_field_set(f, U.rec_buf, row, elem, !v); U.rec_changed = true; U.dirty = true; return; }
    if (f->kind == RF_ENUM) {
        ui_field_get(f, U.rec_buf, row, elem, &v);
        int64_t mx = ui_field_max(f);
        if (mx > f->min + (int64_t)f->n_items - 1) mx = f->min + (int64_t)f->n_items - 1;
        v = v >= mx ? f->min : v + 1;
        ui_field_set(f, U.rec_buf, row, elem, v);
        U.rec_changed = true; U.dirty = true;
        return;
    }
    U.cell_f = f; U.cell_row = row; U.cell_elem = elem;
    U.shift = false;
    U.kb = f->kind == RF_TEXT ? KB_TEXT : f->kind == RF_HMS ? KB_IP : KB_NUM;
    fmt_cell(f, row, elem, U.entry, sizeof U.entry);
    if (f->kind == RF_MS_AS_S) { char *sp = strchr(U.entry, ' '); if (sp) *sp = 0; }
    else if (f->kind == RF_HMS) { for (char *p = U.entry; *p; ++p) if (*p == ':') *p = '.'; }
    else if (f->kind != RF_TEXT) { char *sp = strchr(U.entry, ' '); if (sp) *sp = 0; }
    goto_scr(UI_EDIT);
}

static bool cell_submit(bool on, uint32_t now) {
    (void)on;
    const ui_field_t *f = U.cell_f;
    uint8_t *p = U.rec_buf + ui_field_at(U.rec, f, U.cell_row, U.cell_elem);
    char *end = NULL;
    int64_t v = 0;
    switch (f->kind) {
    case RF_TEXT: {
        size_t n = strlen(U.entry);
        if (n + 1u > f->size) { toast(true, now, "Too long (at most %u characters)", (unsigned)f->size - 1u); return false; }
        memset(p, 0, f->size); memcpy(p, U.entry, n);
        U.cell_f = NULL; U.rec_changed = true;
        return true;
    }
    case RF_MS_AS_S: {
        double s = strtod(U.entry, &end);
        if (!U.entry[0] || *end || !isfinite(s) || s < 0 || s > 4294967.0) { toast(true, now, "Not a valid number of seconds"); return false; }
        v = (int64_t)(s * 1000.0 + 0.5);
        break;
    }
    case RF_HMS: {
        unsigned h = 0, m = 0, s = 0;
        int n = sscanf(U.entry, "%u.%u.%u", &h, &m, &s);
        if (n < 2 || h > 23 || m > 59 || s > 59) { toast(true, now, "Enter hours.minutes.seconds, e.g. 7.30.00"); return false; }
        v = (int64_t)h * 3600 + m * 60 + s;
        break;
    }
    default:
        v = strtoll(U.entry, &end, 10);
        if (!U.entry[0] || *end) { toast(true, now, "Not a valid number"); return false; }
    }
    if (!ui_field_set(f, U.rec_buf, U.cell_row, U.cell_elem, v)) {
        if (f->kind == RF_MS_AS_S) toast(true, now, "Allowed %ld .. %ld s", (long)(f->min / 1000), (long)(ui_field_max(f) / 1000));
        else toast(true, now, "Allowed %ld .. %ld", (long)f->min, (long)ui_field_max(f));
        return false;
    }
    U.cell_f = NULL; U.rec_changed = true;
    return true;
}

static void draw_cell_edit(void) {
    const ui_field_t *f = U.cell_f;
    char b[96], r[64];
    box(16, BODY_Y, 992, 88, C_CARD, 8);
    color(C_INK);
    if (U.rec->rows) snprintf(b, sizeof b, "%s: %s, row %u", U.rec->title, f->label, U.cell_row + 1u);
    else if (f->count > 1) snprintf(b, sizeof b, "%s: %s %u", U.rec->title, f->label, U.cell_elem + 1u);
    else snprintf(b, sizeof b, "%s: %s", U.rec->title, f->label);
    text(32, BODY_Y + 10, F_BIG, 0, b);
    if (f->kind == RF_TEXT) snprintf(r, sizeof r, "Up to %u characters", (unsigned)f->size - 1u);
    else if (f->kind == RF_HMS) snprintf(r, sizeof r, "Hours.minutes.seconds, e.g. 7.30.00");
    else if (f->kind == RF_MS_AS_S) snprintf(r, sizeof r, "Seconds, %ld .. %ld", (long)(f->min / 1000), (long)(ui_field_max(f) / 1000));
    else snprintf(r, sizeof r, "Range %ld .. %ld%s%s", (long)f->min, (long)ui_field_max(f), f->unit ? " " : "", f->unit ? f->unit : "");
    color(C_MUTE); text(32, BODY_Y + 56, F_TEXT, 0, r);
    int16_t y = BODY_Y + 96;
    box(16, y, U.kb == KB_TEXT ? 760 : 628, 48, C_CARD, 6);
    color(C_INK);
    snprintf(b, sizeof b, "%s_", U.entry);
    text(28, (int16_t)(y + 24), F_BIG, OPT_CENTERY, b);
    draw_keyboard((int16_t)(y + 54), U.kb == KB_TEXT);
    int16_t bx = U.kb == KB_TEXT ? 784 : 664;
    if (U.kb == KB_TEXT) { button(T_OK, bx, y, 108, 48, "OK", C_OK, true); button(T_CANCEL, bx + 116, y, 108, 48, "Cancel", 0x5B6577u, true); }
    else { button(T_OK, bx, (int16_t)(y + 54), 344, 119, "OK", C_OK, true); button(T_CANCEL, bx, (int16_t)(y + 176), 344, 119, "Cancel", 0x5B6577u, true); }
}

static void draw_rec(void) {
    const ui_record_t *r = U.rec;
    memset(U.tmap_w, 0, sizeof U.tmap_w);
    if (!r) return;
    const char *why = rec_why_not();
    unsigned used = ui_record_rows_used(r, U.rec_buf);
    box(16, BODY_Y, 992, 56, C_CARD, 8);
    color(C_INK); text(32, BODY_Y + 28, F_BIG, OPT_CENTERY, r->title);
    if (r->rows && r->rows_off != UI_ROWS_ALL) {
        char b[32]; snprintf(b, sizeof b, "In use: %u", used);
        color(C_MUTE); text(330, BODY_Y + 28, F_TEXT, OPT_CENTERY, b);
        button(T_OFF, 470, BODY_Y + 4, 64, 48, "-", C_ACCENT, !why && used > 0);
        button(T_ON, 540, BODY_Y + 4, 64, 48, "+", C_ACCENT, !why && used < r->rows);
    }
    button(T_OK, 700, BODY_Y + 4, 150, 48, U.rec_wait ? "Saving..." : "Save", U.rec_changed ? C_OK : C_ACCENT, !why && !U.rec_wait);
    button(T_CANCEL, 858, BODY_Y + 4, 150, 48, U.rec_changed ? "Cancel" : "Close", 0x5B6577u, true);
    int16_t y = BODY_Y + 64;
    unsigned tag = T_W0;
    if (r->rows) {                                       /* table: 5 rows a page */
        int16_t cx[12], cw[12];
        int16_t x = 70;
        unsigned nf = r->n_fields > 11 ? 11u : r->n_fields;
        for (unsigned c = 0; c < nf; ++c) {
            cw[c] = r->fields[c].kind == RF_TEXT ? 220 : 130;
            if (x + cw[c] > APP_R) { nf = c; break; }
            cx[c] = x; x = (int16_t)(x + cw[c] + 6);
        }
        color(C_MUTE); text(24, (int16_t)(y + 14), F_SMALL, OPT_CENTERY, "#");
        for (unsigned c = 0; c < nf; ++c) text(cx[c], (int16_t)(y + 14), F_SMALL, OPT_CENTERY, r->fields[c].label);
        y = (int16_t)(y + 28);
        unsigned row = U.rec_first;
        for (unsigned k = 0; k < 5 && row < r->rows; ++k, ++row) {
            bool on = row < used;
            int16_t ry = (int16_t)(y + k * 60);
            char b[48];
            snprintf(b, sizeof b, "%u", row + 1u);
            color(on ? C_INK : C_GREY); text(24, (int16_t)(ry + 26), F_TEXT, OPT_CENTERY, b);
            for (unsigned c = 0; c < nf; ++c) {
                const ui_field_t *f = &r->fields[c];
                fmt_cell(f, row, 0, b, sizeof b);
                if (on && tag <= T_WMAX) { U.tmap_w[tag] = (uint8_t)(c + 1u); U.tmap_i[tag] = (uint16_t)(row << 6); eve_tag(E, (uint8_t)tag); }
                box(cx[c], ry, cw[c], 54, !on ? 0xECEEF2u : U.pressed == tag ? 0xDCE6F5u : C_CARD, 6);
                color(on ? C_INK : C_GREY);
                if (strlen(b) > (f->kind == RF_TEXT ? 13u : 8u)) b[f->kind == RF_TEXT ? 13 : 8] = 0;
                text((int16_t)(cx[c] + 10), (int16_t)(ry + 27), F_TEXT, OPT_CENTERY, on ? b : "-");
                eve_tag(E, 0);
                if (on) tag++;
            }
        }
        U.rec_next = (uint16_t)row;
        button(T_PGUP, 908, (int16_t)(BODY_Y + 64), 100, 160, "Up", C_ACCENT, U.rec_first > 0);
        button(T_PGDN, 908, (int16_t)(BODY_Y + 232), 100, 160, "Down", C_ACCENT, row < r->rows);
    } else {                                             /* form: one line per field (arrays wrap at 16) */
        unsigned i = U.rec_first;
        for (; i < r->n_fields; ++i) {
            const ui_field_t *f = &r->fields[i];
            unsigned cnt = f->count > 1 ? f->count : 1u, lines = (cnt + 15u) / 16u;
            int16_t h = (int16_t)(lines * 58);
            if (y + h > APP_BOT && i > U.rec_first) break;
            if (tag + cnt > T_WMAX + 1u) break;
            color(C_INK); text(24, (int16_t)(y + 27), F_TEXT, OPT_CENTERY, f->label);
            for (unsigned e = 0; e < cnt; ++e) {
                int16_t ex, ew;
                char b[32];
                if (cnt == 1) { ex = 300; ew = 240; fmt_cell(f, 0, 0, b, sizeof b); }
                else {
                    ex = (int16_t)(260 + (e % 16u) * 39); ew = 36;
                    int64_t v = 0; ui_field_get(f, U.rec_buf, 0, e, &v);
                    if (f->items && e < f->n_items) snprintf(b, sizeof b, "%.3s", f->items[e]);
                    else snprintf(b, sizeof b, "%u", e + 1u);
                    if (f->kind != RF_BOOL) fmt_cell(f, 0, e, b, sizeof b);
                }
                int16_t ey = (int16_t)(y + (e / 16u) * 58);
                U.tmap_w[tag] = (uint8_t)(i + 1u); U.tmap_i[tag] = (uint16_t)e;
                eve_tag(E, (uint8_t)tag);
                int64_t on = 0;
                bool tick = f->kind == RF_BOOL && ui_field_get(f, U.rec_buf, 0, e, &on) && on;
                box(ex, ey, ew, 52, U.pressed == tag ? 0xDCE6F5u : tick ? C_OK : C_CARD, 6);
                color(tick ? 0xFFFFFFu : C_INK);
                text((int16_t)(ex + ew / 2), (int16_t)(ey + 26), cnt > 1 ? F_SMALL : F_TEXT, OPT_CENTER, b);
                eve_tag(E, 0);
                tag++;
            }
            y = (int16_t)(y + h + 4);
        }
        U.rec_next = (uint16_t)i;
        if (U.rec_first || i < r->n_fields) {
            button(T_PGUP, 908, (int16_t)(BODY_Y + 64), 100, 160, "Up", C_ACCENT, U.rec_first > 0);
            button(T_PGDN, 908, (int16_t)(BODY_Y + 232), 100, 160, "Down", C_ACCENT, i < r->n_fields);
        }
    }
    if (why) { color(C_MUTE); text(456, TOAST_Y + 22, F_SMALL, OPT_CENTER, why); }   /* a message covers it */
}

static void draw_confirm(void) {
    color(0x000000u); eve_cmd(E, DL_COLOR_A(140));
    eve_rect(E, 0, 0, W, 600, 1);
    eve_cmd(E, DL_COLOR_A(255));
    box(162, 150, 700, 300, C_CARD, 12);
    color(C_INK);
    /* the message, wrapped at ~40 characters */
    const char *s = U.confirm_msg;
    int16_t y = 180;
    while (*s && y < 330) {
        char line[48]; size_t n = strlen(s);
        if (n > 44) { n = 44; while (n > 20 && s[n] != ' ') n--; }
        memcpy(line, s, n); line[n] = 0;
        text(512, y, F_TEXT, OPT_CENTERX, line);
        s += n; while (*s == ' ') s++;
        y = (int16_t)(y + 36);
    }
    button(T_YES, 202, 350, 290, 70, "Yes", C_OK, true);
    button(T_NO, 532, 350, 290, 70, "No", 0x5B6577u, true);
}

/* a firmware restart asked from the web, held for someone here to agree (parameter 87) */
static void fw_hold_check(void) {
    fw_status_t st;
    fw_status(&st);
    if (!st.wait_local || (U.confirm && U.act == ACT_FW_LOCAL)) {
        if (!st.wait_local && U.confirm && U.act == ACT_FW_LOCAL) { U.confirm = false; U.act = ACT_NONE; U.dirty = true; }
        return;
    }
    if (U.confirm) return;                    /* let the other question be answered first */
    char m[160];
    snprintf(m, sizeof m, "Restart on firmware %s? Asked from the %s (%s). Measurement stops during the restart.",
             st.staged_version, st.wait_src == PLAT_SRC_WEB ? "web page" : "network", lvl_name(st.wait_level));
    ask(ACT_FW_LOCAL, m);
    U.activity = U.last_draw;                 /* light the screen */
}

void ui_draw(eve_t *e, uint32_t now) {
    E = e;
    fw_hold_check();
    rec_poll(now);
    events_poll(now);
    eve_frame_begin(e, 0xF4, 0xF5, 0xF7);
    eve_fonts_use(e);
    eve_tag(e, 0);
    header(now);
    switch (U.scr) {
    case UI_PARAMS: draw_params(); break;
    case UI_EDIT:   draw_edit(); break;
    case UI_LOGIN:  draw_login(now); break;
    case UI_EVENTS: draw_events(); break;
    case UI_SYSTEM: draw_system(now); break;
    case UI_ALARMS: draw_alarms(); break;
    case UI_ALARM_CFG: draw_alarm_cfg(); break;
    case UI_CLOCK: draw_clock(); break;
    case UI_APP:   draw_app(); break;
    case UI_REC:   draw_rec(); break;
    case UI_IOB:   draw_iob(); break;
    default:        draw_home(); break;
    }
    if (now < U.toast_until) {
        box(16, TOAST_Y, 992, 44, U.toast_bad ? 0xFDE2E0u : 0xD9F2E6u, 8);
        color(U.toast_bad ? C_BAD : C_OK);
        text(512, TOAST_Y + 22, F_TEXT, OPT_CENTER, U.toast);
    }
    nav();
    if (U.scr == UI_APP && !U.confirm) draw_app_dialog();
    if (U.confirm) draw_confirm();
    (void)eve_frame_end(e);
    U.dirty = false;
    U.last_draw = now;
}

/* ---------------- state ---------------- */
void ui_init(const ui_cfg_t *cfg, uint32_t now) {
    memset(&U, 0, sizeof U);
    U.cfg = cfg;
    U.scr = ui_app() && ui_app()->n_screens ? UI_APP : UI_HOME;
    U.activity = now;
    U.bright = 80;
    U.dirty = true;
}

bool ui_due(uint32_t now) {
    if (U.scr == UI_EVENTS) events_poll(now);
    rec_poll(now);
    if (U.toast_until && now >= U.toast_until) { U.toast_until = 0; U.dirty = true; }
    return U.dirty || now - U.last_draw >= 250u;
}

uint8_t ui_brightness(uint32_t now) { return now - U.activity > IDLE_DIM_MS ? 10 : U.bright; }
int ui_screen(void) { return U.scr; }
int ui_app_screen(void) { return U.app_scr; }
bool ui_record_open(uint16_t id) { const ui_record_t *r = ui_app_record(id); return r && rec_open(r, U.last_draw); }
uint8_t ui_level(void) { return U.level; }
const char *ui_toast(void) { return U.toast; }
