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
    T_ROW0 = 200            /* .. T_ROW0 + ROWS - 1 */
};

typedef enum { KB_NUM, KB_IP, KB_TEXT, KB_BOOL } kb_t;
typedef enum { ACT_NONE, ACT_POST, ACT_BLOB, ACT_FW } act_t;

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
    char     confirm_msg[96];
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
        fw_err_t f = fw_request_activate();
        if (f != FW_OK) toast(true, now, "Cannot restart: %s", fw_err_name(f));
        else toast(false, now, "Restarting on the new firmware...");
        U.act = ACT_NONE;
        return;
    }
    U.act = ACT_NONE;
    if (e != PERR_OK) { toast(true, now, "%s", perr_text(e)); return; }
    toast(false, now, "Saved");
    if (U.scr == UI_EDIT) goto_scr(U.back);
}

/* ---------------- edit ---------------- */
static void edit_begin(const param_def_t *d, uint32_t now) {
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

static void edit_submit(bool on, uint32_t now) {
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

static void tap(uint8_t t, uint32_t now) {
    if (U.confirm) {
        if (t == T_YES) { U.confirm = false; do_action(now); }
        else if (t == T_NO) { U.confirm = false; U.act = ACT_NONE; }
        U.dirty = true;
        return;
    }
    switch (t) {
    case T_NAV_HOME: goto_scr(UI_HOME); return;
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
    default: break;
    }
    switch (U.scr) {
    case UI_PARAMS:
        if (t == T_PGUP) { U.first = U.first >= ROWS ? U.first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { if (U.first + ROWS < n_params()) U.first += ROWS; U.dirty = true; }
        else if (t >= T_ROW0 && t < T_ROW0 + ROWS && U.first + (size_t)(t - T_ROW0) < n_params())
            edit_begin(param_at(U.first + (size_t)(t - T_ROW0)), now);
        break;
    case UI_EDIT:
        if (t == T_CANCEL) goto_scr(U.back);
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
    case UI_SYSTEM:
        if (t == T_FW_ACT) {
            fw_status_t st; fw_status(&st);
            if (U.level < PLAT_LVL_FACTORY) { toast(true, now, "Log in at factory (tech) level to change firmware"); break; }
            char m[96]; snprintf(m, sizeof m, "Restart on firmware %s now? Measurement stops during the restart.", st.staged_version);
            ask(ACT_FW, m);
        } else if (t == T_BRIGHT_DN) { U.bright = U.bright > 20 ? (uint8_t)(U.bright - 20) : 10; U.dirty = true; }
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
static void text(int16_t x, int16_t y, int16_t f, uint16_t o, const char *s) { eve_text(E, x, y, f, o, s); }
static void box(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t c, uint16_t r) { color(c); eve_rect(E, x, y, w, h, r); }

static void button(uint8_t tag, int16_t x, int16_t y, int16_t w, int16_t h, const char *label, uint32_t fg, bool enabled) {
    eve_tag(E, enabled ? tag : 0);
    eve_fgcolor(E, !enabled ? C_GREY : (U.pressed == tag ? C_PRESS : fg));
    color(0xFFFFFFu);
    eve_button(E, x, y, w, h, F_TEXT, OPT_FLAT, label);
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
    if (!param_store_calibration_valid()) { box(x, 14, 150, 36, C_BAD, 18); color(0xFFFFFFu); text((int16_t)(x + 75), 32, F_SMALL, OPT_CENTER, "CAL INVALID"); x = (int16_t)(x + 160); }
    if (plat_net_status_word & NETST_CONFLICT) { box(x, 14, 150, 36, C_BAD, 18); color(0xFFFFFFu); text((int16_t)(x + 75), 32, F_SMALL, OPT_CENTER, "IP CONFLICT"); x = (int16_t)(x + 160); }
    if (param_remote_link_lost()) { box(x, 14, 150, 36, C_WARN, 18); color(0xFFFFFFu); text((int16_t)(x + 75), 32, F_SMALL, OPT_CENTER, "LINK LOST"); x = (int16_t)(x + 160); }
    if ((plat_log_status() & LSTAT_NOR_FAULT) && x < 860) { box(x, 14, 120, 36, C_BAD, 18); color(0xFFFFFFu); text((int16_t)(x + 60), 32, F_SMALL, OPT_CENTER, "LOG FAULT"); }
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
        bool cur = U.scr == scr_of[k] || (k == 1 && U.scr == UI_EDIT);
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
        text(880, (int16_t)(y + ROW_H / 2), F_SMALL, OPT_CENTERY | OPT_RIGHTX, can ? "Change >" : (d->flags & PF_RO) ? "read only" : lvl_name(d->level));
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
        for (int r = 0; r < 4; ++r) eve_keys(E, 16, (int16_t)(y + r * 61), 760, 58, F_TEXT, (uint16_t)(OPT_FLAT | o), rows[r]);
        button(T_SHIFT, 784, y, 224, 58, U.shift ? "abc" : "ABC", C_ACCENT, true);
        button(T_SPACE, 784, (int16_t)(y + 61), 224, 58, "Space", C_ACCENT, true);
        button(T_BKSP, 784, (int16_t)(y + 122), 224, 58, "Delete", C_ACCENT, true);
        button(T_CLEAR, 784, (int16_t)(y + 183), 224, 58, "Clear", C_ACCENT, true);
    } else {
        const char *rows[] = { "789", "456", "123", U.kb == KB_IP ? "0." : "-0." };
        for (int r = 0; r < 4; ++r) eve_keys(E, 16, (int16_t)(y + r * 61), 420, 58, F_BIG, (uint16_t)(OPT_FLAT | o), rows[r]);
        button(T_BKSP, 444, y, 200, 58, "Delete", C_ACCENT, true);
        button(T_CLEAR, 444, (int16_t)(y + 61), 200, 58, "Clear", C_ACCENT, true);
    }
}

static void draw_edit(void) {
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
    box(32, BODY_Y + 76, 560, 40, 0xF4F5F7u, 6);
    color(C_INK);
    text(44, BODY_Y + 96, F_BIG, OPT_CENTERY, m);
    if (now < U.lock_until) { color(C_BAD); text(610, BODY_Y + 96, F_SMALL, OPT_CENTERY, "Too many attempts: wait"); }
    button(T_OK, 784, BODY_Y + 76, 108, 40, "Enter", C_OK, now >= U.lock_until);
    button(T_CANCEL, 900, BODY_Y + 76, 108, 40, "Cancel", 0x5B6577u, true);
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
    else kv(32, BODY_Y + 72, "State", st.rolled_back ? "last update rolled back" : "normal");
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

void ui_draw(eve_t *e, uint32_t now) {
    E = e;
    events_poll(now);
    eve_frame_begin(e, 0xF4, 0xF5, 0xF7);
    eve_tag(e, 0);
    header(now);
    switch (U.scr) {
    case UI_PARAMS: draw_params(); break;
    case UI_EDIT:   draw_edit(); break;
    case UI_LOGIN:  draw_login(now); break;
    case UI_EVENTS: draw_events(); break;
    case UI_SYSTEM: draw_system(now); break;
    default:        draw_home(); break;
    }
    if (now < U.toast_until) {
        box(16, TOAST_Y, 992, 44, U.toast_bad ? 0xFDE2E0u : 0xD9F2E6u, 8);
        color(U.toast_bad ? C_BAD : C_OK);
        text(512, TOAST_Y + 22, F_TEXT, OPT_CENTER, U.toast);
    }
    nav();
    if (U.confirm) draw_confirm();
    (void)eve_frame_end(e);
    U.dirty = false;
    U.last_draw = now;
}

/* ---------------- state ---------------- */
void ui_init(const ui_cfg_t *cfg, uint32_t now) {
    memset(&U, 0, sizeof U);
    U.cfg = cfg;
    U.scr = UI_HOME;
    U.activity = now;
    U.bright = 80;
    U.dirty = true;
}

bool ui_due(uint32_t now) {
    if (U.scr == UI_EVENTS) events_poll(now);
    if (U.toast_until && now >= U.toast_until) { U.toast_until = 0; U.dirty = true; }
    return U.dirty || now - U.last_draw >= 250u;
}

uint8_t ui_brightness(uint32_t now) { return now - U.activity > IDLE_DIM_MS ? 10 : U.bright; }
int ui_screen(void) { return U.scr; }
uint8_t ui_level(void) { return U.level; }
const char *ui_toast(void) { return U.toast; }
