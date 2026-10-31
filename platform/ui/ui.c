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
#include "plat_state.h"
#include "plat_power.h"
#include "plat_cal.h"
#include "modbus.h"
#include "plat_service.h"
#include "plat_selftest.h"
#include "plat_boardtest.h"
#include "inst.h"
#include "emis_bind.h"
#include "plat_version.h"

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
#define DT_TRAIL 128             /* touch test: points kept */
#define DT_PATTERNS 7            /* display test: full-screen patterns */
#define REG_CLOCK_DT 0x302008u   /* BT817 REG_CLOCK: counts system clock cycles (display test) */
#define IDLE_DIM_MS (10u * 60u * 1000u)
#define LOCK_MS 30000u

/* tags: 32..126 are keyboard keys (CMD_KEYS uses the character) */
enum {
    T_NAV_HOME = 128, T_NAV_PARAMS, T_NAV_EVENTS, T_NAV_SYSTEM, T_NAV_LOGIN,
    T_MODE, T_PGUP, T_PGDN, T_OK, T_CANCEL, T_BKSP, T_CLEAR, T_SHIFT, T_ON, T_OFF,
    T_LVL1, T_LVL2, T_LVL3, T_YES, T_NO, T_FW_ACT, T_BRIGHT_DN, T_BRIGHT_UP, T_SPACE,
    T_ALARMS, T_ACK_ALL, T_ACTIONS, T_BACK, T_CLOCK, T_IOB, T_MAINT, T_CAL,
    T_CFG0 = 160,           /* .. T_CFG0 + ROWS * 6 - 1: alarm action toggles */
    /* application screens and record editors (no keyboard there, so 1..126 are free):
     * 1..8 tabs, T_W0.. widgets and cells (looked up in U.tmap), T_DLG_* the dialog */
    T_TAB0 = 1, T_W0 = 10, T_WMAX = 118, T_DLG_YES = 120, T_DLG_NO = 121,
    T_ROW0 = 200,           /* .. T_ROW0 + ROWS - 1 */
    T_CAL_START = 210, T_CAL_ACCEPT, T_CAL_ABORT, T_CAL_REVERT,
    T_PRIO0 = 214,          /* .. T_PRIO0 + ROWS - 1: an alarm's tier (actions page), a tier's "Use defaults" (tiers page) */
    T_TIERS = 236,          /* actions page: open the tiers page */
    T_TR_PEN0 = 196,        /* .. 199: trend pens */
    T_TR_ADD = 206, T_TR_DEL, T_TR_CLR, T_TRENDS,       /* trends */
    T_SVC = 226, T_SVC_BT, T_SVC_LOG,                  /* service screen, board test, add a service log entry */
    T_INST = 229,           /* System: open the instruments page */
    T_EMIS = 237,           /* System: open the emissions page */
    T_AL_ROW0 = 220,        /* .. T_AL_ROW0 + ROWS - 1: tap an alarm for what to check */
    T_DISP = 230, T_DT_COLOURS, T_DT_TOUCH, T_DT_NEXT, T_DT_CLEAR, T_DT_DONE,  /* display test (bench bring-up) */
    T_FORCE = 240, T_FC_CLEAR_ALL,      /* Modbus force (commissioning) */
    T_DIA = 242, T_DIA_INPUT, T_DIA_WHEN, T_DIA_DELAY, T_DIA_LATCH, T_DIA_SEV, T_DIA_LABEL, T_DIA_PREV, T_DIA_NEXT   /* input alarms */
};

typedef enum { KB_NUM, KB_IP, KB_TEXT, KB_BOOL } kb_t;
typedef enum { ACT_NONE, ACT_POST, ACT_BLOB, ACT_FW, ACT_FW_LOCAL, ACT_CAL, ACT_FORCE, ACT_ELEM, ACT_BT } act_t;
#define TR_PENS 4
#define INST_PT_ROWS 9u          /* instruments page: point rows shown */
#define EMIS_ROWS 5u             /* emissions page: channel rows shown */
#define TR_N    150              /* trend samples kept: 10 minutes at TR_MS */
#define TR_MS   4000u

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
    uint16_t al_rows[ROWS], al_sel;   /* ids on the alarm rows; the one showing its help (0 = none) */
    size_t   cfg_first;
    /* application screens */
    int      app_scr;
    uint8_t  app_first, app_next, app_hist[8], app_nh;
    uint8_t  tmap_w[128];          /* tag -> widget (or field) index + 1 */
    uint16_t tmap_i[128];          /* tag -> grid item / cell (row << 6 | element) */
    char     ok_msg[48];
    uint8_t  cal_ch, cal_cmd;      /* calibration screen: the channel selected; the command being confirmed */
    /* display test: 0 = the status page, 1 = full-screen patterns (dt_pat), 2 = touch test */
    uint8_t  dt_mode, dt_pat;
    int16_t  dt_x[DT_TRAIL], dt_y[DT_TRAIL];   /* touch test: the points seen, oldest first */
    uint16_t dt_n;
    bool     dt_down;              /* a finger is on the glass (touch test) */
    int16_t  dt_dx, dt_dy;         /* the last touch-down's offset from the nearest target */
    uint8_t  dt_tgt;               /* that target + 1 (0 = none yet) */
    uint32_t dt_t0, dt_frames0, dt_clock0;   /* REG_FRAMES and REG_CLOCK at dt_t0, for the rates */
    uint32_t dt_hz10, dt_mhz10;    /* refresh rate and system clock measured, x 10 (0 = not yet) */
    /* Modbus force: the list's first row; the rows' value and element; the one being forced */
    unsigned fc_first;
    uint16_t fc_id[ROWS], fc_idx[ROWS];
    bool     forcing;              /* UI_EDIT is entering a force value, not a setting */
    uint16_t fc_index;             /* the element being forced (MB_FORCE_SCALAR for a scalar) */
    double   fc_v;                 /* the value waiting for "Yes" */
    /* input alarms: the list's first slot; the slot open; UI_EDIT changing one array element */
    unsigned dia_first, dia_s;
    bool     elem;                 /* UI_EDIT is entering element el_idx of the array U.ed */
    uint16_t el_idx;
    char     el_title[48];
    /* a confirmed one-byte write to one element of an array (ACT_ELEM) */
    uint16_t pe_id, pe_idx; uint8_t pe_v;
    /* trends: the pens (parameter IDs), the selected one, the picker's first row; history below */
    uint16_t tr_id[TR_PENS];
    uint8_t  tr_sel;
    unsigned tr_first;
    /* service log entry from the touchscreen: 1 = typing who, 2 = typing the work */
    uint8_t  svc_step;
    uint8_t  inst_sel;             /* instruments page: the instrument shown (0..INST_MAX-1), its first point row */
    unsigned inst_first;
    unsigned emis_first;           /* emissions page: first channel row (of the channels in use) */
    char     svc_tech[16];
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

/* write one element of an array parameter (a whole-array write is never needed for one cell) */
static bool elem_post(uint16_t id, uint16_t idx, uint32_t v, uint32_t now) {
    const param_def_t *d = param_find(id);
    if (!d || d->ptype != PT_ARRAY) return false;
    size_t es = param_ctype_size(d->ctype);
    uint8_t b[4];
    if (es == 1) b[0] = (uint8_t)v;
    else if (es == 2) { uint16_t x = (uint16_t)v; memcpy(b, &x, 2); }
    else memcpy(b, &v, 4);
    uint16_t off = (uint16_t)(idx * es);
    param_err_t e = param_blob_check(id, off, b, (uint16_t)es, false, PLAT_SRC_LOCAL, U.level);
    if (e == PERR_OK) e = param_blob_post(id, off, b, (uint16_t)es, false, PLAT_SRC_LOCAL, U.level);
    if (e == PERR_ACCESS) { toast(true, now, "Log in as %s to change this", lvl_name(d->level)); return false; }
    if (e != PERR_OK) { toast(true, now, "%s", perr_text(e)); return false; }
    toast(false, now, "Saved");
    return true;
}

/* a refusal from mb_force_* in words */
static const char *force_err(int r) {
    switch (r) {
    case PERR_ACCESS: return "Log in as admin to force values";
    case PERR_NO_ID:  return "This value can't be forced";
    case PERR_RANGE:  return "Modbus can't carry that value here";
    case PERR_BUSY:   return "Too many values forced: clear one first";
    default:          return perr_text((param_err_t)r);
    }
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
    } else if (U.act == ACT_CAL) {
        int r = U.cal_cmd == PLAT_CAL_CMD_BEGIN ? plat_cal_begin(U.cal_ch, PLAT_SRC_LOCAL, U.level)
              : U.cal_cmd == PLAT_CAL_CMD_REVERT ? plat_cal_revert(U.cal_ch, PLAT_SRC_LOCAL, U.level)
              : plat_cal_end(U.cal_cmd == PLAT_CAL_CMD_ACCEPT, PLAT_SRC_LOCAL, U.level);
        static const char *const done[] = { "", "Calibration started", "Accepted", "Calibration aborted", "Factory calibration restored" };
        if (r != PERR_OK) toast(true, now, "%s", perr_text((param_err_t)r));
        else toast(false, now, "%s", done[U.cal_cmd <= PLAT_CAL_CMD_REVERT ? U.cal_cmd : 0]);
        U.act = ACT_NONE;
        return;
    } else if (U.act == ACT_FORCE) {
        int r = mb_force_set(U.ed->id, U.fc_index, U.fc_v, PLAT_SRC_LOCAL, U.level, now);
        U.act = ACT_NONE;
        if (r != PERR_OK) { toast(true, now, "%s", force_err(r)); return; }
        toast(false, now, "Modbus now reads the forced value for %u s", (unsigned)(PLAT_MB_FORCE_MS / 1000u));
        U.forcing = false;
        goto_scr(UI_FORCE);
        return;
    } else if (U.act == ACT_ELEM) {
        e = param_blob_post(U.pe_id, U.pe_idx, &U.pe_v, 1, false, PLAT_SRC_LOCAL, U.level);
    } else if (U.act == ACT_BT) {
        int r = plat_boardtest_start(PLAT_SRC_LOCAL, U.level);
        U.act = ACT_NONE;
        if (r == -3) toast(true, now, "Log in at factory (tech) level to run the board test");
        else if (r) toast(true, now, "Not now: a test is running or the start-up self-test has not finished");
        else toast(false, now, "Board test started");
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
    if (U.svc_step) {                                 /* a service log entry: who, then the work done */
        if (!U.entry[0]) { toast(true, now, U.svc_step == 1 ? "Enter who did the work" : "Enter the work done"); return; }
        if (U.svc_step == 1) {
            snprintf(U.svc_tech, sizeof U.svc_tech, "%.15s", U.entry);
            U.svc_step = 2; U.entry[0] = 0; U.shift = false; U.dirty = true;
            return;
        }
        int r = plat_service_log(U.svc_tech, U.entry, PLAT_SRC_LOCAL, U.level);
        if (r == -3) { toast(true, now, "Log in at factory (tech) level to add to the service log"); return; }
        if (r) { toast(true, now, r == -1 ? "Busy: try again" : "Too long (at most 39 characters)"); return; }
        toast(false, now, "Service log entry added; hours since service start again");
        U.svc_step = 0;
        goto_scr(UI_SERVICE);
        return;
    }
    const param_def_t *d = U.ed;
    if (U.elem) {                                     /* one element of an array (input alarm settings) */
        char *end = NULL;
        unsigned long x = strtoul(U.entry, &end, 10);
        if (!U.entry[0] || *end || U.entry[0] == '-') { toast(true, now, "Not a valid number"); return; }
        if (x < d->vmin.u || x > d->vmax.u) { toast(true, now, "Allowed %lu .. %lu", (unsigned long)d->vmin.u, (unsigned long)d->vmax.u); return; }
        if (elem_post(d->id, U.el_idx, (uint32_t)x, now)) { U.elem = false; goto_scr(U.back); }
        return;
    }
    if (U.forcing) {                                  /* a Modbus force: the value only, then "Are you sure?" */
        double x = on ? 1.0 : 0.0;
        if (U.kb != KB_BOOL) {
            char *end = NULL;
            x = strtod(U.entry, &end);
            if (!U.entry[0] || *end || !isfinite(x)) { toast(true, now, "Not a valid number"); return; }
        }
        U.fc_v = x;
        char m[128], v[24];
        if (U.kb == KB_BOOL) snprintf(v, sizeof v, "%s", on ? "ON" : "OFF");
        else fmt_float(v, sizeof v, x);
        snprintf(m, sizeof m, "Send %s to Modbus as %.24s for %u s? The PLC sees it instead of the real value.",
                 v, d->name, (unsigned)(PLAT_MB_FORCE_MS / 1000u));
        ask(ACT_FORCE, m);
        return;
    }
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

/* MAINTENANCE (CIP-09, section 7): the operator enters and leaves it here, confirmed;
 * parameter 301, so levels and the event log apply as for any write */
static bool maint_on(void) { plat_val_t v; return param_read(PARAM_ID_MAINTENANCE, &v) && v.b; }
static void maint_tap(uint32_t now) {
    bool in = maint_on();
    if (U.level < PLAT_LVL_OPERATOR) { toast(true, now, "Log in as operator to %s maintenance", in ? "leave" : "enter"); return; }
    plat_cmd_t c = { .id = PARAM_ID_MAINTENANCE, .src = PLAT_SRC_LOCAL, .kind = PLAT_VAL_BOOL, .level = U.level, .v.b = !in };
    U.pend = c;
    snprintf(U.ok_msg, sizeof U.ok_msg, "%s", in ? "Maintenance ended" : "Maintenance started");
    ask(ACT_POST, in ? "Leave maintenance? Readings are reported as valid again."
                     : "Start maintenance? Every reading is marked \"maintenance\" until you leave it.");
}

/* Calibration (CIP-18): every command is checked first so a refusal says why in words, then
 * confirmed; plat_cal applies it in the scan with the same checks */
static void cal_tap(uint8_t cmd, uint32_t now) {
    if (plat_cal_check(cmd, U.cal_ch, U.level) != PERR_OK) { toast(true, now, "%s", plat_cal_refusal()); return; }
    char m[112];
    const char *n = plat_cal_name(U.cal_ch);
    if (cmd == PLAT_CAL_CMD_BEGIN) snprintf(m, sizeof m, "Start calibrating %s? Readings are marked \"calibration\" until it ends.", n);
    else if (cmd == PLAT_CAL_CMD_ACCEPT) snprintf(m, sizeof m, "Accept the new %s calibration and put it in force?", n);
    else if (cmd == PLAT_CAL_CMD_ABORT) snprintf(m, sizeof m, "Abort the %s calibration? The calibration in force stays.", n);
    else snprintf(m, sizeof m, "Put the factory calibration of %s back in force?", n);
    U.cal_cmd = cmd;
    ask(ACT_CAL, m);
}

/* ---------------- input ---------------- */
static size_t n_params(void) { return param_count(); }

static bool app_tap(uint8_t t, uint32_t now);
static int64_t pv(uint16_t id, uint16_t idx, int64_t dflt);
static void rec_tap(uint8_t t, uint32_t now);
/* input alarm slot U.dia_s: toggles and the cycle write at once; numbers and the label open the editor */
static void dia_number(uint16_t id, const char *what) {
    U.ed = param_find(id);
    U.elem = true; U.el_idx = (uint16_t)U.dia_s;
    snprintf(U.el_title, sizeof U.el_title, "Input alarm %u: %s", U.dia_s + 1u, what);
    U.shift = false; U.entry[0] = 0; U.kb = KB_NUM;
    U.back = UI_DIA_SLOT;
    goto_scr(UI_EDIT);
}

static void dia_tap(uint8_t t, uint32_t now) {
    unsigned s = U.dia_s;
    int64_t v = 0;
    if (t == T_BACK) { U.dia_first = s / ROWS * ROWS; goto_scr(UI_DIA); return; }
    if (t == T_DIA_PREV) { U.dia_s = s ? s - 1u : PLAT_DIA_SLOTS - 1u; U.dirty = true; return; }
    if (t == T_DIA_NEXT) { U.dia_s = (s + 1u) % PLAT_DIA_SLOTS; U.dirty = true; return; }
    if (t < T_DIA_INPUT || t > T_DIA_LABEL) return;
    if (U.level < PLAT_LVL_ADMIN) { toast(true, now, "Log in as admin to set up input alarms"); return; }
    switch (t) {
    case T_DIA_INPUT: dia_number(PARAM_ID_DIA_INPUT, "input"); break;
    case T_DIA_DELAY: dia_number(PARAM_ID_DIA_DELAY, "on-delay"); break;
    case T_DIA_WHEN:  ui_param_value(PARAM_ID_DIA_WHEN, (uint16_t)s, &v); (void)elem_post(PARAM_ID_DIA_WHEN, (uint16_t)s, v ? 0u : 1u, now); break;
    case T_DIA_LATCH: ui_param_value(PARAM_ID_DIA_LATCH, (uint16_t)s, &v); (void)elem_post(PARAM_ID_DIA_LATCH, (uint16_t)s, v ? 0u : 1u, now); break;
    case T_DIA_SEV:   ui_param_value(PARAM_ID_DIA_SEV, (uint16_t)s, &v);
                      (void)elem_post(PARAM_ID_DIA_SEV, (uint16_t)s, v >= AL_SEV_FAULT || v < AL_SEV_WARNING ? AL_SEV_WARNING : (uint32_t)v + 1u, now); break;
    case T_DIA_LABEL: { const param_def_t *d = param_find((uint16_t)(PARAM_ID_DIA_LABEL_FIRST + s)); if (d) edit_begin(d, now); break; }
    default: break;
    }
    U.dirty = true;
}

/* ---------------- trends: sampled here whatever screen is up ---------------- */
static float    tr_v[TR_PENS][TR_N];   /* NAN = no valid value at that sample */
static unsigned tr_head, tr_count;      /* tr_head = the next slot written */
static uint32_t tr_last;
static bool tr_value(uint16_t id, double *out) {   /* a numeric scalar, valid quality */
    const param_def_t *d = id ? param_find(id) : NULL;
    plat_val_t v;
    if (!d || !param_read(id, &v) || param_quality(id, UW_NO_INDEX) != PLAT_Q_VALID) return false;
    switch (d->ptype) {
    case PT_BOOL:  *out = v.b ? 1.0 : 0.0; return true;
    case PT_INT:   *out = v.i; return true;
    case PT_UINT:  *out = v.u; return true;
    case PT_FLOAT: *out = v.f; return isfinite(v.f);
    default:       return false;
    }
}
static bool tr_able(const param_def_t *d) {
    return d && d->ptype <= PT_FLOAT && !(d->flags & (PF_SECRET | PF_DEV)) && !is_ip(d);
}
static void tr_sample(uint32_t now) {
    if (now - tr_last < TR_MS && tr_count) return;
    bool any = false;
    for (unsigned p = 0; p < TR_PENS; ++p) any |= U.tr_id[p] != 0;
    if (!any) return;
    tr_last = now;
    for (unsigned p = 0; p < TR_PENS; ++p) { double x; tr_v[p][tr_head] = U.tr_id[p] && tr_value(U.tr_id[p], &x) ? (float)x : NAN; }
    tr_head = (tr_head + 1u) % TR_N;
    if (tr_count < TR_N) ++tr_count;
    if (U.scr == UI_TRENDS) U.dirty = true;
}
static void tr_clear(void) { tr_count = 0; tr_head = 0; }
static float tr_at(unsigned p, unsigned k) { return tr_v[p][(tr_head + TR_N - tr_count + k) % TR_N]; }   /* k = 0 oldest */

/* the values a trend can follow: numeric scalars, not secret */
static unsigned tr_pick_count(void) {
    unsigned n = 0;
    for (size_t i = 0; i < n_params(); ++i) n += tr_able(param_at(i));
    return n;
}
static const param_def_t *tr_pick_at(unsigned k) {   /* the product's values first, then the platform's */
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < n_params(); ++i) {
            const param_def_t *d = param_at(i);
            if (tr_able(d) && (d->id >= PARAM_ID_APP_FIRST) == (pass == 0) && !k--) return d;
        }
    return NULL;
}

