/* param.c - see param.h */
#include <math.h>
#include <string.h>
#include "param.h"

/* ---------------- platform parameter storage ---------------- */
static uint8_t  p_mode = PARAM_MODE_LOCAL;
static bool     p_link_lost;
static uint16_t p_comms_timeout_s = 10;
static bool     p_modbus_write_en = true;
static uint32_t p_modbus_allow_ip;
static bool     p_dhcp;
static uint32_t p_ip   = 0xC0A80132u;   /* 192.168.1.50  - DECISION D5 */
static uint32_t p_mask = 0xFFFFFF00u;   /* 255.255.255.0 */
static uint32_t p_gw   = 0xC0A80101u;   /* 192.168.1.1   */
static bool     p_restore;              /* momentary */
static uint32_t p_serial, p_hw_rev;
uint32_t        param_store_status_word; /* written by param_store */

#define NET (PF_PERSIST | PF_ANY_MODE | PF_NO_MODBUS)
static const param_def_t plat_defs[] = {
    PARAM_UINT(PARAM_ID_CTRL_MODE, "ctrl_mode", p_mode, CT_U8, 0, 1, PARAM_MODE_LOCAL,
               PLAT_LVL_OPERATOR, PF_LOCAL_ONLY | PF_ANY_MODE, NULL),
    PARAM_BIT (PARAM_ID_REMOTE_LINK_LOST, "remote_link_lost", p_link_lost, PLAT_LVL_VIEW, PF_RO),
    PARAM_UINT(PARAM_ID_COMMS_TIMEOUT_S, "comms_timeout", p_comms_timeout_s, CT_U16, 0, 3600, 10,
               PLAT_LVL_ADMIN, NET, "s"),
    PARAM_BIT_D(PARAM_ID_MODBUS_WRITE_EN, "modbus_write_en", p_modbus_write_en, true, PLAT_LVL_ADMIN, NET),
    PARAM_UINT(PARAM_ID_MODBUS_ALLOW_IP, "modbus_allow_ip", p_modbus_allow_ip, CT_U32, 0, 0xFFFFFFFFu, 0,
               PLAT_LVL_ADMIN, NET, NULL),
    PARAM_BIT (PARAM_ID_NET_DHCP, "net_dhcp", p_dhcp, PLAT_LVL_ADMIN, NET),
    PARAM_UINT(PARAM_ID_NET_IP,   "net_ip",   p_ip,   CT_U32, 0, 0xFFFFFFFFu, 0xC0A80132u, PLAT_LVL_ADMIN, NET, NULL),
    PARAM_UINT(PARAM_ID_NET_MASK, "net_mask", p_mask, CT_U32, 0, 0xFFFFFFFFu, 0xFFFFFF00u, PLAT_LVL_ADMIN, NET, NULL),
    PARAM_UINT(PARAM_ID_NET_GW,   "net_gw",   p_gw,   CT_U32, 0, 0xFFFFFFFFu, 0xC0A80101u, PLAT_LVL_ADMIN, NET, NULL),
    PARAM_BIT (PARAM_ID_RESTORE_DEFAULTS, "restore_defaults", p_restore, PLAT_LVL_ADMIN, PF_ANY_MODE | PF_NO_MODBUS),
    PARAM_UINT(PARAM_ID_SERIAL, "serial", p_serial, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_FACTORY,
               PF_PERSIST | PF_FACTORY | PF_ANY_MODE | PF_NO_MODBUS, NULL),
    PARAM_UINT(PARAM_ID_HW_REV, "hw_rev", p_hw_rev, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_FACTORY,
               PF_PERSIST | PF_FACTORY | PF_ANY_MODE | PF_NO_MODBUS, NULL),
    PARAM_UINT(PARAM_ID_STORE_STATUS, "store_status", param_store_status_word, CT_U32, 0, 0xFFFFFFFFu, 0,
               PLAT_LVL_VIEW, PF_RO, NULL),
};
#define N_PLAT (sizeof plat_defs / sizeof plat_defs[0])

/* ---------------- table ---------------- */
static const param_def_t *tab[PARAM_MAX];     /* sorted by id */
static plat_val_t         shadow[PARAM_MAX];  /* value at end of last scan, index = tab index */
static size_t             n_tab;
static uint16_t           tab_err_id;
static volatile uint32_t  last_remote_ms;
static volatile bool      remote_entered;     /* mode just switched to REMOTE */
void (*param_on_restore_defaults)(void);

size_t param_ctype_size(uint8_t ct) {
    switch (ct) {
    case CT_BOOL: case CT_U8: case CT_I8: return 1;
    case CT_U16: case CT_I16: return 2;
    case CT_U32: case CT_I32: case CT_F32: return 4;
    default: return 0;
    }
}

/* limits of a C type, as int64 */
static bool ct_limits(uint8_t ct, int64_t *lo, int64_t *hi) {
    switch (ct) {
    case CT_BOOL: *lo = 0; *hi = 1; return true;
    case CT_U8:  *lo = 0; *hi = 255; return true;
    case CT_I8:  *lo = -128; *hi = 127; return true;
    case CT_U16: *lo = 0; *hi = 65535; return true;
    case CT_I16: *lo = -32768; *hi = 32767; return true;
    case CT_U32: *lo = 0; *hi = 4294967295LL; return true;
    case CT_I32: *lo = -2147483648LL; *hi = 2147483647LL; return true;
    default: return false;
    }
}

static int64_t as_i64(uint8_t pt, plat_val_t v) {
    switch (pt) {
    case PT_BOOL: return v.b ? 1 : 0;
    case PT_INT:  return v.i;
    case PT_UINT: return (int64_t)v.u;
    default:      return 0;
    }
}

static bool def_ok(const param_def_t *d) {
    if (d->id == 0 || d->ptr == NULL || d->name == NULL) return false;
    if (param_ctype_size(d->ctype) == 0) return false;
    if (d->ptype == PT_FLOAT) {
        if (d->ctype != CT_F32) return false;
        if (!isfinite(d->vmin.f) || !isfinite(d->vmax.f) || !isfinite(d->vdef.f)) return false;
        return d->vmin.f <= d->vmax.f && d->vdef.f >= d->vmin.f && d->vdef.f <= d->vmax.f;
    }
    if (d->ctype == CT_F32) return false;
    if (d->ptype == PT_BOOL && d->ctype != CT_BOOL) return false;
    if (d->ptype > PT_FLOAT) return false;
    int64_t lo, hi;
    if (!ct_limits(d->ctype, &lo, &hi)) return false;
    int64_t mn = as_i64(d->ptype, d->vmin), mx = as_i64(d->ptype, d->vmax), df = as_i64(d->ptype, d->vdef);
    return mn <= mx && mn >= lo && mx <= hi && df >= mn && df <= mx;
}

param_err_t param_init(const param_def_t *app_defs, size_t n_app) {
    n_tab = 0;
    tab_err_id = 0;
    if (N_PLAT + n_app > PARAM_MAX) { tab_err_id = 0xFFFF; return PERR_TABLE; }
    for (size_t k = 0; k < N_PLAT; ++k) tab[n_tab++] = &plat_defs[k];
    for (size_t k = 0; k < n_app; ++k) {
        if (app_defs[k].id < PARAM_ID_APP_FIRST) { tab_err_id = app_defs[k].id; return PERR_TABLE; }
        tab[n_tab++] = &app_defs[k];
    }
    /* insertion sort by id (runs once at boot, n <= 512) */
    for (size_t i = 1; i < n_tab; ++i) {
        const param_def_t *x = tab[i]; size_t j = i;
        while (j > 0 && tab[j - 1]->id > x->id) { tab[j] = tab[j - 1]; --j; }
        tab[j] = x;
    }
    for (size_t i = 0; i < n_tab; ++i) {
        if (!def_ok(tab[i]) || (i > 0 && tab[i]->id == tab[i - 1]->id)) { tab_err_id = tab[i]->id; return PERR_TABLE; }
        shadow[i] = tab[i]->vdef;
    }
    return PERR_OK;
}