/* the tiers page: rename, actions (AR_SET | bits for alarms with none of their own), back to the defaults */
static void tier_tap(uint8_t t, uint32_t now) {
    static const uint8_t bit[6] = { AR_EMAIL, AR_MODBUS, AR_RELAY(0), AR_RELAY(1), AR_RELAY(2), AR_RELAY(3) };
    if (t == T_BACK) { goto_scr(UI_ALARM_CFG); return; }
    bool name = t >= T_ROW0 && t < T_ROW0 + AL_TIERS, dflt = t >= T_PRIO0 && t < T_PRIO0 + AL_TIERS;
    bool act = t >= T_CFG0 && t < T_CFG0 + AL_TIERS * 6;
    if (!name && !dflt && !act) return;
    if (U.level < PLAT_LVL_ADMIN) { toast(true, now, "Log in as admin to change the tiers"); return; }
    if (name) { const param_def_t *d = param_find((uint16_t)(PARAM_ID_ALARM_TIER_NAME_FIRST + t - T_ROW0)); if (d) edit_begin(d, now); return; }
    unsigned k = dflt ? (unsigned)(t - T_PRIO0) : (unsigned)(t - T_CFG0) / 6u;
    uint8_t r = (uint8_t)pv(PARAM_ID_ALARM_TIER_ROUTE, (uint16_t)k, 0), v;
    if (dflt) v = 0;
    else v = (uint8_t)(AR_SET | ((r & AR_SET ? r : 0u) ^ bit[(t - T_CFG0) % 6u]));
    param_err_t e = param_blob_post(PARAM_ID_ALARM_TIER_ROUTE, (uint16_t)k, &v, 1, false, PLAT_SRC_LOCAL, U.level);
    if (e != PERR_OK) toast(true, now, "%s", perr_text(e));
    U.dirty = true;
}

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
    case T_MAINT: maint_tap(now); return;
    case T_FORCE: U.fc_first = 0; goto_scr(UI_FORCE); return;
    case T_ALARMS: U.al_first = 0; U.al_sel = 0; goto_scr(UI_ALARMS); return;
    case T_ACTIONS: U.cfg_first = 0; goto_scr(UI_ALARM_CFG); return;
    case T_TRENDS: goto_scr(UI_TRENDS); return;
    default: break;
    }
    switch (U.scr) {
    case UI_PARAMS:
        if (t == T_PGUP) { U.first = U.first >= ROWS ? U.first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { if (U.first + ROWS < n_params()) U.first += ROWS; U.dirty = true; }
        else if (t >= T_ROW0 && t < T_ROW0 + ROWS && U.first + (size_t)(t - T_ROW0) < n_params())
            edit_begin(param_at(U.first + (size_t)(t - T_ROW0)), now);
        else if (t == T_FORCE) { U.fc_first = 0; goto_scr(UI_FORCE); }
        break;
    case UI_FORCE:
        if (t == T_PGUP) { U.fc_first = U.fc_first >= ROWS ? U.fc_first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { U.fc_first += ROWS; U.dirty = true; }
        else if (t == T_BACK) goto_scr(UI_PARAMS);
        else if (t == T_FC_CLEAR_ALL) {
            int r = mb_force_clear_all(PLAT_SRC_LOCAL, U.level);
            if (r != PERR_OK) toast(true, now, "%s", force_err(r)); else toast(false, now, "All forces cleared: Modbus reads the real values");
        } else if (t >= T_ROW0 && t < T_ROW0 + ROWS && U.fc_id[t - T_ROW0]) {
            uint16_t id = U.fc_id[t - T_ROW0], ix = U.fc_idx[t - T_ROW0];
            if (mb_force_active(id, ix)) {
                int r = mb_force_clear(id, ix, PLAT_SRC_LOCAL, U.level);
                if (r != PERR_OK) toast(true, now, "%s", force_err(r)); else toast(false, now, "Cleared: Modbus reads the real value");
            } else if (U.level < PLAT_LVL_ADMIN) toast(true, now, "Log in as admin to force values");
            else {
                const param_def_t *d = param_find(id);
                U.ed = d; U.shift = false; U.entry[0] = 0;
                U.kb = d->ctype == CT_BOOL ? KB_BOOL : KB_NUM;
                U.forcing = true; U.fc_index = ix;
                U.back = UI_FORCE;
                goto_scr(UI_EDIT);
            }
        }
        U.dirty = true;
        break;
    case UI_REC: rec_tap(t, now); break;
    case UI_EDIT:
        if (t == T_CANCEL) { if (U.cell_f) { U.cell_f = NULL; goto_scr(UI_REC); } else { U.forcing = false; U.elem = false; U.svc_step = 0; goto_scr(U.back); } }
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
        else if (t >= T_AL_ROW0 && t < T_AL_ROW0 + ROWS && U.al_rows[t - T_AL_ROW0]) {
            uint16_t id = U.al_rows[t - T_AL_ROW0];
            U.al_sel = U.al_sel == id ? 0 : id; U.dirty = true;
        }
        else if (t == T_DIA) { U.dia_first = 0; goto_scr(UI_DIA); }
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
    case UI_DIA:
        if (t == T_PGUP) { U.dia_first = U.dia_first >= ROWS ? U.dia_first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { if (U.dia_first + ROWS < PLAT_DIA_SLOTS) U.dia_first += ROWS; U.dirty = true; }
        else if (t == T_BACK) goto_scr(UI_ALARMS);
        else if (t >= T_ROW0 && t < T_ROW0 + ROWS && U.dia_first + (unsigned)(t - T_ROW0) < PLAT_DIA_SLOTS) {
            U.dia_s = U.dia_first + (unsigned)(t - T_ROW0);
            goto_scr(UI_DIA_SLOT);
        }
        break;
    case UI_DIA_SLOT: dia_tap(t, now); break;
    case UI_ALARM_CFG:
        if (t == T_BACK) goto_scr(UI_ALARMS);
        else if (t == T_PGUP) { U.cfg_first = U.cfg_first >= ROWS ? U.cfg_first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { if (U.cfg_first + ROWS < alarm_count()) U.cfg_first += ROWS; U.dirty = true; }
        else if (t == T_TIERS) goto_scr(UI_TIERS);
        else if (t >= T_ROW0 && t < T_ROW0 + ROWS) {     /* the alarm's name: back to its tier's (or default) actions */
            const alarm_def_t *d = alarm_def_at(U.cfg_first + (unsigned)(t - T_ROW0));
            if (!d) break;
            if (!(pv(PARAM_ID_ALARM_ROUTE, d->id, 0) & AR_SET)) { toast(false, now, "%s follows its tier's actions", d->name); break; }
            if (U.level < PLAT_LVL_ADMIN) { toast(true, now, "Log in as admin to change alarm actions"); break; }
            char m[112];
            snprintf(m, sizeof m, "Put %.32s back on its tier's actions (or its default if the tier has none)?", d->name);
            U.pe_id = PARAM_ID_ALARM_ROUTE; U.pe_idx = d->id; U.pe_v = 0;
            snprintf(U.ok_msg, sizeof U.ok_msg, "%.24s follows its tier again", d->name);
            ask(ACT_ELEM, m);
        }
        else if (t >= T_PRIO0 && t < T_PRIO0 + ROWS) {   /* the tier: default, Critical .. Info, Off, round again */
            const alarm_def_t *d = alarm_def_at(U.cfg_first + (unsigned)(t - T_PRIO0));
            if (!d) break;
            if (U.level < PLAT_LVL_ADMIN) { toast(true, now, "Log in as admin to change alarm tiers"); break; }
            int64_t p = pv(PARAM_ID_ALARM_PRIO, d->id, 0);
            uint8_t v = (uint8_t)(p >= AL_PRIO_DEFAULT && p < AL_PRIO_OFF ? p + 1 : AL_PRIO_DEFAULT);
            if (v == AL_PRIO_OFF && !alarm_can_switch_off(d)) v = AL_PRIO_DEFAULT;   /* AL_STOPS, AL_NO_OFF */
            param_err_t e = param_blob_post(PARAM_ID_ALARM_PRIO, d->id, &v, 1, false, PLAT_SRC_LOCAL, U.level);
            if (e != PERR_OK) toast(true, now, "%s", perr_text(e));
            U.dirty = true;
        }
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
    case UI_TIERS: tier_tap(t, now); break;
    case UI_TRENDS:
        if (t >= T_TR_PEN0 && t < T_TR_PEN0 + TR_PENS) { U.tr_sel = (uint8_t)(t - T_TR_PEN0); U.dirty = true; }
        else if (t == T_TR_ADD) {
            unsigned p = 0;
            while (p < TR_PENS && U.tr_id[p]) ++p;
            if (p == TR_PENS) { toast(true, now, "Up to %u values: remove one first", TR_PENS); break; }
            U.tr_first = 0; goto_scr(UI_TR_PICK);
        } else if (t == T_TR_DEL && U.tr_id[U.tr_sel]) {
            U.tr_id[U.tr_sel] = 0;
            for (unsigned k = 0; k < TR_N; ++k) tr_v[U.tr_sel][k] = NAN;
            for (unsigned p = 0; p < TR_PENS; ++p) if (U.tr_id[p]) { U.tr_sel = (uint8_t)p; break; }
            U.dirty = true;
        } else if (t == T_TR_CLR) { tr_clear(); U.dirty = true; }
        break;
    case UI_TR_PICK:
        if (t == T_BACK) goto_scr(UI_TRENDS);
        else if (t == T_PGUP) { U.tr_first = U.tr_first >= ROWS ? U.tr_first - ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { if (U.tr_first + ROWS < tr_pick_count()) U.tr_first += ROWS; U.dirty = true; }
        else if (t >= T_ROW0 && t < T_ROW0 + ROWS) {
            const param_def_t *d = tr_pick_at(U.tr_first + (unsigned)(t - T_ROW0));
            if (!d) break;
            unsigned p = 0;
            for (unsigned k = 0; k < TR_PENS; ++k) if (U.tr_id[k] == d->id) { toast(true, now, "Already trended"); goto_scr(UI_TRENDS); return; }
            while (p < TR_PENS && U.tr_id[p]) ++p;
            if (p < TR_PENS) {
                U.tr_id[p] = d->id; U.tr_sel = (uint8_t)p;
                for (unsigned k = 0; k < TR_N; ++k) tr_v[p][k] = NAN;   /* history starts now */
            }
            goto_scr(UI_TRENDS);
        }
        break;
    case UI_EMIS:
        if (t == T_BACK) goto_scr(UI_SYSTEM);
        else if (t == T_PGUP) { U.emis_first = U.emis_first >= EMIS_ROWS ? U.emis_first - EMIS_ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { U.emis_first += EMIS_ROWS; U.dirty = true; }
        break;
    case UI_INST:
        if (t == T_BACK) goto_scr(UI_SYSTEM);
        else if (t >= T_ROW0 && t < T_ROW0 + INST_MAX) { U.inst_sel = (uint8_t)(t - T_ROW0); U.inst_first = 0; U.dirty = true; }
        else if (t == T_PGUP) { U.inst_first = U.inst_first >= INST_PT_ROWS ? U.inst_first - INST_PT_ROWS : 0; U.dirty = true; }
        else if (t == T_PGDN) { U.inst_first += INST_PT_ROWS; U.dirty = true; }
        break;
    case UI_SERVICE:
        if (t == T_BACK) goto_scr(UI_SYSTEM);
        else if (t == T_DISP) { U.dt_mode = 0; U.dt_t0 = 0; U.dt_hz10 = U.dt_mhz10 = 0; goto_scr(UI_DISPLAY); }
        else if (t == T_SVC_BT) {
            if (U.level < PLAT_LVL_FACTORY) { toast(true, now, "Log in at factory (tech) level to run the board test"); break; }
            ask(ACT_BT, "Run the board test now? It checks the settings memory byte by byte: run it on the bench, not on a unit in service.");
        } else if (t == T_SVC_LOG) {
            if (U.level < PLAT_LVL_FACTORY) { toast(true, now, "Log in at factory (tech) level to add to the service log"); break; }
            U.svc_step = 1; U.entry[0] = 0; U.shift = false; U.kb = KB_TEXT; U.ed = NULL;
            U.back = UI_SERVICE;
            goto_scr(UI_EDIT);
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
        else if (t == T_SVC) goto_scr(UI_SERVICE);
        else if (t == T_INST) { U.inst_first = 0; goto_scr(UI_INST); }
        else if (t == T_EMIS) { U.emis_first = 0; goto_scr(UI_EMIS); }
        else if (t == T_CAL) { unsigned a; if (plat_cal_active(&a)) U.cal_ch = (uint8_t)a; goto_scr(UI_CAL); }
        else if (t == T_BRIGHT_DN) { U.bright = U.bright > 20 ? (uint8_t)(U.bright - 20) : 10; U.dirty = true; }
        else if (t == T_BRIGHT_UP) { U.bright = U.bright < 80 ? (uint8_t)(U.bright + 20) : 100; U.dirty = true; }
        break;
    case UI_CAL:
        if (t >= T_ROW0 && t < T_ROW0 + 4 && (size_t)(t - T_ROW0) < plat_cal_count()) { U.cal_ch = (uint8_t)(t - T_ROW0); U.dirty = true; }
        else if (t == T_CAL_START) cal_tap(PLAT_CAL_CMD_BEGIN, now);
        else if (t == T_CAL_ACCEPT) cal_tap(PLAT_CAL_CMD_ACCEPT, now);
        else if (t == T_CAL_ABORT) cal_tap(PLAT_CAL_CMD_ABORT, now);
        else if (t == T_CAL_REVERT) cal_tap(PLAT_CAL_CMD_REVERT, now);
        break;
    case UI_DISPLAY:
        if (t == T_BACK) { U.dt_mode = 0; goto_scr(UI_SERVICE); }
        else if (t == T_DT_COLOURS) { U.dt_mode = 1; U.dt_pat = 0; U.dirty = true; }
        else if (t == T_DT_TOUCH) { U.dt_mode = 2; U.dt_n = 0; U.dt_tgt = 0; U.dirty = true; }
        else if (t == T_DT_NEXT && U.dt_mode == 1) { if (++U.dt_pat >= DT_PATTERNS) U.dt_mode = 0; U.dirty = true; }
        else if (t == T_DT_CLEAR) { U.dt_n = 0; U.dt_tgt = 0; U.dirty = true; }
        else if (t == T_DT_DONE) { U.dt_mode = 0; U.dirty = true; }
        break;
    default:
        break;
    }
}

void ui_input(uint8_t tag, uint32_t now) {
    tr_sample(now);
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

/* UI_CONVENTIONS 3 and 4: the instrument state, the supervisory link and a value's quality */
static uint32_t state_color(uint8_t st) {
    switch (st) {
    case PLAT_ST_READY: case PLAT_ST_MEASURING: return C_OK;
    case PLAT_ST_WARMING_UP: case PLAT_ST_MAINTENANCE: case PLAT_ST_CALIBRATION: return C_WARN;
    case PLAT_ST_FAULT: case PLAT_ST_SAFE_MODE: return C_BAD;
    default: return 0x5B6577u;
    }
}
enum { LINK_NONE, LINK_OK, LINK_LOST };
static int link_state(uint32_t now) {
    plat_val_t v;
    uint32_t to = param_read(PARAM_ID_COMMS_TIMEOUT_S, &v) ? v.u : 0u, age = plat_link_age_ms(now);
    if (!to || age == UINT32_MAX) return LINK_NONE;
    return age > to * 1000u ? LINK_LOST : LINK_OK;
}
static uint32_t q_color(uint8_t q) {
    return q == PLAT_Q_RANGE || q == PLAT_Q_FAULT ? C_BAD : q == PLAT_Q_UNAVAILABLE ? C_GREY : C_WARN;
}
/* A shown value with its quality, centred on cy: when it is not valid the number is greyed
 * ("----" when unavailable) and moves up, with the quality word under it. idx: UW_NO_INDEX
 * (= PLAT_MEAS_NO_INDEX) for a scalar. */
static void qvalue(int16_t x, int16_t cy, int16_t f, uint16_t id, uint16_t idx, const char *v) {
    uint8_t q = param_quality(id, idx);
    if (q == PLAT_Q_VALID) { color(C_INK); text(x, cy, f, OPT_CENTERY, v); return; }
    int16_t hv = f == F_HUGE ? 49 : f == F_BIG ? 36 : 25;          /* the ROM fonts' cell heights */
    int16_t vc = (int16_t)(cy - 11);
    color(C_GREY); text(x, vc, f, OPT_CENTERY, q == PLAT_Q_UNAVAILABLE ? "----" : v);
    color(q_color(q)); text(x, (int16_t)(vc + hv / 2 + 11), f >= F_BIG ? F_TEXT : F_SMALL, OPT_CENTERY, plat_quality_name(q));
}

static void header(uint32_t now) {
    box(0, 0, W, HEAD_H, C_HEAD, 1);
    color(0xFFFFFFu);
    unsigned nf = mb_force_count();
    if (nf) {                                         /* Modbus values forced: shown in place of the name, tap for the list */
        char f[24];
        snprintf(f, sizeof f, "FORCED %u", nf);
        eve_tag(E, T_FORCE);
        box(12, 10, 208, 44, U.pressed == T_FORCE ? C_PRESS : C_WARN, 22);
        color(0xFFFFFFu); text(116, 32, F_SMALL, OPT_CENTER, f);
        eve_tag(E, 0);
    } else text(16, HEAD_H / 2, 29, OPT_CENTERY, U.cfg && U.cfg->product ? U.cfg->product : "CEMS Supervisor");
    uint8_t st = plat_state();
    int16_t x = 236;
    box(x, 14, 176, 36, state_color(st), 18);
    color(0xFFFFFFu);
    text((int16_t)(x + 88), 32, F_SMALL, OPT_CENTER, plat_state_name(st));
    x = (int16_t)(x + 184);
    bool remote = param_ctrl_mode() == PARAM_MODE_REMOTE;
    box(x, 14, 112, 36, remote ? C_OK : 0x5B6577u, 18);
    color(0xFFFFFFu);
    text((int16_t)(x + 56), 32, F_SMALL, OPT_CENTER, remote ? "REMOTE" : "LOCAL");
    x = (int16_t)(x + 120);
    int ls = link_state(now);
    box(x, 14, 124, 36, ls == LINK_OK ? C_OK : ls == LINK_LOST ? C_BAD : 0x5B6577u, 18);
    color(0xFFFFFFu);
    text((int16_t)(x + 62), 32, F_SMALL, OPT_CENTER, ls == LINK_OK ? "LINK OK" : ls == LINK_LOST ? "LINK LOST" : "NO LINK");
    x = (int16_t)(x + 132);
    alarm_summary_t as;
    alarm_summary(&as);
    if (as.shown) {                                   /* one pill for every alarm: tap for the list */
        char a[32];
        if (as.unacked) snprintf(a, sizeof a, "%u ALARM%s  %u NEW", as.active, as.active == 1 ? "" : "S", as.unacked);
        else snprintf(a, sizeof a, "%u ALARM%s", as.active, as.active == 1 ? "" : "S");
        uint32_t c = !as.worst_unacked ? 0x5B6577u : as.worst >= AL_SEV_ALARM ? C_BAD : as.worst == AL_SEV_WARNING ? C_WARN : 0x5B6577u;
        eve_tag(E, T_ALARMS);
        box(x, 10, 196, 44, U.pressed == T_ALARMS ? C_PRESS : c, 22);
        color(0xFFFFFFu); text((int16_t)(x + 98), 32, F_SMALL, OPT_CENTER, a);
        eve_tag(E, 0);
    }
    char t[16];
    snprintf(t, sizeof t, "%02u:%02u:%02u", RTC_Time.Hours, RTC_Time.Minutes, RTC_Time.Seconds);
    color(0xC9D2E3u);
    text(W - 16, HEAD_H / 2, F_TEXT, OPT_CENTERY | OPT_RIGHTX, RTC_Date.Year >= 2000 ? t : "--:--:--");
}

static void nav(void) {
    static const char *const lab[] = { "Home", "Trends", "Settings", "Events", "System" };
    static const uint8_t tag[] = { T_NAV_HOME, T_TRENDS, T_NAV_PARAMS, T_NAV_EVENTS, T_NAV_SYSTEM };
    static const int16_t x[] = { 8, 162, 316, 482, 636 }, w[] = { 148, 148, 160, 148, 148 };
    box(0, NAV_Y - 4, W, NAV_H + 8, 0xE4E7ECu, 1);
    for (int k = 0; k < 5; ++k) {
        bool cur = (k == 0 && (U.scr == UI_HOME || U.scr == UI_APP || U.scr == UI_REC)) ||
                   (k == 1 && (U.scr == UI_TRENDS || U.scr == UI_TR_PICK)) ||
                   (k == 2 && (U.scr == UI_PARAMS || (U.scr == UI_EDIT && !U.cell_f && !U.svc_step) || U.scr == UI_FORCE)) ||
                   (k == 3 && U.scr == UI_EVENTS) ||
                   (k == 4 && (U.scr == UI_SYSTEM || U.scr == UI_IOB || U.scr == UI_CLOCK || U.scr == UI_CAL || U.scr == UI_DISPLAY ||
                               U.scr == UI_SERVICE || (U.scr == UI_EDIT && U.svc_step)));
        button(tag[k], x[k], NAV_Y, w[k], NAV_H, lab[k], cur ? C_HEAD : C_ACCENT, true);
    }
    char b[32];
    if (U.level) snprintf(b, sizeof b, "Log out %s", lvl_name(U.level));
    else snprintf(b, sizeof b, "Log in");
    button(T_NAV_LOGIN, 790, NAV_Y, 226, NAV_H, b, U.level ? C_WARN : C_ACCENT, true);
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
        qvalue((int16_t)(x + 16), (int16_t)(y + 81), F_HUGE, c->home_ids[k], UW_NO_INDEX, v);
    }
    int16_t y = (int16_t)(BODY_Y + (n > 3 ? 280 : n ? 140 : 10));
    box(16, y, 992, 110, C_CARD, 8);
    bool remote = param_ctrl_mode() == PARAM_MODE_REMOTE;
    color(C_INK);
    text(36, (int16_t)(y + 18), F_BIG, 0, remote ? "Control: REMOTE" : "Control: LOCAL");
    color(C_MUTE);
    text(36, (int16_t)(y + 64), F_TEXT, 0, remote ? "Changes here are refused." : "The PLC and web can only read.");
    bool maint = maint_on();
    button(T_MAINT, 476, (int16_t)(y + 23), 236, 64, maint ? "End maintenance" : "Maintenance", maint ? C_WARN : C_ACCENT, true);
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
        qvalue(440, (int16_t)(y + ROW_H / 2), F_TEXT, d->id, UW_NO_INDEX, b);
        color(can ? C_ACCENT : C_GREY);
        text(880, (int16_t)(y + ROW_H / 2), F_SMALL, OPT_CENTERY | OPT_RIGHTX, can ? "Change >" : (d->flags & PF_RO) ? "read only" : (d->flags & PF_DEV) ? "developer" : lvl_name(d->level));
        eve_tag(E, 0);
        if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
    }
    char pg[32];
    snprintf(pg, sizeof pg, "%u-%u of %u", (unsigned)(U.first + 1), (unsigned)(U.first + ROWS < n ? U.first + ROWS : n), (unsigned)n);
    button(T_PGUP, 908, BODY_Y, 100, 130, "Up", C_ACCENT, U.first > 0);
    color(C_MUTE); text(958, BODY_Y + 148, F_SMALL, OPT_CENTER, pg);
    button(T_PGDN, 908, BODY_Y + 166, 100, 130, "Down", C_ACCENT, U.first + ROWS < n);
    button(T_FORCE, 908, BODY_Y + 312, 100, 80, "Force", 0x5B6577u, true);   /* Modbus force (commissioning) */
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
static void fmt_elem(const param_def_t *d, uint16_t idx, char *b, size_t cap);
static void draw_edit(void) {
    if (U.cell_f) { draw_cell_edit(); return; }
    if (U.svc_step) {                                    /* service log entry: no parameter behind it */
        char b[64];
        box(16, BODY_Y, 760, 88, C_CARD, 8);
        color(C_INK); text(32, BODY_Y + 10, F_BIG, 0, U.svc_step == 1 ? "Service log: who did the work" : "Service log: the work done");
        if (U.svc_step == 1) snprintf(b, sizeof b, "Up to 15 characters, for example a name or company");
        else snprintf(b, sizeof b, "By %.15s. Up to 39 characters", U.svc_tech);
        color(C_MUTE); text(32, BODY_Y + 56, F_TEXT, 0, b);
        box(16, BODY_Y + 96, 760, 48, C_CARD, 6);
        snprintf(b, sizeof b, "%.60s_", U.entry);
        color(C_INK); text(28, BODY_Y + 120, F_BIG, OPT_CENTERY, b);
        draw_keyboard(BODY_Y + 150, true);
        button(T_OK, 784, BODY_Y, 108, 144, U.svc_step == 1 ? "Next" : "Save", C_OK, true);
        button(T_CANCEL, 900, BODY_Y, 108, 144, "Cancel", 0x5B6577u, true);
        return;
    }
    const param_def_t *d = U.ed;
    if (!d) return;
    char b[96], r[64];
    int16_t cw = U.kb == KB_TEXT ? 760 : 992;            /* the text keyboard's OK/Cancel stand beside it */
    box(16, BODY_Y, cw, 88, C_CARD, 8);
    color(C_INK);
    if (U.elem) snprintf(b, sizeof b, "%s", U.el_title);
    else if (U.forcing && U.fc_index != MB_FORCE_SCALAR) snprintf(b, sizeof b, "Force %s[%u]", d->name, (unsigned)U.fc_index);
    else snprintf(b, sizeof b, U.forcing ? "Force %s  (%u)" : "%s  (%u)", d->name, d->id);
    text(32, BODY_Y + 10, F_BIG, 0, b);
    if (U.elem) fmt_elem(d, U.el_idx, r, sizeof r);
    else if (U.forcing && U.fc_index != MB_FORCE_SCALAR) fmt_elem(d, U.fc_index, r, sizeof r);
    else fmt_value(d, r, sizeof r);
    snprintf(b, sizeof b, "Now: %s", r);
    color(C_MUTE);
    text((int16_t)(cw), BODY_Y + 16, F_TEXT, OPT_RIGHTX, b);
    if (d->ptype == PT_INT) snprintf(r, sizeof r, "Range %ld .. %ld", (long)d->vmin.i, (long)d->vmax.i);
    else if (d->ptype == PT_UINT && !is_ip(d)) snprintf(r, sizeof r, "Range %lu .. %lu", (unsigned long)d->vmin.u, (unsigned long)d->vmax.u);
    else if (d->ptype == PT_FLOAT) { char a[24], z[24]; fmt_float(a, sizeof a, (double)d->vmin.f); fmt_float(z, sizeof z, (double)d->vmax.f); snprintf(r, sizeof r, "Range %s .. %s", a, z); }
    else if (PARAM_IS_BLOB(d)) snprintf(r, sizeof r, "Up to %u characters", (unsigned)(d->ptype == PT_TEXT16 ? d->size / 2u - 1u : d->size - 1u));
    else r[0] = 0;
    if (d->unit && r[0]) { strncat(r, " ", sizeof r - strlen(r) - 1); strncat(r, d->unit, sizeof r - strlen(r) - 1); }
    if (U.forcing) snprintf(r, sizeof r, "Modbus reads this for %u s, then the real value again", (unsigned)(PLAT_MB_FORCE_MS / 1000u));
    if (U.elem) {
        snprintf(r, sizeof r, "Range %lu .. %lu", (unsigned long)d->vmin.u, (unsigned long)d->vmax.u);
        if (d->id == PARAM_ID_DIA_INPUT) strncat(r, " (0 = not used)", sizeof r - strlen(r) - 1);
        else if (d->unit) { strncat(r, " ", sizeof r - strlen(r) - 1); strncat(r, d->unit, sizeof r - strlen(r) - 1); }
    }
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
    if (U.kb == KB_TEXT) {                               /* beside the narrowed card and the entry: 144 px tall */
        button(T_OK, bx, BODY_Y, 108, 144, "OK", C_OK, true); button(T_CANCEL, (int16_t)(bx + 116), BODY_Y, 108, 144, "Cancel", 0x5B6577u, true);
    }
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
    snprintf(b, sizeof b, "%.15s, core %s, build %.11s", st.running_version, PLAT_CORE_VERSION, fw_build_id());   /* R4: core version beside it */
    kv(32, BODY_Y + 44, "Running", b);
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
    {   /* CIP-11.1: the start-up self-test; failed checks by name */
        unsigned nst = plat_selftest_count(), skip = 0;
        size_t o = 0; b[0] = 0;
        for (unsigned i = 0; i < nst; ++i) {
            const char *nm; uint8_t r;
            if (!plat_selftest_get(i, &nm, &r)) continue;
            if (r == PLAT_STR_SKIP) ++skip;
            else if (r == PLAT_STR_FAIL && o < sizeof b - 2u)
                o += (size_t)snprintf(b + o, sizeof b - o, "%s%s", o ? ", " : "failed: ", nm);
        }
        if (!plat_selftest_done()) { snprintf(b, sizeof b, "running"); o = 0; }
        else if (!o) snprintf(b, sizeof b, "passed (%u checks, %u skipped)", nst - skip, skip);
        color(C_MUTE); text(32, BODY_Y + 128, F_SMALL, 0, "Self-test");
        color(o ? C_BAD : C_INK); text(202, BODY_Y + 128, F_SMALL, 0, b);
    }
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
    color(C_INK); text(32, BODY_Y + 282, F_TEXT, OPT_CENTERY, "Screen brightness");
    snprintf(b, sizeof b, "%u %%", U.bright);
    button(T_BRIGHT_DN, 252, BODY_Y + 254, 72, 56, "-", C_ACCENT, true);
    color(C_INK); text(372, BODY_Y + 282, F_BIG, OPT_CENTER, b);
    button(T_BRIGHT_UP, 420, BODY_Y + 254, 72, 56, "+", C_ACCENT, true);
    button(T_EMIS, 32, BODY_Y + 316, 148, 56, "Emissions", C_ACCENT, true);     /* the emissions engine's results (emis.h) */
    button(T_INST, 188, BODY_Y + 316, 148, 56, "Instruments", C_ACCENT, true);  /* third-party analysers (inst.h) */
    button(T_SVC, 344, BODY_Y + 316, 148, 56, "Service", 0x5B6577u, true);      /* self-test, board test, service log, display test */
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
    {   unsigned long sn = param_read(PARAM_ID_SERIAL, &v) ? v.u : 0, hw = param_read(PARAM_ID_HW_REV, &v) ? v.u : 0;
        int br = fw_board_rev();                     /* -1 = no revision straps on this board */
        if (br >= 0) snprintf(b, sizeof b, "%lu, hardware %lu, board rev %d", sn, hw, br);
        else snprintf(b, sizeof b, "%lu, hardware %lu", sn, hw);
        kv(544, BODY_Y + 44, "Serial", b); }
    {   uint32_t h = plat_service_hours_x10(), sv = plat_service_since_x10();   /* CIP-05: operating hours */
        snprintf(b, sizeof b, "%lu.%lu total, %lu.%lu since service", (unsigned long)(h / 10u), (unsigned long)(h % 10u),
                 (unsigned long)(sv / 10u), (unsigned long)(sv % 10u));
        kv(544, BODY_Y + 70, "Hours", b); }
    snprintf(b, sizeof b, "%lu h %lu min", (unsigned long)(now / 3600000u), (unsigned long)((now / 60000u) % 60u)); kv(544, BODY_Y + 96, "Up", b);
    snprintf(b, sizeof b, "%lu%s%.24s", (unsigned long)plat_diag.boots, plat_diag.fault ? ", last: " : "", plat_diag.fault ? plat_fault_text(plat_diag.fault) : "");
    kv(544, BODY_Y + 122, "Restarts", b);
    uint32_t mv = plat_supply_mv();                  /* CIP-07.3: 0 = not measured on this board */
    if (mv) snprintf(b, sizeof b, "%lu.%lu V", (unsigned long)(mv / 1000u), (unsigned long)((mv % 1000u) / 100u));
    kv(544, BODY_Y + 148, "Supply", mv ? b : "not measured");
    color(C_INK); text(544, BODY_Y + 172, F_TEXT, 0, "Network");
    uint32_t ip = 0; bool link = false; uint8_t mac[6] = { 0 };
    bool have = ui_net_status(&ip, &link, mac);
    if (!have && param_read(PARAM_ID_NET_IP, &v)) ip = v.u;
    fmt_ip(c, sizeof c, ip);
    if (plat_net_status_word & NETST_CONFLICT) snprintf(b, sizeof b, "%s IN USE by another device", c);
    else if (plat_net_status_word & NETST_PROBING) snprintf(b, sizeof b, "%s (checking...)", c);
    else snprintf(b, sizeof b, "%s%s", c, have ? (link ? "  (link up)" : "  (no link)") : "");
    kv(544, BODY_Y + 202, "Address", b);
    if (have) { snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); kv(544, BODY_Y + 226, "MAC", b); }
    color(C_INK); text(544, BODY_Y + 250, F_TEXT, 0, "Storage");
    kv(544, BODY_Y + 280, "Calibration", param_store_calibration_valid() ? "valid" : "INVALID");
    uint32_t ls = plat_log_status();
    snprintf(b, sizeof b, "%s%s%s", ls & LSTAT_NOR_FAULT ? "FAULT " : "", ls & LSTAT_VOLATILE ? "RAM only (no flash) " : "",
             (ls & (LSTAT_NOR_FAULT | LSTAT_VOLATILE)) ? "" : "ok");
    snprintf(b + strlen(b), sizeof b - strlen(b), ", store 0x%04lx", (unsigned long)param_store_status());
    kv(544, BODY_Y + 302, "Log, store", b);
    snprintf(b, sizeof b, "%08lX", (unsigned long)plat_config_crc());
    kv(544, BODY_Y + 324, "Config", b);                 /* CIP-05.4: configuration checksum, 8 hex digits */
    if (plat_health.valid) {
        unsigned lo = 0xFFFFu; const char *ln = "";
        for (unsigned i = 0; i < plat_health.n_tasks && i < PLAT_HEALTH_TASKS; ++i)
            if (plat_health.task[i].free_words < lo) { lo = plat_health.task[i].free_words; ln = plat_health.task[i].name; }
        snprintf(b, sizeof b, "heap %lu KB free at worst, stack %.11s %u words", (unsigned long)(plat_health.heap_min / 1024u), ln, lo);
        kv(544, BODY_Y + 368, "Memory", b);
    }
    uint32_t ct;
    if (plat_clock_now(&ct))                          /* day first, no seconds (the header has them): clears the buttons */
        snprintf(b, sizeof b, "%02u/%02u/%04u %02u:%02u", RTC_Date.Day_In_Month, RTC_Date.Month, RTC_Date.Year, RTC_Time.Hours, RTC_Time.Minutes);
    else snprintf(b, sizeof b, "NOT SET");
    kv(544, BODY_Y + 346, plat_clock_synced() ? "Clock" : "Clock, not synced", b);   /* CIP-02.6 */
    button(T_CLOCK, 884, BODY_Y + 318, 116, 64, "Set clock", C_ACCENT, true);
    button(T_IOB, 884, BODY_Y + 244, 116, 64, "I/O boards", C_ACCENT, true);
    if (plat_cal_count()) button(T_CAL, 884, BODY_Y + 40, 116, 64, plat_cal_any_due() ? "Cal DUE" : "Calibrate", plat_cal_any_due() ? C_WARN : C_ACCENT, true);
}

/* ---------------- display test (bench bring-up, HMI_BENCH_TEST.md) ----------------
 * Status page: what the BT817 reports and what the driver has seen, with the refresh rate and
 * system clock measured from REG_FRAMES and REG_CLOCK. "Colour test": full-screen patterns, a
 * tap anywhere for the next. "Touch test": five targets; the trail of points touched and each
 * touch-down's offset from the nearest target, to check the touch lines up with the picture. */
static const int16_t dt_tx[5] = { 40, W - 40, 40, W - 40, W / 2 }, dt_ty[5] = { 40, 40, 560, 560, 300 };
static const char *const dt_pat_name[DT_PATTERNS] = { "Colour bars and grey ramp", "White", "Black", "Red", "Green", "Blue", "Grid, 50 px" };

static void dt_cross(int16_t x, int16_t y, uint32_t c) {
    color(c);
    eve_line(E, (int16_t)(x - 24), y, (int16_t)(x + 24), y, 2);
    eve_line(E, x, (int16_t)(y - 24), x, (int16_t)(y + 24), 2);
}

static void draw_display_patterns(void) {
    eve_tag(E, T_DT_NEXT);
    switch (U.dt_pat) {
    case 0: {
        static const uint32_t bars[8] = { 0xFFFFFFu, 0xFFFF00u, 0x00FFFFu, 0x00FF00u, 0xFF00FFu, 0xFF0000u, 0x0000FFu, 0x000000u };
        for (int k = 0; k < 8; ++k) box((int16_t)(k * 128), 0, 128, 400, bars[k], 0);
        for (int k = 0; k < 32; ++k) { uint32_t g = (uint32_t)(k * 255 / 31); box((int16_t)(k * 32), 400, 32, 200, g << 16 | g << 8 | g, 0); }
        break; }
    case 1: box(0, 0, W, 600, 0xFFFFFFu, 0); break;
    case 2: box(0, 0, W, 600, 0x000000u, 0); break;
    case 3: box(0, 0, W, 600, 0xFF0000u, 0); break;
    case 4: box(0, 0, W, 600, 0x00FF00u, 0); break;
    case 5: box(0, 0, W, 600, 0x0000FFu, 0); break;
    default:
        box(0, 0, W, 600, 0x000000u, 0);
        color(0xFFFFFFu);
        for (int16_t x = 0; x < W; x = (int16_t)(x + 50)) eve_line(E, x, 0, x, 599, 1);
        for (int16_t y = 0; y < 600; y = (int16_t)(y + 50)) eve_line(E, 0, y, W - 1, y, 1);
        eve_line(E, W - 1, 0, W - 1, 599, 1); eve_line(E, 0, 599, W - 1, 599, 1);   /* the last pixel on every edge */
        break;
    }
    char b[64];
    snprintf(b, sizeof b, "%u of %u: %s. Tap for the next", (unsigned)U.dt_pat + 1u, (unsigned)DT_PATTERNS, dt_pat_name[U.dt_pat]);
    color(U.dt_pat == 1 || U.dt_pat == 4 ? 0x000000u : 0xFFFFFFu);
    if (U.dt_pat != 0) text(W / 2, 560, F_SMALL, OPT_CENTERX, b);
    eve_tag(E, 0);
}

static void draw_display_touch(void) {
    eve_tag(E, 255);                                  /* the glass itself: no tap, just points */
    box(0, 0, W, 600, 0xFFFFFFu, 0);
    eve_tag(E, 0);
    int16_t x, y;
    bool down = eve_touch_xy(E, &x, &y);
    if (down) {
        if (U.dt_n == DT_TRAIL) { memmove(U.dt_x, U.dt_x + 1, sizeof U.dt_x - sizeof U.dt_x[0]); memmove(U.dt_y, U.dt_y + 1, sizeof U.dt_y - sizeof U.dt_y[0]); U.dt_n--; }
        U.dt_x[U.dt_n] = x; U.dt_y[U.dt_n] = y; U.dt_n++;
        if (!U.dt_down) {                             /* touch-down: offset from the nearest target */
            int32_t best = INT32_MAX;
            for (uint8_t k = 0; k < 5; ++k) {
                int32_t dx = x - dt_tx[k], dy = y - dt_ty[k], d = dx * dx + dy * dy;
                if (d < best) { best = d; U.dt_tgt = (uint8_t)(k + 1); U.dt_dx = (int16_t)dx; U.dt_dy = (int16_t)dy; }
            }
        }
    }
    U.dt_down = down;
    U.dirty = true;                                   /* follow the finger at the task's rate */
    for (int k = 0; k < 5; ++k) dt_cross(dt_tx[k], dt_ty[k], U.dt_tgt == k + 1 ? C_ACCENT : C_INK);
    color(C_BAD);
    for (unsigned i = 0; i < U.dt_n; ++i) eve_rect(E, (int16_t)(U.dt_x[i] - 2), (int16_t)(U.dt_y[i] - 2), 5, 5, 1);
    char b[96];
    color(C_INK);
    text(W / 2, 150, F_TEXT, OPT_CENTERX, "Touch each cross, then draw along the edges");
    if (down) snprintf(b, sizeof b, "Finger at %d, %d", x, y);
    else snprintf(b, sizeof b, "No finger on the glass");
    text(W / 2, 190, F_SMALL, OPT_CENTERX, b);
    if (U.dt_tgt) {
        int16_t ax = (int16_t)(U.dt_dx < 0 ? -U.dt_dx : U.dt_dx), ay = (int16_t)(U.dt_dy < 0 ? -U.dt_dy : U.dt_dy);
        snprintf(b, sizeof b, "Last touch: %d px %s, %d px %s of the cross", ax, U.dt_dx < 0 ? "left" : "right", ay, U.dt_dy < 0 ? "above" : "below");
        color(ax > 10 || ay > 10 ? C_BAD : C_OK);     /* about 1.5 mm at 6.64 px/mm */
        text(W / 2, 220, F_SMALL, OPT_CENTERX, b);
    }
    button(T_DT_CLEAR, 332, 420, 160, 64, "Clear", 0x5B6577u, true);
    button(T_DT_DONE, 532, 420, 160, 64, "Done", C_ACCENT, true);
}

static void draw_display(uint32_t now) {
    if (U.dt_mode == 1) { draw_display_patterns(); return; }
    if (U.dt_mode == 2) { draw_display_touch(); return; }
    char b[96];
    uint32_t frames = eve_rd32(E, REG_FRAMES), clk = eve_rd32(E, REG_CLOCK_DT);
    if (!U.dt_t0) { U.dt_t0 = now ? now : 1u; U.dt_frames0 = frames; U.dt_clock0 = clk; }
    else if (now - U.dt_t0 >= 1000u) {                /* rates over the last second or more */
        uint32_t ms = now - U.dt_t0;
        U.dt_hz10 = (uint32_t)((uint64_t)(frames - U.dt_frames0) * 10000u / ms);
        U.dt_mhz10 = (uint32_t)((uint64_t)(clk - U.dt_clock0) / 100u / ms);
        U.dt_t0 = now; U.dt_frames0 = frames; U.dt_clock0 = clk;
    }
    box(16, BODY_Y, 640, 392, C_CARD, 8);
    color(C_INK); text(32, BODY_Y + 10, F_TEXT, 0, "Display");
    uint8_t id = eve_rd8(E, REG_ID);
    uint32_t chip = eve_rd32(E, 0x0C0000u);           /* BT817: 08 17 01 00 (RAM_G chip ID, read after boot) */
    snprintf(b, sizeof b, "REG_ID 0x%02X%s, chip %02X%02X", id, id == EVE_CHIP_ID ? " (answering)" : " (WRONG)", (unsigned)(chip & 0xFFu), (unsigned)((chip >> 8) & 0xFFu));
    kv(32, BODY_Y + 44, "Controller", b);
    snprintf(b, sizeof b, "%lu frames, %lu faults, %lu timeouts", (unsigned long)E->frames, (unsigned long)E->faults, (unsigned long)E->timeouts);
    kv(32, BODY_Y + 70, "Driver", b);
    kv(32, BODY_Y + 96, "Last fault", E->fault_msg[0] ? E->fault_msg : "none");
    if (U.dt_hz10) snprintf(b, sizeof b, "%lu.%lu Hz", (unsigned long)(U.dt_hz10 / 10u), (unsigned long)(U.dt_hz10 % 10u));
    else snprintf(b, sizeof b, "measuring...");
    kv(32, BODY_Y + 122, "Refresh", b);
    if (U.dt_mhz10) snprintf(b, sizeof b, "%lu.%lu MHz measured, %lu MHz set", (unsigned long)(U.dt_mhz10 / 10u), (unsigned long)(U.dt_mhz10 % 10u), (unsigned long)(eve_rd32(E, REG_FREQUENCY) / 1000000u));
    else snprintf(b, sizeof b, "%lu MHz set, measuring...", (unsigned long)(eve_rd32(E, REG_FREQUENCY) / 1000000u));
    kv(32, BODY_Y + 148, "System clock", b);
    snprintf(b, sizeof b, "PCLK %lu, PCLK_FREQ 0x%03lX, 2X %lu", (unsigned long)eve_rd8(E, REG_PCLK), (unsigned long)eve_rd16(E, REG_PCLK_FREQ), (unsigned long)eve_rd8(E, REG_PCLK_2X));
    kv(32, BODY_Y + 174, "Pixel clock", b);
    snprintf(b, sizeof b, "%lu x %lu", (unsigned long)eve_rd16(E, REG_HSIZE), (unsigned long)eve_rd16(E, REG_VSIZE));
    kv(32, BODY_Y + 200, "Size", b);
    unsigned nf = 0;
    for (unsigned i = 0; i < EVE_FONTS; ++i) nf += (E->fonts_ok >> i) & 1u;
    snprintf(b, sizeof b, "%u of %u own fonts loaded%s", nf, (unsigned)EVE_FONTS, nf == EVE_FONTS ? "" : " (ROM fonts used)");
    kv(32, BODY_Y + 226, "Fonts", b);
    int16_t tx, ty;
    bool td = eve_touch_xy(E, &tx, &ty);
    if (td) snprintf(b, sizeof b, "config 0x%04X, finger at %d, %d", (unsigned)eve_rd16(E, REG_TOUCH_CONFIG), tx, ty);
    else snprintf(b, sizeof b, "config 0x%04X, mode %u", (unsigned)eve_rd16(E, REG_TOUCH_CONFIG), (unsigned)eve_rd8(E, REG_TOUCH_MODE));
    kv(32, BODY_Y + 252, "Touch", b);
    snprintf(b, sizeof b, "%u %%, PWM duty %u of 128", (unsigned)U.bright, (unsigned)eve_rd8(E, REG_PWM_DUTY));
    kv(32, BODY_Y + 278, "Backlight", b);
    color(C_MUTE);
    text(32, BODY_Y + 320, F_SMALL, 0, "Use this page for the bench checks in docs/HMI_BENCH_TEST.md.");
    text(32, BODY_Y + 344, F_SMALL, 0, "Brightness is set on the System page.");
    button(T_DT_COLOURS, 672, BODY_Y, 336, 120, "Colour test", C_ACCENT, true);
    button(T_DT_TOUCH, 672, BODY_Y + 136, 336, 120, "Touch test", C_ACCENT, true);
    button(T_BACK, 672, BODY_Y + 272, 336, 120, "Back", 0x5B6577u, true);
}

/* I/O boards (iobus.h): one card per board, outputs and inputs as lit squares, faults in words */
/* dd/mm/yyyy, or "clock not set" (UI_CONVENTIONS 5) */
static void fmt_day(char *b, size_t cap, uint32_t t) {
    if (!t) { snprintf(b, cap, "clock not set"); return; }
    unsigned y, mo, d, h, mi, s;
    plat_civil_from_unix(t, &y, &mo, &d, &h, &mi, &s);
    snprintf(b, cap, "%02u/%02u/%04u", d % 100u, mo % 100u, y % 10000u);
}
static const char *cal_kind(uint8_t k) { return k == PLAT_CAL_FACTORY ? "factory" : k == PLAT_CAL_FIELD ? "field" : "-"; }
static uint32_t cal_outcome_color(uint8_t o) { return o == PLAT_CAL_PASS || o == PLAT_CAL_REVERTED ? C_OK : o == PLAT_CAL_FAIL ? C_BAD : C_WARN; }
static void fmt_f(char *b, size_t cap, float v) {
    if (v != v) { snprintf(b, cap, "-"); return; }                  /* NaN: not taken */
    fmt_float(b, cap, (double)v);
}

/* Calibration (CIP-18): channels with the record in force and due date; the selected channel's
 * points (reference, as found, as left); the history; start, accept, abort, revert */
static void draw_cal(uint32_t now) {
    char b[96], c[24];
    size_t n = plat_cal_count();
    if (U.cal_ch >= n) U.cal_ch = 0;
    unsigned act_ch = 0;
    bool active = plat_cal_active(&act_ch);
    uint32_t today = 0; bool clk = plat_clock_now(&today);
    /* channels */
    box(16, BODY_Y, 480, 248, C_CARD, 8);
    color(C_INK); text(32, BODY_Y + 10, F_TEXT, 0, "Channels");
    for (size_t k = 0; k < n && k < 4; ++k) {
        int16_t y = (int16_t)(BODY_Y + 44 + k * 50);
        uint8_t tg = (uint8_t)(T_ROW0 + k);
        eve_tag(E, tg);
        box(24, y, 464, 46, k == U.cal_ch ? 0xDCE6F5u : C_CARD, 6);
        color(C_INK); text(36, (int16_t)(y + 23), F_TEXT, OPT_CENTERY, plat_cal_name((unsigned)k));
        plat_cal_record_t r;
        uint32_t due = plat_cal_due((unsigned)k);
        if (active && act_ch == k) { color(C_WARN); snprintf(b, sizeof b, "calibrating now"); }
        else if (!plat_cal_in_force((unsigned)k, &r)) { color(C_BAD); snprintf(b, sizeof b, "never calibrated"); }
        else if (due && clk && today >= due) { fmt_day(c, sizeof c, due); color(C_WARN); snprintf(b, sizeof b, "DUE since %s", c); }
        else if (due) { fmt_day(c, sizeof c, due); color(C_MUTE); snprintf(b, sizeof b, "due %s", c); }
        else { fmt_day(c, sizeof c, r.time); color(C_MUTE); snprintf(b, sizeof b, "%s, %s", cal_kind(r.kind), c); }
        text(480, (int16_t)(y + 23), F_SMALL, OPT_CENTERY | OPT_RIGHTX, b);
        eve_tag(E, 0);
    }
    if (n > 4) { color(C_MUTE); snprintf(b, sizeof b, "+%u more on the web page", (unsigned)(n - 4)); text(32, BODY_Y + 226, F_SMALL, 0, b); }
    /* history */
    box(16, BODY_Y + 256, 480, 184, C_CARD, 8);
    color(C_INK); text(32, BODY_Y + 266, F_TEXT, 0, "History");
    plat_cal_record_t h;
    unsigned shown = 0;
    for (unsigned i = 0; shown < 6 && plat_cal_history(i, &h); ++i, ++shown) {
        int16_t y = (int16_t)(BODY_Y + 300 + shown * 22);
        fmt_day(c, sizeof c, h.time);
        snprintf(b, sizeof b, "%s  %.12s  %s  %s", c, plat_cal_name(h.channel), cal_kind(h.kind), lvl_name(h.level));
        color(C_INK); text(32, y, F_SMALL, 0, b);
        color(cal_outcome_color(h.outcome)); text(480, y, F_SMALL, OPT_RIGHTX, plat_cal_outcome_name(h.outcome));
    }
    if (!shown) { color(C_MUTE); text(32, BODY_Y + 300, F_SMALL, 0, "No calibrations yet"); }
    /* the selected channel */
    box(508, BODY_Y, 500, 440, C_CARD, 8);
    if (!n) return;
    color(C_INK); text(524, BODY_Y + 10, F_BIG, 0, plat_cal_name(U.cal_ch));
    plat_cal_record_t r;
    bool have = plat_cal_in_force(U.cal_ch, &r);
    if (have) {
        fmt_day(c, sizeof c, r.time);
        snprintf(b, sizeof b, "In force: %s, %s, %s", cal_kind(r.kind), c, plat_cal_outcome_name(r.outcome));
    } else snprintf(b, sizeof b, "No calibration in force");
    color(have ? C_MUTE : C_BAD); text(524, BODY_Y + 50, F_SMALL, 0, b);
    plat_cal_record_t f;
    if (plat_cal_factory(U.cal_ch, &f)) { fmt_day(c, sizeof c, f.time); snprintf(b, sizeof b, "Factory: %s", c); }
    else snprintf(b, sizeof b, "Factory: none");
    color(C_MUTE); text(524, BODY_Y + 72, F_SMALL, 0, b);
    if (have && r.n_pts) {                                /* reference points of the record in force */
        static const int16_t cx[] = { 524, 640, 770, 900 };
        static const char *const hd[] = { "Point", "Reference", "As found", "As left" };
        color(C_MUTE);
        for (int k = 0; k < 4; ++k) text(cx[k], BODY_Y + 104, F_SMALL, 0, hd[k]);
        for (unsigned i = 0; i < r.n_pts && i < PLAT_CAL_PTS_MAX; ++i) {
            int16_t y = (int16_t)(BODY_Y + 128 + i * 24);
            color(C_INK);
            snprintf(b, sizeof b, "%u", i + 1u); text(cx[0], y, F_SMALL, 0, b);
            fmt_f(b, sizeof b, r.ref[i]); text(cx[1], y, F_SMALL, 0, b);
            fmt_f(b, sizeof b, r.found[i]); text(cx[2], y, F_SMALL, 0, b);
            fmt_f(b, sizeof b, r.left[i]); text(cx[3], y, F_SMALL, 0, b);
        }
    }
    /* actions: what is refused shows greyed with the reason under it */
    int16_t by = BODY_Y + 296;
    if (active && act_ch == U.cal_ch) {
        color(C_WARN); text(524, (int16_t)(by - 26), F_SMALL, 0, "Calibrating: the procedure runs, then accept or abort");
        button(T_CAL_ACCEPT, 524, by, 228, 64, "Accept", C_OK, plat_cal_check(PLAT_CAL_CMD_ACCEPT, U.cal_ch, U.level) == PERR_OK);
        button(T_CAL_ABORT, 764, by, 228, 64, "Abort", 0x5B6577u, true);
    } else {
        bool can = plat_cal_check(PLAT_CAL_CMD_BEGIN, U.cal_ch, U.level) == PERR_OK;
        const char *why = can ? NULL : plat_cal_refusal();
        char wb[64]; snprintf(wb, sizeof wb, "%s", why ? why : "");
        bool rv = plat_cal_check(PLAT_CAL_CMD_REVERT, U.cal_ch, U.level) == PERR_OK;
        eve_tag(E, T_CAL_START);                         /* tappable even when refused, so the reason toasts */
        eve_fgcolor(E, U.pressed == T_CAL_START ? C_PRESS : can ? C_ACCENT : C_GREY);
        color(0xFFFFFFu); ebutton(524, by, 228, 64, F_TEXT, OPT_FLAT, "Start calibration");
        eve_tag(E, T_CAL_REVERT);
        eve_fgcolor(E, U.pressed == T_CAL_REVERT ? C_PRESS : rv ? 0x5B6577u : C_GREY);
        color(0xFFFFFFu); ebutton(764, by, 228, 64, F_TEXT, OPT_FLAT, "Revert to factory");
        eve_tag(E, 0);
        if (why) { color(C_MUTE); text(524, (int16_t)(by + 74), F_SMALL, 0, wb); }
    }
    (void)now;
}

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

/* "what to check" for an alarm (alarm_def_t.help), or NULL */
static const char *alarm_help(uint16_t id) {
    const alarm_def_t *d;
    for (size_t i = 0; (d = alarm_def_at(i)) != NULL; ++i)
        if (d->id == id) return d->help;
    return NULL;
}

static void draw_alarms(void) {
    alarm_view_t v[ROWS];
    alarm_summary_t as;
    alarm_summary(&as);
    if (U.al_first >= as.shown) U.al_first = as.shown > ROWS ? (as.shown - 1u) / ROWS * ROWS : 0;
    size_t n = alarm_list(v, U.al_first, ROWS);
    box(16, BODY_Y, 880, ROWS * ROW_H + 8, C_CARD, 8);
    memset(U.al_ids, 0, sizeof U.al_ids);
    memset(U.al_rows, 0, sizeof U.al_rows);
    for (size_t r = 0; r < n; ++r) {
        const alarm_view_t *a = &v[r];
        int16_t y = (int16_t)(BODY_Y + 4 + (int)r * ROW_H);
        const char *help = alarm_help(a->id);
        bool sel = help && U.al_sel == a->id;
        if (help) {                                   /* the row is tappable: it swaps the status line for the help */
            U.al_rows[r] = a->id;
            eve_tag(E, (uint8_t)(T_AL_ROW0 + r));
            box(20, (int16_t)(y + 2), 696, ROW_H - 4, sel ? 0xEEF3FAu : C_CARD, 4);
        }
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
        snprintf(line, sizeof line, "%.16s  -  %s  -  %s%s", alarm_prio_name(a->priority), t,
                 a->active ? (a->unacked ? "active, not acknowledged" : "active") : "cleared, not acknowledged",
                 a->active && a->unacked && (a->flags & AL_LATCH) ? " (latched)" : "");
        color(C_INK); text(44, (int16_t)(y + 18), F_TEXT, OPT_CENTERY, a->text);
        color(a->unacked ? sev_colour(a->severity) : C_MUTE);
        text(44, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, line);
        eve_tag(E, 0);
        if (a->unacked) {
            U.al_ids[r] = a->id;
            button((uint8_t)(T_ROW0 + r), 724, (int16_t)(y + 4), 164, ROW_H - 8, "Acknowledge", C_ACCENT, true);
        }
    }
    if (!n) { color(C_MUTE); text(456, BODY_Y + 200, F_TEXT, OPT_CENTER, "No alarms"); }
    /* the tapped alarm's "what to check", in a panel over two rows away from it, wrapped */
    for (size_t r = 0; r < n; ++r) {
        const char *help = U.al_rows[r] && U.al_rows[r] == U.al_sel ? alarm_help(U.al_sel) : NULL;
        if (!help) continue;
        int16_t py = (int16_t)(BODY_Y + 4 + (r < ROWS - 2 ? (ROWS - 2) * ROW_H : 0));
        box(20, py, 872, 2 * ROW_H, 0xEEF3FAu, 6);
        box(20, py, 6, 2 * ROW_H, sev_colour(v[r].severity), 2);
        color(C_INK); text(40, (int16_t)(py + 8), F_SMALL, 0, "What to check");
        const char *h = help;
        for (int16_t y = (int16_t)(py + 34); *h && y < py + 2 * ROW_H - 16; y = (int16_t)(y + 22)) {
            char ln[96]; size_t k = strlen(h);
            if (k > 88) { k = 88; while (k > 40 && h[k] != ' ') k--; }
            memcpy(ln, h, k); ln[k] = 0;
            color(C_INK); text(40, y, F_SMALL, 0, ln);
            h += k; while (*h == ' ') h++;
        }
        break;
    }
    button(T_PGUP, 908, BODY_Y, 100, 76, "Up", C_ACCENT, U.al_first > 0);
    button(T_ACK_ALL, 908, BODY_Y + 82, 100, 76, "Ack all", C_WARN, as.unacked > 0);
    button(T_PGDN, 908, BODY_Y + 164, 100, 76, "Down", C_ACCENT, U.al_first + ROWS < as.shown);
    button(T_ACTIONS, 908, BODY_Y + 246, 100, 70, "Actions", 0x5B6577u, true);
    button(T_DIA, 908, BODY_Y + 322, 100, 70, "Inputs", 0x5B6577u, true);   /* digital input alarms */
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
        eve_tag(E, (uint8_t)(T_ROW0 + r));            /* the name: tap to go back to the tier's actions */
        box(20, (int16_t)(y + 2), 274, ROW_H - 4, U.pressed == T_ROW0 + r ? 0xDCE6F5u : C_CARD, 4);
        alarm_view_t v = { .severity = d->severity };
        char d7[64]; size_t k = 0;
        if (alarm_get(d->id, &v))
            for (const unsigned char *p = (const unsigned char *)v.text; *p && k + 1 < sizeof d7; ++p) {
                d7[k++] = (char)*p;                    /* UTF-8 kept: text() shows what the font has */
            }
        d7[k] = 0;
        if (k > 30) {                                  /* the column holds ~30 characters: cut at a word */
            size_t c = 27;
            while (c > 20 && d7[c] != ' ') c--;
            strcpy(d7 + c, "...");
        }
        uint8_t pr = alarm_prio(d->id);
        box(24, (int16_t)(y + 8), 8, ROW_H - 16, pr == AL_PRIO_OFF ? C_GREY : sev_colour(v.severity), 2);
        color(C_INK); text(40, (int16_t)(y + 20), F_SMALL, OPT_CENTERY, d7);
        {
            char nm[48];
            snprintf(nm, sizeof nm, "%.20s%s", d->name, pv(PARAM_ID_ALARM_ROUTE, d->id, 0) & AR_SET ? ", own actions" : "");
            color(C_MUTE); text(40, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, nm);
        }
        eve_tag(E, 0);
        {   /* the tier: a tap moves to the next; "(default)" when it is the definition's own */
            char tl[32];
            uint8_t tg = (uint8_t)(T_PRIO0 + r);
            snprintf(tl, sizeof tl, "%.16s%s", alarm_prio_name(pr), pv(PARAM_ID_ALARM_PRIO, d->id, 0) ? "" : "*");
            eve_tag(E, can ? tg : 0);
            eve_fgcolor(E, U.pressed == tg ? C_PRESS : pr == AL_PRIO_OFF ? 0xB8C0CCu : 0x5B6577u);
            color(0xFFFFFFu);
            ebutton(300, (int16_t)(y + 6), 132, ROW_H - 12, F_SMALL, OPT_FLAT, tl);
            eve_tag(E, 0);
        }
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
    button(T_PGUP, 908, BODY_Y, 100, 92, "Up", C_ACCENT, U.cfg_first > 0);
    button(T_PGDN, 908, BODY_Y + 100, 100, 92, "Down", C_ACCENT, U.cfg_first + ROWS < n);
    button(T_TIERS, 908, BODY_Y + 200, 100, 92, "Tiers", 0x5B6577u, true);
    button(T_BACK, 908, BODY_Y + 300, 100, 92, "Back", 0x5B6577u, true);
    if (!can) { color(C_MUTE); text(456, TOAST_Y + 22, F_SMALL, OPT_CENTER, "Green = on. * = the alarm's default tier. Log in as admin to change."); }
}

/* priority tiers: name (tap to rename), what is active, and the actions for alarms with none of their own */
static void draw_tiers(void) {
    static const char *const lab[6] = { "Email", "Modbus", "R1", "R2", "R3", "R4" };
    static const uint8_t bit[6] = { AR_EMAIL, AR_MODBUS, AR_RELAY(0), AR_RELAY(1), AR_RELAY(2), AR_RELAY(3) };
    static const uint8_t sev[AL_TIERS] = { AL_SEV_FAULT, AL_SEV_FAULT, AL_SEV_ALARM, AL_SEV_WARNING, AL_SEV_STATUS };
    bool can = U.level >= PLAT_LVL_ADMIN;
    box(16, BODY_Y, 880, AL_TIERS * ROW_H + 8, C_CARD, 8);
    for (unsigned k = 0; k < AL_TIERS; ++k) {
        int16_t y = (int16_t)(BODY_Y + 4 + (int)k * ROW_H);
        uint8_t tg = (uint8_t)(T_ROW0 + k);
        eve_tag(E, tg);
        box(20, (int16_t)(y + 2), 272, ROW_H - 4, U.pressed == tg ? 0xDCE6F5u : C_CARD, 4);
        if (k) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
        box(24, (int16_t)(y + 8), 8, ROW_H - 16, sev_colour(sev[k]), 2);
        char b[64];
        snprintf(b, sizeof b, "%u  %.16s", k + 1u, alarm_prio_name((uint8_t)(k + 1u)));
        color(C_INK); text(40, (int16_t)(y + 20), F_TEXT, OPT_CENTERY, b);
        snprintf(b, sizeof b, "%u active, %u not acknowledged", (unsigned)pv(PARAM_ID_ALARM_TIER_ACTIVE, (uint16_t)k, 0),
                 (unsigned)pv(PARAM_ID_ALARM_TIER_UNACKED, (uint16_t)k, 0));
        color(C_MUTE); text(40, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, b);
        eve_tag(E, 0);
        uint8_t r = (uint8_t)pv(PARAM_ID_ALARM_TIER_ROUTE, (uint16_t)k, 0);
        bool set = (r & AR_SET) != 0;
        if (set) {                                     /* back to each alarm's own default */
            uint8_t td = (uint8_t)(T_PRIO0 + k);
            eve_tag(E, can ? td : 0);
            eve_fgcolor(E, U.pressed == td ? C_PRESS : 0x5B6577u);
            color(0xFFFFFFu);
            ebutton(300, (int16_t)(y + 6), 132, ROW_H - 12, F_SMALL, OPT_FLAT, "Use defaults");
            eve_tag(E, 0);
        } else { color(C_MUTE); text(366, (int16_t)(y + 32), F_SMALL, OPT_CENTER, "alarm defaults"); }
        for (int j = 0; j < 6; ++j) {
            bool on = set && (r & bit[j]) != 0;
            uint8_t tb = (uint8_t)(T_CFG0 + k * 6 + (unsigned)j);
            eve_tag(E, can ? tb : 0);
            eve_fgcolor(E, U.pressed == tb ? C_PRESS : on ? C_OK : 0xB8C0CCu);
            color(0xFFFFFFu);
            ebutton((int16_t)(440 + j * 75), (int16_t)(y + 6), 70, ROW_H - 12, F_SMALL, OPT_FLAT, lab[j]);
            eve_tag(E, 0);
        }
    }
    color(C_MUTE);
    text(32, (int16_t)(BODY_Y + AL_TIERS * ROW_H + 30), F_SMALL, OPT_CENTERY,
         "A tier's actions apply to alarms whose own actions were never changed. Tap a name to rename it.");
    button(T_BACK, 908, BODY_Y, 100, 120, "Back", 0x5B6577u, true);
    if (!can) { color(C_MUTE); text(456, TOAST_Y + 22, F_SMALL, OPT_CENTER, "Green = on. Log in as admin to change."); }
}


/* ---------------- trends ---------------- */
static const uint32_t tr_col[TR_PENS] = { 0x1F5FAEu, 0xD9822Bu, 0x1A7F4Bu, 0x7B4FB0u };   /* pen colours: blue, orange, green, purple */

static void tr_range(unsigned p, float *lo, float *hi) {
    bool any = false;
    for (unsigned k = 0; k < tr_count; ++k) {
        float x = tr_at(p, k);
        if (isnan(x)) continue;
        if (!any || x < *lo) *lo = x;
        if (!any || x > *hi) *hi = x;
        any = true;
    }
    if (!any) { *lo = 0.0f; *hi = 1.0f; return; }
    float pad = (*hi - *lo) * 0.05f;
    if (pad <= 0.0f) pad = fabsf(*hi) > 1.0f ? fabsf(*hi) * 0.05f : 0.5f;   /* a flat line sits in the middle */
    *lo -= pad; *hi += pad;
}

static void draw_trends(void) {
    enum { L = 104, R = 880, T = BODY_Y + 92, B = BODY_Y + 334 };
    char b[112];
    unsigned npen = 0;
    for (unsigned p = 0; p < TR_PENS; ++p) npen += U.tr_id[p] != 0;
    if (npen && !U.tr_id[U.tr_sel]) for (unsigned p = 0; p < TR_PENS; ++p) if (U.tr_id[p]) { U.tr_sel = (uint8_t)p; break; }
    /* the pens: name and value now; tap one to show its scale */
    for (unsigned p = 0; p < TR_PENS; ++p) {
        int16_t x = (int16_t)(16 + p * 222);
        const param_def_t *d = U.tr_id[p] ? param_find(U.tr_id[p]) : NULL;
        if (!d) { box(x, BODY_Y, 214, 64, 0xE9ECF1u, 8); continue; }
        uint8_t tg = (uint8_t)(T_TR_PEN0 + p);
        eve_tag(E, tg);
        box(x, BODY_Y, 214, 64, U.pressed == tg ? 0xDCE6F5u : p == U.tr_sel ? 0xEEF3FAu : C_CARD, 8);
        box((int16_t)(x + 6), BODY_Y + 8, 6, 48, tr_col[p], 2);
        snprintf(b, sizeof b, "%.18s", d->name);
        color(C_MUTE); text((int16_t)(x + 20), BODY_Y + 18, F_SMALL, OPT_CENTERY, b);
        char v[48]; fmt_value(d, v, sizeof v);
        qvalue((int16_t)(x + 20), BODY_Y + 44, F_TEXT, d->id, UW_NO_INDEX, v);
        eve_tag(E, 0);
    }
    box(16, BODY_Y + 72, 880, 320, C_CARD, 8);
    for (int g = 0; g <= 4; ++g) { int16_t y = (int16_t)(T + g * (B - T) / 4); color(C_LINE); eve_line(E, L, y, R, y, 1); }
    color(C_MUTE);
    text(L, B + 18, F_SMALL, OPT_CENTERY, "-10 min");
    text((L + R) / 2, B + 18, F_SMALL, OPT_CENTER, "-5 min");
    text(R, B + 18, F_SMALL, OPT_CENTERY | OPT_RIGHTX, "now");
    if (!npen) {
        color(C_MUTE); text((L + R) / 2, (T + B) / 2, F_TEXT, OPT_CENTER, "Tap Add to choose a value to trend");
    } else {
        float lo = 0, hi = 1;
        tr_range(U.tr_sel, &lo, &hi);
        const param_def_t *d = param_find(U.tr_id[U.tr_sel]);
        for (int g = 0; g <= 4; ++g) {                 /* the selected pen's scale */
            fmt_float(b, sizeof b, (double)(hi - (hi - lo) * (float)g / 4.0f));
            color(tr_col[U.tr_sel]); text(L - 8, (int16_t)(T + g * (B - T) / 4), F_SMALL, OPT_CENTERY | OPT_RIGHTX, b);
        }
        static int16_t xy[2 * TR_N];
        for (unsigned p = 0; p < TR_PENS; ++p) {        /* each pen on its own scale; a gap where it had no valid value */
            if (!U.tr_id[p]) continue;
            float plo = 0, phi = 1;
            tr_range(p, &plo, &phi);
            unsigned n = 0;
            for (unsigned k = 0; k <= tr_count; ++k) {
                float x = k < tr_count ? tr_at(p, k) : NAN;
                if (!isnan(x)) {
                    xy[2 * n] = (int16_t)(R - (int)((tr_count - 1u - k) * (unsigned)(R - L) / (TR_N - 1u)));
                    xy[2 * n + 1] = (int16_t)(B - (int)((x - plo) / (phi - plo) * (float)(B - T)));
                    ++n;
                    continue;
                }
                color(tr_col[p]);
                if (n == 1) eve_line(E, xy[0], xy[1], (int16_t)(xy[0] + 1), xy[1], p == U.tr_sel ? 3 : 2);
                else eve_strip(E, xy, n, p == U.tr_sel ? 3 : 2);
                n = 0;
            }
        }
        snprintf(b, sizeof b, "Each value on its own scale; the scale shown is %.20s's%s%.8s%s. A sample every %u s.", d ? d->name : "?",
                 d && d->unit ? " (" : "", d && d->unit ? d->unit : "", d && d->unit ? ")" : "", TR_MS / 1000u);
        color(C_MUTE); text(32, BODY_Y + 372, F_SMALL, OPT_CENTERY, b);
    }
    button(T_TR_ADD, 908, BODY_Y, 100, 120, "Add", C_ACCENT, npen < TR_PENS);
    button(T_TR_DEL, 908, BODY_Y + 136, 100, 120, "Remove", 0x5B6577u, npen > 0);
    button(T_TR_CLR, 908, BODY_Y + 272, 100, 120, "Restart", 0x5B6577u, npen > 0);
}

static void draw_tr_pick(void) {
    unsigned n = tr_pick_count();
    box(16, BODY_Y, 880, ROWS * ROW_H + 8, C_CARD, 8);
    for (unsigned r = 0; r < ROWS && U.tr_first + r < n; ++r) {
        const param_def_t *d = tr_pick_at(U.tr_first + r);
        int16_t y = (int16_t)(BODY_Y + 4 + (int)r * ROW_H);
        eve_tag(E, (uint8_t)(T_ROW0 + r));
        box(20, y, 872, ROW_H - 2, U.pressed == T_ROW0 + r ? 0xDCE6F5u : C_CARD, 4);
        if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
        char b[64], v[48];
        snprintf(b, sizeof b, "%.40s", d->name);
        color(C_INK); text(32, (int16_t)(y + 20), F_TEXT, OPT_CENTERY, b);
        snprintf(b, sizeof b, "%u", d->id);
        color(C_MUTE); text(32, (int16_t)(y + 46), F_SMALL, OPT_CENTERY, b);
        fmt_value(d, v, sizeof v);
        color(C_INK); text(880, (int16_t)(y + 32), F_TEXT, OPT_CENTERY | OPT_RIGHTX, v);
        eve_tag(E, 0);
    }
    if (!n) { color(C_MUTE); text(456, BODY_Y + 200, F_TEXT, OPT_CENTER, "No values to trend"); }
    button(T_PGUP, 908, BODY_Y, 100, 120, "Up", C_ACCENT, U.tr_first > 0);
    button(T_PGDN, 908, BODY_Y + 136, 100, 120, "Down", C_ACCENT, U.tr_first + ROWS < n);
    button(T_BACK, 908, BODY_Y + 272, 100, 120, "Back", 0x5B6577u, true);
}

/* ---------------- emissions (emis.h, docs/EMISSIONS_MATH.md), read only ---------------- */
static const char *const emis_slot[EMIS_CH_MAX] = {     /* the standard block's slot names (docs/MODBUS_STANDARD_BLOCK.md) */
    "O2 dry", "O2 wet", "CO2", "CO", "NO", "NO2", "NOx as NO2", "SO2", "HCl", "HF", "NH3", "H2O",
    "TOC as C", "CH4", "N2O", "H2S", "Hg", "Dust", "Flue gas flow", "Flue gas velocity",
    "Stack temperature", "Stack pressure", "Sample flow", "Probe/line temp.", "Cooler temp.", "Cell temp.",
    "Cabinet temp.", "Air/cal gas pressure", "Opacity", "TRS", "Spare 31", "Spare 32" };
static const char *emis_unit(uint8_t u, bool raw) {      /* the unit a bank is in (EMISSIONS_MATH section 2) */
    if (u == EMIS_U_PPM || u == EMIS_U_MG_ACT || u == EMIS_U_MG_STD) return raw ? (u == EMIS_U_PPM ? "ppm" : u == EMIS_U_MG_ACT ? "mg/m\xC2\xB3" : "mg/Nm\xC2\xB3") : "mg/Nm\xC2\xB3";
    if (u == EMIS_U_PCT) return "%";
    if (u == EMIS_U_FLOW_ACT || u == EMIS_U_FLOW_STD) return raw && u == EMIS_U_FLOW_ACT ? "m\xC2\xB3/h" : "Nm\xC2\xB3/h";
    return "";
}
/* one value right-aligned at x: the number and unit, greyed with its quality word under it when not valid */
static void emis_cell(int16_t x, int16_t y, float v, uint8_t q, const char *unit) {
    char b[40], n[24];
    if (q == PLAT_Q_UNAVAILABLE || !isfinite(v)) snprintf(n, sizeof n, "----"); else fmt_float(n, sizeof n, (double)v);
    snprintf(b, sizeof b, "%s %s", n, isfinite(v) && q != PLAT_Q_UNAVAILABLE ? unit : "");
    color(q == PLAT_Q_VALID ? C_INK : C_GREY); text(x, y, F_SMALL, OPT_CENTERY | OPT_RIGHTX, b);
    if (q != PLAT_Q_VALID) { color(q_color(q)); text(x, (int16_t)(y + 18), F_SMALL, OPT_CENTERY | OPT_RIGHTX, plat_quality_name(q)); }
}
static void emis_notes(unsigned i, char *b, size_t cap, uint32_t *c) {   /* status words, QAL3, the second period */
    static const struct { uint16_t bit; const char *w; } st[] = {
        { EMIS_ST_QAL2_REVIEW, "new QAL2 due" }, { EMIS_ST_ABOVE_CAL, "above calibration range" }, { EMIS_ST_OVER_RANGE, "over range" },
        { EMIS_ST_BELOW_RANGE, "below range" }, { EMIS_ST_O2_HIGH, "O2 too high" }, { EMIS_ST_INPUT_MISSING, "input missing" },
        { EMIS_ST_DAY_INVALID, "last day invalid" }, { EMIS_ST_AVG_INCOMPLETE, "last average discarded" }, { EMIS_ST_NO_QAL2, "no QAL2" } };
    const uint16_t bad = EMIS_ST_QAL2_REVIEW | EMIS_ST_ABOVE_CAL | EMIS_ST_OVER_RANGE | EMIS_ST_BELOW_RANGE | EMIS_ST_O2_HIGH |
                         EMIS_ST_INPUT_MISSING | EMIS_ST_DAY_INVALID;
    size_t o = 0; b[0] = 0; *c = C_MUTE;
    uint16_t s = emis_res.status[i];
    for (unsigned k = 0; k < sizeof st / sizeof st[0] && o + 2u < cap; ++k)
        if (s & st[k].bit) o += (size_t)snprintf(b + o, cap - o, "%s%s", o ? ", " : "", st[k].w);
    if (s & bad) *c = C_WARN;
    uint8_t q3 = emis_q3.status[i];
    if ((q3 & EMIS_Q3_ON) && o + 2u < cap) {
        const char *w = (q3 & (EMIS_Q3_ZERO_OUT | EMIS_Q3_SPAN_OUT)) ? ((q3 & EMIS_Q3_ZERO_OUT) && (q3 & EMIS_Q3_SPAN_OUT) ? "QAL3 OUT OF CONTROL (zero, span)"
                        : q3 & EMIS_Q3_ZERO_OUT ? "QAL3 OUT OF CONTROL (zero)" : "QAL3 OUT OF CONTROL (span)")
                      : q3 & EMIS_Q3_DUE ? "QAL3 check due"
                      : q3 & (EMIS_Q3_ZERO_WARN | EMIS_Q3_SPAN_WARN) ? "QAL3 last check beyond the action limit" : "QAL3 ok";
        o += (size_t)snprintf(b + o, cap - o, "%s%s", o ? ", " : "", w);
        if (q3 & (EMIS_Q3_ZERO_OUT | EMIS_Q3_SPAN_OUT)) *c = C_BAD;
        else if (q3 & (EMIS_Q3_DUE | EMIS_Q3_ZERO_WARN | EMIS_Q3_SPAN_WARN) && *c != C_BAD) *c = C_WARN;
    }
    if (emis_set.period2_s && emis_set.ch.elv2[i] > 0.0f && o + 2u < cap) {
        char a[24];
        if (emis_res.q2[i] == PLAT_Q_VALID && isfinite(emis_res.v2[i])) fmt_float(a, sizeof a, (double)emis_res.v2[i]); else snprintf(a, sizeof a, "----");
        o += (size_t)snprintf(b + o, cap - o, "%s%u-min %s %s, %u of %u above the limit today", o ? "; " : "", (unsigned)(emis_set.period2_s / 60u), a,
                              emis_unit(emis_set.ch.unit[i], false), (unsigned)emis_res.n2_above[i], (unsigned)emis_res.n2[i]);
        if (emis_res.n2[i] && emis_res.n2_above[i] * 20u > emis_res.n2[i] && *c != C_BAD) *c = C_WARN;   /* more than 5 % above */
    }
}

static void draw_emis(void) {
    char b[128];
    box(16, BODY_Y, 880, 392, C_CARD, 8);
    if (!emis_set.enable) {
        color(C_MUTE); text(456, BODY_Y + 180, F_TEXT, OPT_CENTER, "The emissions engine is off.");
        text(456, BODY_Y + 214, F_SMALL, OPT_CENTER, "It is set up on the web page, Emissions.");
        button(T_BACK, 908, BODY_Y + 272, 100, 120, "Back", 0x5B6577u, true);
        return;
    }
    if (emis_p_cfg_error) {
        if (emis_p_cfg_error > EMIS_CH_MAX) snprintf(b, sizeof b, "The plant settings don't fit together: the engine is stopped.");
        else snprintf(b, sizeof b, "Channel %s has a setting that doesn't fit: the engine is stopped.", emis_slot[emis_p_cfg_error - 1u]);
        color(C_BAD);
    } else {
        snprintf(b, sizeof b, "Plant %s. Averages every %u min, at %.4g %% %s.", emis_p_operating ? "operating" : "NOT operating: averages paused",
                 (unsigned)(emis_set.period_s / 60u), (double)emis_set.ref_pct, emis_set.diluent == EMIS_DIL_CO2 ? "CO2" : "O2");
        color(emis_p_operating ? C_INK : C_WARN);
    }
    text(32, BODY_Y + 18, F_SMALL, OPT_CENTERY, b);
    color(C_MUTE);
    text(430, BODY_Y + 44, F_SMALL, OPT_CENTERY | OPT_RIGHTX, "Now, corrected");
    text(590, BODY_Y + 44, F_SMALL, OPT_CENTERY | OPT_RIGHTX, "This period");
    text(740, BODY_Y + 44, F_SMALL, OPT_CENTERY | OPT_RIGHTX, "Validated");
    text(880, BODY_Y + 44, F_SMALL, OPT_CENTERY | OPT_RIGHTX, "Last day");
    unsigned used = 0, row = 0;
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        uint8_t u = emis_set.ch.unit[i];
        if (u == EMIS_U_OFF) continue;
        if (used++ < U.emis_first || row >= EMIS_ROWS) continue;
        int16_t y = (int16_t)(BODY_Y + 60 + row * 66);
        color(C_LINE); eve_line(E, 24, y, 888, y, 1);
        bool ref = emis_set.ch.ref[i];
        const char *un = emis_unit(u, false);
        color(C_INK); text(32, (int16_t)(y + 16), F_TEXT, OPT_CENTERY, emis_slot[i]);
        emis_cell(430, (int16_t)(y + 16), emis_res.v[ref ? EMIS_B_REF : EMIS_B_STD][i], emis_res.q[EMIS_Q_CALC][i], un);
        emis_cell(590, (int16_t)(y + 16), emis_res.v[EMIS_B_RUN][i], emis_res.q[EMIS_Q_RUN][i], un);
        emis_cell(740, (int16_t)(y + 16), emis_res.v[EMIS_B_VAL][i], emis_res.q[EMIS_Q_AVG][i], un);
        emis_cell(880, (int16_t)(y + 16), emis_res.v[EMIS_B_DAY][i], emis_res.q[EMIS_Q_DAY][i], un);
        uint32_t c;
        emis_notes(i, b, sizeof b, &c);
        if (emis_res.discarded[i]) {
            size_t o = strlen(b);
            snprintf(b + o, sizeof b - o, "%s%u discarded today", o ? ", " : "", (unsigned)emis_res.discarded[i]);
        }
        if (strlen(b) > 92) { b[89] = b[90] = b[91] = '.'; b[92] = 0; }   /* one line under the numbers' quality words */
        color(c); text(32, (int16_t)(y + 52), F_SMALL, OPT_CENTERY, b);
        ++row;
    }
    if (!used) { color(C_MUTE); text(456, BODY_Y + 200, F_TEXT, OPT_CENTER, "No channels are set up yet"); }
    if (U.emis_first >= used && used) U.emis_first = (used - 1u) / EMIS_ROWS * EMIS_ROWS;
    button(T_PGUP, 908, BODY_Y, 100, 120, "Up", C_ACCENT, U.emis_first > 0);
    button(T_PGDN, 908, BODY_Y + 136, 100, 120, "Down", C_ACCENT, U.emis_first + EMIS_ROWS < used);
    button(T_BACK, 908, BODY_Y + 272, 100, 120, "Back", 0x5B6577u, true);
}

/* ---------------- instruments (role 2: the core polls analysers on the I/O network), read only ---------------- */
static uint32_t inst_colour(uint8_t s) {
    return s == INST_S_OK ? C_OK : s == INST_S_LOST || s == INST_S_BAD_SETTINGS ? C_BAD : s == INST_S_FAILING ? C_WARN : C_GREY;
}
static void inst_label(unsigned k, char *b, size_t cap) {
    if (inst_tags.name[k][0]) snprintf(b, cap, "%.23s", inst_tags.name[k]); else snprintf(b, cap, "Instrument %u", k + 1u);
}

static void draw_inst(void) {
    char b[112], l[32];
    unsigned shown = 0;
    box(16, BODY_Y, 384, 392, C_CARD, 8);
    box(412, BODY_Y, 484, 392, C_CARD, 8);
    if (U.inst_sel >= INST_MAX || !inst_tags.enable[U.inst_sel])
        for (unsigned k = 0; k < INST_MAX; ++k) if (inst_tags.enable[k]) { U.inst_sel = (uint8_t)k; break; }
    for (unsigned k = 0; k < INST_MAX; ++k) {          /* the instruments: enabled ones only */
        if (!inst_tags.enable[k]) continue;
        int16_t y = (int16_t)(BODY_Y + 4 + shown * 48);
        uint8_t tg = (uint8_t)(T_ROW0 + k);
        eve_tag(E, tg);
        box(20, y, 376, 46, U.pressed == tg ? 0xDCE6F5u : k == U.inst_sel ? 0xEEF3FAu : C_CARD, 4);
        if (shown) { color(C_LINE); eve_line(E, 24, y, 392, y, 1); }
        inst_label(k, l, sizeof l);
        color(C_INK); text(32, (int16_t)(y + 14), F_SMALL, OPT_CENTERY, l);
        color(inst_colour(inst_tags.state[k])); text(388, (int16_t)(y + 14), F_SMALL, OPT_CENTERY | OPT_RIGHTX, inst_state_name(inst_tags.state[k]));
        char ip[20]; fmt_ip(ip, sizeof ip, inst_tags.ip[k]);
        snprintf(b, sizeof b, "%s:%u, unit %u", inst_tags.ip[k] ? ip : "no address", (unsigned)inst_tags.port[k], (unsigned)inst_tags.unit[k]);
        color(C_MUTE); text(32, (int16_t)(y + 34), F_SMALL, OPT_CENTERY, b);
        eve_tag(E, 0);
        ++shown;
    }
    if (!shown) {
        color(C_MUTE);
        text(32, BODY_Y + 24, F_SMALL, 0, "No instruments are set up.");
        text(32, BODY_Y + 48, F_SMALL, 0, "Set them up on the web page, Instruments.");
        button(T_BACK, 908, BODY_Y + 272, 100, 120, "Back", 0x5B6577u, true);
        return;
    }
    /* the selected instrument: counters, then its points */
    unsigned k = U.inst_sel;
    inst_label(k, l, sizeof l);
    color(C_INK); text(428, BODY_Y + 20, F_TEXT, OPT_CENTERY, l);
    color(inst_colour(inst_tags.state[k])); text(880, BODY_Y + 20, F_SMALL, OPT_CENTERY | OPT_RIGHTX, inst_state_name(inst_tags.state[k]));
    snprintf(b, sizeof b, "%lu polls good, %lu failed, reply %u ms", (unsigned long)inst_tags.good[k], (unsigned long)inst_tags.errors[k],
             (unsigned)inst_tags.resp_ms[k]);
    color(C_MUTE); text(428, BODY_Y + 46, F_SMALL, OPT_CENTERY, b);
    if (inst_tags.last_err[k]) {
        if (inst_tags.last_err[k] == INST_E_EXCEPTION) snprintf(b, sizeof b, "Last error: exception %02X", (unsigned)inst_tags.last_exc[k]);
        else snprintf(b, sizeof b, "Last error: %s", inst_err_name(inst_tags.last_err[k]));
        color(C_WARN); text(428, BODY_Y + 68, F_SMALL, OPT_CENTERY, b);
    }
    unsigned np = 0, row = 0;
    for (unsigned p = 0; p < INST_POINTS; ++p) {
        if (inst_pt.inst[p] != k + 1u) continue;
        if (np++ < U.inst_first || row >= INST_PT_ROWS) continue;
        int16_t y = (int16_t)(BODY_Y + 92 + row * 33);
        if (row) { color(C_LINE); eve_line(E, 424, (int16_t)(y - 4), 884, (int16_t)(y - 4), 1); }
        float v; uint8_t q;
        if (!inst_point(p, &v, &q)) { v = NAN; q = PLAT_Q_UNAVAILABLE; }
        char nm[32], un[24], vs[24];
        if (inst_pt.name[p][0]) snprintf(nm, sizeof nm, "%.22s", inst_pt.name[p]); else snprintf(nm, sizeof nm, "Point %u", p + 1u);
        if (utf16le_to_utf8(inst_pt.unit[p], INST_UNIT_LEN / 2u, un, sizeof un) < 0) un[0] = 0;
        if (q == PLAT_Q_UNAVAILABLE || isnan(v)) snprintf(vs, sizeof vs, "----"); else fmt_float(vs, sizeof vs, (double)v);
        snprintf(b, sizeof b, "%s %s", vs, un);
        color(C_MUTE); text(428, (int16_t)(y + 12), F_SMALL, OPT_CENTERY, nm);
        color(q == PLAT_Q_VALID ? C_INK : C_GREY); text(730, (int16_t)(y + 12), F_SMALL, OPT_CENTERY | OPT_RIGHTX, b);
        color(q_color(q)); text(880, (int16_t)(y + 12), F_SMALL, OPT_CENTERY | OPT_RIGHTX, q == PLAT_Q_VALID ? "ok" : plat_quality_name(q));
        ++row;
    }
    if (!np) { color(C_MUTE); text(428, BODY_Y + 104, F_SMALL, 0, "No points read from this instrument"); }
    if (U.inst_first >= np && np) U.inst_first = (np - 1u) / INST_PT_ROWS * INST_PT_ROWS;
    button(T_PGUP, 908, BODY_Y, 100, 120, "Up", C_ACCENT, U.inst_first > 0);
    button(T_PGDN, 908, BODY_Y + 136, 100, 120, "Down", C_ACCENT, U.inst_first + INST_PT_ROWS < np);
    button(T_BACK, 908, BODY_Y + 272, 100, 120, "Back", 0x5B6577u, true);
}

/* ---------------- service: self-test, board test, test record, service log ---------------- */
static const char *test_word(uint8_t r, uint32_t *c) {
    if (r == PLAT_TEST_PASS) { *c = C_OK; return "pass"; }
    if (r == PLAT_TEST_FAIL) { *c = C_BAD; return "FAIL"; }
    *c = C_GREY; return "skipped";
}

static void draw_service(void) {
    char b[112];
    uint32_t c;
    box(16, BODY_Y, 496, 392, C_CARD, 8);
    box(528, BODY_Y, 480, 316, C_CARD, 8);
    /* start-up self-test, every check by name */
    color(C_INK); text(32, BODY_Y + 10, F_TEXT, 0, plat_selftest_done() ? "Start-up self-test" : "Start-up self-test (running)");
    unsigned nst = plat_selftest_count();
    for (unsigned i = 0; i < nst && i < 16u; ++i) {
        const char *nm; uint8_t r;
        if (!plat_selftest_get(i, &nm, &r)) continue;
        int16_t x = (int16_t)(32 + (i % 2u) * 236), y = (int16_t)(BODY_Y + 44 + (i / 2u) * 22);
        const char *w = r == PLAT_STR_PASS ? "pass" : r == PLAT_STR_FAIL ? "FAIL" : r == PLAT_STR_SKIP ? "skipped" : "...";
        c = r == PLAT_STR_PASS ? C_OK : r == PLAT_STR_FAIL ? C_BAD : C_GREY;
        snprintf(b, sizeof b, "%.14s", nm);
        color(C_MUTE); text(x, y, F_SMALL, 0, b);
        color(c); text((int16_t)(x + 140), y, F_SMALL, 0, w);
    }
    /* board test (factory): live while it runs, else the last run or the stored record */
    plat_bt_status_t bt; plat_boardtest_status(&bt);
    plat_test_record_t tr;
    bool rec = plat_service_test_record(&tr);
    color(C_INK); text(32, BODY_Y + 230, F_TEXT, 0, "Board test");
    if (bt.running) snprintf(b, sizeof b, "running...");
    else if (bt.done || rec) {
        unsigned np = 0, nf = 0, ns = 0, n = bt.done ? PLAT_BT_N : (tr.n < PLAT_SVC_TESTS ? tr.n : PLAT_SVC_TESTS);
        const char *first = NULL;
        for (unsigned i = 0; i < n; ++i) {
            uint8_t r = bt.done ? bt.result[i] : tr.result[i];
            if (r == PLAT_TEST_PASS) ++np; else if (r == PLAT_TEST_FAIL) { ++nf; if (!first) first = plat_boardtest_name(i); } else ++ns;
        }
        if (bt.done) snprintf(b, sizeof b, "this run: %u passed, %u failed, %u skipped", np, nf, ns);
        else {
            char t[24];
            if (tr.time) log_fmt_time(t, sizeof t, tr.time, true); else snprintf(t, sizeof t, "clock not set");
            snprintf(b, sizeof b, "%.16s, %.11s: %u passed, %u failed", t, tr.station, np, nf);
        }
        if (first) { color(C_BAD); char f[48]; snprintf(f, sizeof f, "First failure: %.24s", first); text(32, BODY_Y + 286, F_SMALL, 0, f); }
    } else snprintf(b, sizeof b, "not run on this unit");
    color(C_INK); text(32, BODY_Y + 262, F_SMALL, 0, b);
    if (bt.done) {                                     /* the run's results, two columns */
        for (unsigned i = 0; i < PLAT_BT_N; ++i) {
            const char *nm = plat_boardtest_name(i);
            if (!nm) break;
            int16_t x = (int16_t)(32 + (i % 3u) * 156), y = (int16_t)(BODY_Y + 286 + 22 + (i / 3u) * 20);
            const char *w = test_word(bt.result[i], &c);
            snprintf(b, sizeof b, "%.9s %s", nm, w);
            color(c); text(x, y, F_SMALL, 0, b);
        }
    }
    button(T_SVC_BT, 300, BODY_Y + 222, 196, 56, bt.running ? "Running..." : "Run board test", C_ACCENT, !bt.running);
    /* service log, newest first */
    color(C_INK); text(544, BODY_Y + 10, F_TEXT, 0, "Service log");
    {   uint32_t h = plat_service_since_x10();
        snprintf(b, sizeof b, "%lu.%lu h since the last service", (unsigned long)(h / 10u), (unsigned long)(h % 10u));
        color(C_MUTE); text(544, BODY_Y + 40, F_SMALL, 0, b); }
    plat_svc_entry_t se;
    unsigned ne = 0;
    for (unsigned i = 0; i < 4u && plat_service_entry(i, &se); ++i, ++ne) {
        int16_t y = (int16_t)(BODY_Y + 70 + i * 60);
        char t[24];
        if (se.time) log_fmt_time(t, sizeof t, se.time, true); else snprintf(t, sizeof t, "clock not set");
        snprintf(b, sizeof b, "%.16s  %.15s", t, se.tech);
        color(C_INK); text(544, y, F_SMALL, 0, b);
        snprintf(b, sizeof b, "%.39s", se.work);
        color(C_MUTE); text(544, (int16_t)(y + 22), F_SMALL, 0, b);
        if (i) { color(C_LINE); eve_line(E, 544, (int16_t)(y - 8), 992, (int16_t)(y - 8), 1); }
    }
    if (!ne) { color(C_MUTE); text(544, BODY_Y + 76, F_SMALL, 0, "No entries yet"); }
    button(T_SVC_LOG, 544, BODY_Y + 330, 196, 62, "Add entry", C_ACCENT, true);
    button(T_DISP, 752, BODY_Y + 330, 150, 62, "Display test", 0x5B6577u, true);
    button(T_BACK, 910, BODY_Y + 330, 98, 62, "Back", 0x5B6577u, true);
    if (U.level < PLAT_LVL_FACTORY) { color(C_MUTE); text(456, TOAST_Y + 22, F_SMALL, OPT_CENTER, "The board test and the service log need a factory (tech) login."); }
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

/* ---------------- Modbus force (commissioning, modbus.h mb_force_*) ----------------
 * Every value the PLC reads that may be forced (mb_forceable), array elements one per row, the
 * product's values before the platform block's. A force
 * changes only what Modbus serves, for PLAT_MB_FORCE_MS; this screen shows the real value and,
 * beside it, what Modbus is sending and for how long. Forcing needs admin; anyone sees the list. */
static void draw_force(uint32_t now) {
    unsigned total = 0, r = 0;
    memset(U.fc_id, 0, sizeof U.fc_id);
    box(16, BODY_Y, 880, ROWS * ROW_H + 8, C_CARD, 8);
    for (int pass = 0; pass < 2; ++pass)              /* the product's values first, then the platform's */
    for (size_t i = 0; i < param_count(); ++i) {
        const param_def_t *d = param_at(i);
        if ((d->id >= PARAM_ID_APP_FIRST) != (pass == 0) || !mb_forceable(d->id)) continue;
        unsigned ne = d->ptype == PT_ARRAY ? (unsigned)(d->size / param_ctype_size(d->ctype)) : 1u;
        for (unsigned k = 0; k < ne; ++k, ++total) {
            if (total < U.fc_first || r >= ROWS) continue;
            uint16_t ix = d->ptype == PT_ARRAY ? (uint16_t)k : MB_FORCE_SCALAR;
            int16_t y = (int16_t)(BODY_Y + 4 + (int)r * ROW_H);
            if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
            char b[80], v[40];
            if (ix == MB_FORCE_SCALAR) { snprintf(b, sizeof b, "%u  %.40s", d->id, d->name); fmt_value(d, v, sizeof v); }
            else { snprintf(b, sizeof b, "%u  %.36s[%u]", d->id, d->name, k); fmt_elem(d, ix, v, sizeof v); }
            color(C_INK); text(32, (int16_t)(y + 18), F_TEXT, OPT_CENTERY, b);
            snprintf(b, sizeof b, "Real value: %s", v);
            color(C_MUTE); text(32, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, b);
            bool on = mb_force_active(d->id, ix);
            if (on) {
                mb_force_t f;
                for (unsigned j = 0; j < mb_force_count(); ++j)
                    if (mb_force_get(j, &f, now) && f.id == d->id && f.index == ix) {
                        if (d->ctype == CT_BOOL) snprintf(v, sizeof v, "%s", f.v != 0 ? "ON" : "OFF");
                        else fmt_float(v, sizeof v, f.v);
                        snprintf(b, sizeof b, "Modbus: forced to %s, %u s", v, (unsigned)((f.left_ms + 999u) / 1000u));
                        color(C_WARN); text(360, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, b);
                        break;
                    }
            }
            U.fc_id[r] = d->id; U.fc_idx[r] = ix;
            button((uint8_t)(T_ROW0 + r), 724, (int16_t)(y + 4), 164, ROW_H - 8, on ? "Clear" : "Force", on ? C_WARN : C_ACCENT, true);
            ++r;
        }
    }
    if (!total) { color(C_MUTE); text(456, BODY_Y + 200, F_TEXT, OPT_CENTER, "No values on Modbus can be forced"); }
    if (U.fc_first && U.fc_first >= total) { U.fc_first = 0; U.dirty = true; }
    button(T_PGUP, 908, BODY_Y, 100, 96, "Up", C_ACCENT, U.fc_first > 0);
    button(T_PGDN, 908, BODY_Y + 104, 100, 96, "Down", C_ACCENT, U.fc_first + ROWS < total);
    button(T_FC_CLEAR_ALL, 908, BODY_Y + 208, 100, 96, "Clear all", C_WARN, mb_force_count() > 0);
    button(T_BACK, 908, BODY_Y + 312, 100, 80, "Back", 0x5B6577u, true);
    if (mb_force_count()) U.dirty = true;             /* the countdown */
}

/* ---------------- input alarms (plat_alarm.h, parameters 340-345 and 350-373) ----------------
 * PLAT_DIA_SLOTS slots, each tying a digital input to alarm 30 + s. The label typed here is the
 * alarm's name everywhere: the alarm list, the web page, the Modbus map and its Excel file. */
static const char *dia_sev_name(int64_t v) { return v == AL_SEV_WARNING ? "warning" : v == AL_SEV_FAULT ? "fault" : "alarm"; }
static void dia_status(unsigned s, const char **w, uint32_t *c) {
    switch (pv(PARAM_ID_DIA_STATUS, (uint16_t)s, DIA_ST_UNUSED)) {
    case DIA_ST_NORMAL:          *w = "normal"; *c = C_OK; break;
    case DIA_ST_ACTIVE_ACKED:    *w = "active"; *c = C_BAD; break;
    case DIA_ST_ACTIVE_UNACKED:  *w = "ACTIVE, not acknowledged"; *c = C_BAD; break;
    case DIA_ST_CLEARED_UNACKED: *w = "cleared, not acknowledged"; *c = C_WARN; break;
    case DIA_ST_UNRELIABLE:      *w = "input unreliable"; *c = C_WARN; break;
    default:                     *w = "not used"; *c = C_GREY; break;
    }
}
/* the slot's name as the alarm list shows it */
static void dia_label(unsigned s, char *b, size_t cap) {
    const param_def_t *d = param_find((uint16_t)(PARAM_ID_DIA_LABEL_FIRST + s));
    b[0] = 0;
    if (d) fmt_value(d, b, cap);
    if (!b[0]) {
        int64_t in = pv(PARAM_ID_DIA_INPUT, (uint16_t)s, 0);
        if (in) snprintf(b, cap, "Digital input %u", (unsigned)in); else snprintf(b, cap, "Not used");
    }
}
static void dia_detail(unsigned s, char *b, size_t cap) {
    int64_t in = pv(PARAM_ID_DIA_INPUT, (uint16_t)s, 0);
    if (!in) { snprintf(b, cap, "No input chosen"); return; }
    char n[32];
    plat_dia_input_name((unsigned)in, n, sizeof n);
    snprintf(b, cap, "%.28s, when %s, %u s%s, %.7s", n, pv(PARAM_ID_DIA_WHEN, (uint16_t)s, 0) ? "inactive" : "active",
             (unsigned)pv(PARAM_ID_DIA_DELAY, (uint16_t)s, 0), pv(PARAM_ID_DIA_LATCH, (uint16_t)s, 0) ? ", latched" : "",
             dia_sev_name(pv(PARAM_ID_DIA_SEV, (uint16_t)s, AL_SEV_ALARM)));
}

static void draw_dia(void) {
    box(16, BODY_Y, 880, ROWS * ROW_H + 8, C_CARD, 8);
    for (unsigned r = 0; r < ROWS && U.dia_first + r < PLAT_DIA_SLOTS; ++r) {
        unsigned s = U.dia_first + r;
        int16_t y = (int16_t)(BODY_Y + 4 + (int)r * ROW_H);
        eve_tag(E, (uint8_t)(T_ROW0 + r));
        box(20, y, 872, ROW_H - 2, U.pressed == T_ROW0 + r ? 0xDCE6F5u : C_CARD, 4);
        char b[112], l[40];
        dia_label(s, l, sizeof l);
        snprintf(b, sizeof b, "%u  %s", s + 1u, l);
        color(C_INK); text(32, (int16_t)(y + 18), F_TEXT, OPT_CENTERY, b);
        dia_detail(s, b, sizeof b);
        color(C_MUTE); text(32, (int16_t)(y + 44), F_SMALL, OPT_CENTERY, b);
        const char *w; uint32_t c;
        dia_status(s, &w, &c);
        color(c); text(880, (int16_t)(y + 18), F_SMALL, OPT_CENTERY | OPT_RIGHTX, w);
        color(C_ACCENT); text(880, (int16_t)(y + 44), F_SMALL, OPT_CENTERY | OPT_RIGHTX, "Set up >");
        eve_tag(E, 0);
        if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
    }
    button(T_PGUP, 908, BODY_Y, 100, 120, "Up", C_ACCENT, U.dia_first > 0);
    button(T_PGDN, 908, BODY_Y + 136, 100, 120, "Down", C_ACCENT, U.dia_first + ROWS < PLAT_DIA_SLOTS);
    button(T_BACK, 908, BODY_Y + 272, 100, 120, "Back", 0x5B6577u, true);
}

static void draw_dia_slot(void) {
    unsigned s = U.dia_s;
    char b[112], l[40];
    box(16, BODY_Y, 880, 392, C_CARD, 8);
    dia_label(s, l, sizeof l);
    snprintf(b, sizeof b, "Input alarm %u: %s", s + 1u, l);
    color(C_INK); text(32, BODY_Y + 20, F_BIG, OPT_CENTERY, b);
    const char *w; uint32_t c;
    dia_status(s, &w, &c);
    color(c); text(880, BODY_Y + 20, F_TEXT, OPT_CENTERY | OPT_RIGHTX, w);
    snprintf(b, sizeof b, "Modbus discrete inputs: %u active, %u not acknowledged, %u input unreliable",
             PLAT_DIA_MB_ADDR + s, PLAT_DIA_MB_ADDR + 32u + s, PLAT_DIA_MB_ADDR + 64u + s);
    color(C_MUTE); text(32, BODY_Y + 42, F_SMALL, OPT_CENTERY, b);
    int64_t in = pv(PARAM_ID_DIA_INPUT, (uint16_t)s, 0);
    struct { uint8_t tag; const char *k; const char *btn; } rows[6] = {
        { T_DIA_INPUT, "Input", "Change" }, { T_DIA_WHEN, "Alarm when", "Switch" }, { T_DIA_DELAY, "On-delay", "Change" },
        { T_DIA_LATCH, "Latch", "Switch" }, { T_DIA_SEV, "Severity", "Next" }, { T_DIA_LABEL, "Label", "Change" } };
    for (int r = 0; r < 6; ++r) {
        int16_t y = (int16_t)(BODY_Y + 54 + r * 56);    /* the whole row is the 872 x 56 touch target */
        eve_tag(E, rows[r].tag);
        box(20, y, 872, 56, U.pressed == rows[r].tag ? 0xDCE6F5u : C_CARD, 4);
        if (r) { color(C_LINE); eve_line(E, 24, y, 888, y, 1); }
        switch (rows[r].tag) {
        case T_DIA_INPUT:
            if (in) { char n[32]; plat_dia_input_name((unsigned)in, n, sizeof n); snprintf(b, sizeof b, "%u: %.30s", (unsigned)in, n); }
            else snprintf(b, sizeof b, "not used (0)");
            break;
        case T_DIA_WHEN:  snprintf(b, sizeof b, pv(PARAM_ID_DIA_WHEN, (uint16_t)s, 0) ? "the input is inactive (open)" : "the input is active (closed)"); break;
        case T_DIA_DELAY: snprintf(b, sizeof b, "%u s", (unsigned)pv(PARAM_ID_DIA_DELAY, (uint16_t)s, 0)); break;
        case T_DIA_LATCH: snprintf(b, sizeof b, pv(PARAM_ID_DIA_LATCH, (uint16_t)s, 0) ? "on: stays active until acknowledged" : "off"); break;
        case T_DIA_SEV:   snprintf(b, sizeof b, "%s", dia_sev_name(pv(PARAM_ID_DIA_SEV, (uint16_t)s, AL_SEV_ALARM))); break;
        default: {
            const param_def_t *d = param_find((uint16_t)(PARAM_ID_DIA_LABEL_FIRST + s));
            b[0] = 0; if (d) fmt_value(d, b, sizeof b);
            if (!b[0]) snprintf(b, sizeof b, "(empty: shown as \"Digital input %u\")", (unsigned)in);
        } }
        color(C_MUTE); text(32, (int16_t)(y + 28), F_TEXT, OPT_CENTERY, rows[r].k);
        color(C_INK);  text(220, (int16_t)(y + 28), F_TEXT, OPT_CENTERY, b);
        snprintf(b, sizeof b, "%s >", rows[r].btn);
        color(C_ACCENT); text(880, (int16_t)(y + 28), F_TEXT, OPT_CENTERY | OPT_RIGHTX, b);
        eve_tag(E, 0);
    }
    button(T_DIA_PREV, 908, BODY_Y, 100, 120, "Previous", C_ACCENT, true);
    button(T_DIA_NEXT, 908, BODY_Y + 136, 100, 120, "Next", C_ACCENT, true);
    button(T_BACK, 908, BODY_Y + 272, 100, 120, "Back", 0x5B6577u, true);
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
                /* the item's state element, if it is a measurement, carries its own quality (UI_CONVENTIONS 4) */
                uint8_t q = w->state_id && is_arr(w->state_id) ? param_quality(w->state_id, (uint16_t)(k - 1u)) : PLAT_Q_VALID;
                U.tmap_w[tag] = (uint8_t)(i + 1u); U.tmap_i[tag] = (uint16_t)k;
                eve_tag(E, en ? (uint8_t)tag : 0);
                bool qv = q == PLAT_Q_VALID;
                eve_fgcolor(E, !qv ? (U.pressed == tag ? 0xDCE6F5u : 0xE4E7ECu) : !en ? C_GREY : U.pressed == tag ? C_PRESS : item_lit(w, k) ? C_OK : 0x5B6577u);
                color(0xFFFFFFu);
                if (qv) ebutton(ix, iy, 104, 56, F_SMALL, OPT_FLAT, b);
                else {                                       /* light, not lit or unlit: the name and the quality word under it */
                    ebutton(ix, iy, 104, 56, F_SMALL, OPT_FLAT, "");
                    color(en ? C_INK : C_GREY); text((int16_t)(ix + 52), (int16_t)(iy + 17), F_SMALL, OPT_CENTER, b);
                    color(q_color(q)); text((int16_t)(ix + 52), (int16_t)(iy + 38), F_SMALL, OPT_CENTER, plat_quality_name(q));
                }
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
            if (d) qvalue((int16_t)(x + 12), (int16_t)(y + 56), F_BIG, d->id, w->index, v);
            else { color(C_INK); text((int16_t)(x + 12), (int16_t)(y + 56), F_BIG, OPT_CENTERY, v); }
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
    box(16, BODY_Y, U.kb == KB_TEXT ? 760 : 992, 88, C_CARD, 8);
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
    if (U.kb == KB_TEXT) {                               /* beside the narrowed card and the entry: 144 px tall */
        button(T_OK, bx, BODY_Y, 108, 144, "OK", C_OK, true); button(T_CANCEL, (int16_t)(bx + 116), BODY_Y, 108, 144, "Cancel", 0x5B6577u, true);
    }
    else { button(T_OK, bx, (int16_t)(y + 54), 344, 119, "OK", C_OK, true); button(T_CANCEL, bx, (int16_t)(y + 176), 344, 119, "Cancel", 0x5B6577u, true); }
}

static void draw_rec(void) {
    const ui_record_t *r = U.rec;
    memset(U.tmap_w, 0, sizeof U.tmap_w);
    if (!r) return;
    const char *why = rec_why_not();
    unsigned used = ui_record_rows_used(r, U.rec_buf);
    box(16, BODY_Y, 992, 64, C_CARD, 8);
    color(C_INK); text(32, BODY_Y + 32, F_BIG, OPT_CENTERY, r->title);
    if (r->rows && r->rows_off != UI_ROWS_ALL) {
        char b[32]; snprintf(b, sizeof b, "In use: %u", used);
        color(C_MUTE); text(330, BODY_Y + 32, F_TEXT, OPT_CENTERY, b);
        button(T_OFF, 462, BODY_Y + 4, 72, 56, "-", C_ACCENT, !why && used > 0);
        button(T_ON, 540, BODY_Y + 4, 72, 56, "+", C_ACCENT, !why && used < r->rows);
    }
    button(T_OK, 700, BODY_Y + 4, 150, 56, U.rec_wait ? "Saving..." : "Save", U.rec_changed ? C_OK : C_ACCENT, !why && !U.rec_wait);
    button(T_CANCEL, 858, BODY_Y + 4, 150, 56, U.rec_changed ? "Cancel" : "Close", 0x5B6577u, true);
    int16_t y = BODY_Y + 72;
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
        button(T_PGUP, 908, (int16_t)(BODY_Y + 72), 100, 160, "Up", C_ACCENT, U.rec_first > 0);
        button(T_PGDN, 908, (int16_t)(BODY_Y + 240), 100, 160, "Down", C_ACCENT, row < r->rows);
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
            button(T_PGUP, 908, (int16_t)(BODY_Y + 72), 100, 160, "Up", C_ACCENT, U.rec_first > 0);
            button(T_PGDN, 908, (int16_t)(BODY_Y + 240), 100, 160, "Down", C_ACCENT, i < r->n_fields);
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
    if (U.scr == UI_DISPLAY && U.dt_mode) {           /* full screen: no header, toasts or navigation */
        draw_display(now);
        (void)eve_frame_end(e);
        U.dirty = U.dt_mode == 2;
        U.last_draw = now;
        return;
    }
    header(now);
    switch (U.scr) {
    case UI_PARAMS: draw_params(); break;
    case UI_EDIT:   draw_edit(); break;
    case UI_LOGIN:  draw_login(now); break;
    case UI_EVENTS: draw_events(); break;
    case UI_SYSTEM: draw_system(now); break;
    case UI_ALARMS: draw_alarms(); break;
    case UI_ALARM_CFG: draw_alarm_cfg(); break;
    case UI_TIERS: draw_tiers(); break;
    case UI_TRENDS: draw_trends(); break;
    case UI_TR_PICK: draw_tr_pick(); break;
    case UI_SERVICE: draw_service(); break;
    case UI_INST: draw_inst(); break;
    case UI_EMIS: draw_emis(); break;
    case UI_CLOCK: draw_clock(); break;
    case UI_APP:   draw_app(); break;
    case UI_REC:   draw_rec(); break;
    case UI_IOB:   draw_iob(); break;
    case UI_CAL:   draw_cal(now); break;
    case UI_DISPLAY: draw_display(now); break;
    case UI_FORCE: draw_force(now); break;
    case UI_DIA: draw_dia(); break;
    case UI_DIA_SLOT: draw_dia_slot(); break;
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