uint16_t param_table_error(void) { return tab_err_id; }
size_t param_count(void) { return n_tab; }
const param_def_t *param_at(size_t i) { return i < n_tab ? tab[i] : NULL; }

static long find_index(uint16_t id) {
    size_t lo = 0, hi = n_tab;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (tab[mid]->id == id) return (long)mid;
        if (tab[mid]->id < id) lo = mid + 1; else hi = mid;
    }
    return -1;
}

const param_def_t *param_find(uint16_t id) { long i = find_index(id); return i < 0 ? NULL : tab[i]; }

/* ---------------- tag access ---------------- */
plat_val_t param_tag_get(const param_def_t *d) {
    plat_val_t v = { 0 };
    switch (d->ctype) {
    case CT_BOOL: v.b = *(const bool *)d->ptr; return v;
    case CT_F32:  v.f = *(const float *)d->ptr; return v;
    default: break;
    }
    int64_t x = 0;
    switch (d->ctype) {
    case CT_U8:  x = *(const uint8_t *)d->ptr; break;
    case CT_I8:  x = *(const int8_t *)d->ptr; break;
    case CT_U16: x = *(const uint16_t *)d->ptr; break;
    case CT_I16: x = *(const int16_t *)d->ptr; break;
    case CT_U32: x = *(const uint32_t *)d->ptr; break;
    case CT_I32: x = *(const int32_t *)d->ptr; break;
    default: break;
    }
    if (d->ptype == PT_UINT) v.u = (uint32_t)x;
    else if (d->ptype == PT_BOOL) v.b = x != 0;
    else v.i = (int32_t)x;
    return v;
}

void param_tag_set(const param_def_t *d, plat_val_t v) {
    if (d->ctype == CT_F32) { *(float *)d->ptr = v.f; return; }
    if (d->ctype == CT_BOOL) { *(bool *)d->ptr = (d->ptype == PT_BOOL) ? v.b : (as_i64(d->ptype, v) != 0); return; }
    int64_t x = as_i64(d->ptype, v);
    switch (d->ctype) {
    case CT_U8:  *(uint8_t *)d->ptr = (uint8_t)x; break;
    case CT_I8:  *(int8_t *)d->ptr = (int8_t)x; break;
    case CT_U16: *(uint16_t *)d->ptr = (uint16_t)x; break;
    case CT_I16: *(int16_t *)d->ptr = (int16_t)x; break;
    case CT_U32: *(uint32_t *)d->ptr = (uint32_t)x; break;
    case CT_I32: *(int32_t *)d->ptr = (int32_t)x; break;
    default: break;
    }
}

void param_set_defaults(int which) {
    for (size_t i = 0; i < n_tab; ++i) {
        const param_def_t *d = tab[i];
        if (which == PARAM_USER_ONLY && (d->flags & PF_FACTORY)) continue;
        param_tag_set(d, d->vdef);
    }
}

/* ---------------- checks ---------------- */

/* Convert an incoming value to the parameter's own representation. */
static param_err_t normalise(const param_def_t *d, uint8_t kind, plat_val_t in, plat_val_t *out) {
    int64_t x;
    switch (kind) {
    case PLAT_VAL_BOOL: x = in.b ? 1 : 0; break;
    case PLAT_VAL_I32:  x = in.i; break;
    case PLAT_VAL_U32:  x = (int64_t)in.u; break;
    case PLAT_VAL_F32:
        if (d->ptype != PT_FLOAT) return PERR_TYPE;   /* no silent float -> int rounding */
        if (!isfinite(in.f)) return PERR_RANGE;
        out->f = in.f;
        return PERR_OK;
    default: return PERR_TYPE;
    }
    switch (d->ptype) {
    case PT_BOOL:  if (x != 0 && x != 1) return PERR_RANGE; out->b = x != 0; return PERR_OK;
    case PT_INT:   if (x < -2147483648LL || x > 2147483647LL) return PERR_RANGE; out->i = (int32_t)x; return PERR_OK;
    case PT_UINT:  if (x < 0 || x > 4294967295LL) return PERR_RANGE; out->u = (uint32_t)x; return PERR_OK;
    case PT_FLOAT: out->f = (float)x; return PERR_OK;
    default:       return PERR_TYPE;
    }
}

static bool in_range(const param_def_t *d, plat_val_t v) {
    switch (d->ptype) {
    case PT_FLOAT: return v.f >= d->vmin.f && v.f <= d->vmax.f;
    case PT_BOOL:  return true;
    case PT_INT:   return v.i >= d->vmin.i && v.i <= d->vmax.i;
    case PT_UINT:  return v.u >= d->vmin.u && v.u <= d->vmax.u;
    default:       return false;
    }
}

static param_err_t check_def(const param_def_t *d, uint8_t kind, plat_val_t v, uint8_t src, uint8_t level,
                             plat_val_t *norm) {
    if (!d) return PERR_NO_ID;
    if (d->flags & PF_RO) return PERR_READONLY;
    if (src != PLAT_SRC_INTERNAL) {
        if (level < d->level) return PERR_ACCESS;
        if (src == PLAT_SRC_MODBUS && ((d->flags & PF_NO_MODBUS) || !p_modbus_write_en)) return PERR_ACCESS;
        if ((d->flags & PF_LOCAL_ONLY) && src != PLAT_SRC_LOCAL) return PERR_MODE;
        if (!(d->flags & PF_ANY_MODE)) {
            uint8_t mode = p_mode;
            if (src == PLAT_SRC_LOCAL && mode != PARAM_MODE_LOCAL) return PERR_MODE;
            if (src != PLAT_SRC_LOCAL && mode != PARAM_MODE_REMOTE) return PERR_MODE;
        }
    }
    param_err_t e = normalise(d, kind, v, norm);
    if (e != PERR_OK) return e;
    return in_range(d, *norm) ? PERR_OK : PERR_RANGE;
}

param_err_t param_check(uint16_t id, uint8_t kind, plat_val_t v, uint8_t src, uint8_t level) {
    plat_val_t n;
    return check_def(param_find(id), kind, v, src, level, &n);
}

param_err_t param_apply(const plat_cmd_t *c) {
    const param_def_t *d = param_find(c->id);
    plat_val_t n;
    param_err_t e = check_def(d, c->kind, c->v, c->src, c->level, &n);
    if (e != PERR_OK) return e;
    if (d->interlock && d->interlock(d->id, n) != 0) return PERR_INTERLOCK;

    if (d->id == PARAM_ID_RESTORE_DEFAULTS) {          /* momentary command */
        if (n.b) {
            if (param_on_restore_defaults) param_on_restore_defaults();
            else param_set_defaults(PARAM_USER_ONLY);
        }
        p_restore = false;
        return PERR_OK;
    }
    if (d->id == PARAM_ID_CTRL_MODE && n.u == PARAM_MODE_REMOTE && p_mode != PARAM_MODE_REMOTE)
        remote_entered = true;                        /* grace: timing starts at the next service */
    param_tag_set(d, n);
    return PERR_OK;
}

void param_publish(void) {
    for (size_t i = 0; i < n_tab; ++i) shadow[i] = param_tag_get(tab[i]);
}

bool param_read(uint16_t id, plat_val_t *out) {
    long i = find_index(id);
    if (i < 0) return false;
    *out = shadow[i];      /* one aligned word: atomic */
    return true;
}

/* ---------------- arbitration ---------------- */
uint8_t param_ctrl_mode(void) { return p_mode; }
bool param_remote_link_lost(void) { return p_link_lost; }

void param_comms_activity(uint8_t src, uint32_t now_ms) {
    if (src == PLAT_SRC_WEB || src == PLAT_SRC_MODBUS) last_remote_ms = now_ms;
}

void param_scan_service(uint32_t now_ms) {
    if (p_mode != PARAM_MODE_REMOTE || p_comms_timeout_s == 0) { p_link_lost = false; return; }
    if (remote_entered) { last_remote_ms = now_ms; remote_entered = false; }
    p_link_lost = (uint32_t)(now_ms - last_remote_ms) > (uint32_t)p_comms_timeout_s * 1000u;
}
