/* param.c - see param.h */
#include <math.h>
#include <string.h>
#include "param.h"
#include "plat_clock.h"
#include "plat_port.h"
#include "plat_seqlock.h"
#include "iobus.h"

#ifndef PARAM_BLOB_POOL
#define PARAM_BLOB_POOL (16u * 1024u)   /* reader copies of all blocks (old project: ~11 KB of buffers) */
#endif
#ifndef PARAM_BLOBQ_DEPTH
#define PARAM_BLOBQ_DEPTH 16u     /* one FC16 of 123 registers can touch 13 ten-register names */
#endif

/* ---------------- platform parameter storage ---------------- */
static uint8_t  p_mode = PARAM_MODE_LOCAL;
static bool     p_link_lost;
static uint16_t p_comms_timeout_s = 10;
static bool     p_modbus_write_en = true;
static uint32_t p_modbus_allow_ip;
static bool     p_dhcp;
static bool     p_http_redirect = true;
static uint32_t p_ip   = 0xC0A80132u;   /* 192.168.1.50  - DECISION D5 */
static uint32_t p_mask = 0xFFFFFF00u;   /* 255.255.255.0 */
static uint32_t p_gw   = 0xC0A80101u;   /* 192.168.1.1   */
static bool     p_restore;              /* momentary */
static uint32_t p_serial, p_hw_rev;
uint32_t        param_store_status_word; /* written by param_store */
uint32_t        plat_net_status_word;     /* written by the network task (port) */
uint16_t        plat_log_interval_s = 60; /* data-log interval (plat_log) */
uint32_t        plat_log_status_word;     /* written by plat_log */

#define NET (PF_PERSIST | PF_ANY_MODE | PF_NO_MODBUS)
static uint16_t  p_alarm_ack;
static bool      p_fw_local;
static uint32_t  p_fw_min;
static int16_t   p_utc_off;
static uint32_t  p_sntp_ip, p_clock_set;
static uint16_t  p_sntp_min;
extern uint32_t  plat_clock_unix;                                           /* plat_clock.c */
static bool      p_mail_en, p_mail_on_clear;
static uint32_t  p_mail_ip;
static uint16_t  p_mail_port, p_mail_gap;
static uint8_t   p_mail_sec;
static uint8_t   p_mail_pin[32];
static char      p_mail_user[48], p_mail_pass[48], p_mail_from[64], p_mail_to[160], p_site[32];
#define MAILF (PF_PERSIST | PF_ANY_MODE | PF_NO_MODBUS)
extern uint16_t plat_alarm_active_count, plat_alarm_unack_count;            /* plat_alarm.c */
extern uint16_t plat_alarm_active_bits[16], plat_alarm_unack_bits[16], plat_alarm_common;
extern uint8_t  plat_alarm_route[256], plat_alarm_relay_do[4], plat_alarm_relay_mode[4];
extern bool     plat_alarm_relay_inv[4];
void (*param_on_alarm_ack)(uint16_t id, uint8_t src, uint8_t level);

#define IOB_DEFS(K, N) \
    PARAM_BIT(PARAM_ID_IOB_FIRST + 10 * K, "iob" N "_enable", iob_tags[K].enable, PLAT_LVL_ADMIN, PF_PERSIST), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 1, "iob" N "_address", iob_tags[K].address, CT_U8, 1, 247, K + 1, PLAT_LVL_ADMIN, PF_PERSIST, NULL), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 2, "iob" N "_first_do", iob_tags[K].first_do, CT_U8, 1, PLAT_DO_MAX - IOB_OUTS + 1, 1 + IOB_OUTS * K, PLAT_LVL_ADMIN, PF_PERSIST, NULL), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 3, "iob" N "_first_di", iob_tags[K].first_di, CT_U8, 1, PLAT_DI_MAX - IOB_INS + 1, 1 + IOB_INS * K, PLAT_LVL_ADMIN, PF_PERSIST, NULL), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 4, "iob" N "_open_check", iob_tags[K].ol_mask, CT_U8, 0, 255, 255, PLAT_LVL_ADMIN, PF_PERSIST, NULL), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 5, "iob" N "_wire_check", iob_tags[K].wb_mask, CT_U8, 0, 255, 0, PLAT_LVL_ADMIN, PF_PERSIST, NULL), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 6, "iob" N "_status", iob_tags[K].status, CT_U16, 0, 0xFFFF, 0, PLAT_LVL_VIEW, PF_RO, NULL), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 7, "iob" N "_out_faults", iob_tags[K].out_faults, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_VIEW, PF_RO, NULL), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 8, "iob" N "_in_faults", iob_tags[K].in_faults, CT_U8, 0, 255, 0, PLAT_LVL_VIEW, PF_RO, NULL), \
    PARAM_UINT(PARAM_ID_IOB_FIRST + 10 * K + 9, "iob" N "_errors", iob_tags[K].errors, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_VIEW, PF_RO, NULL), \
    PARAM_UINT(PARAM_ID_IOB_LIVE_FIRST + 2 * K, "iob" N "_inputs", iob_tags[K].inputs, CT_U8, 0, 255, 0, PLAT_LVL_VIEW, PF_RO, NULL), \
    PARAM_UINT(PARAM_ID_IOB_LIVE_FIRST + 2 * K + 1, "iob" N "_outputs", iob_tags[K].outputs, CT_U32, 0, 0x3FFFF, 0, PLAT_LVL_VIEW, PF_RO, NULL)

static const param_def_t plat_defs[] = {
    PARAM_UINT(PARAM_ID_CTRL_MODE, "ctrl_mode", p_mode, CT_U8, 0, 1, PARAM_MODE_LOCAL,
               PLAT_LVL_OPERATOR, PF_LOCAL_ONLY | PF_ANY_MODE, NULL),
    PARAM_BIT (PARAM_ID_REMOTE_LINK_LOST, "remote_link_lost", p_link_lost, PLAT_LVL_VIEW, PF_RO),
    PARAM_UINT(PARAM_ID_COMMS_TIMEOUT_S, "comms_timeout", p_comms_timeout_s, CT_U16, 0, 3600, 10,
               PLAT_LVL_ADMIN, NET, "s"),
    PARAM_BIT_D(PARAM_ID_MODBUS_WRITE_EN, "modbus_write_en", p_modbus_write_en, true, PLAT_LVL_ADMIN, NET),
    PARAM_UINT(PARAM_ID_MODBUS_ALLOW_IP, "modbus_allow_ip", p_modbus_allow_ip, CT_U32, 0, 0xFFFFFFFFu, 0,
               PLAT_LVL_ADMIN, NET, NULL),
    PARAM_BIT_D(PARAM_ID_HTTP_REDIRECT, "http_redirect", p_http_redirect, true, PLAT_LVL_ADMIN, NET),
    PARAM_BIT (PARAM_ID_NET_DHCP, "net_dhcp", p_dhcp, PLAT_LVL_ADMIN, NET),
    PARAM_UINT(PARAM_ID_NET_IP,   "net_ip",   p_ip,   CT_U32, 0, 0xFFFFFFFFu, 0xC0A80132u, PLAT_LVL_ADMIN, NET, NULL),
    PARAM_UINT(PARAM_ID_NET_MASK, "net_mask", p_mask, CT_U32, 0, 0xFFFFFFFFu, 0xFFFFFF00u, PLAT_LVL_ADMIN, NET, NULL),
    PARAM_UINT(PARAM_ID_NET_GW,   "net_gw",   p_gw,   CT_U32, 0, 0xFFFFFFFFu, 0xC0A80101u, PLAT_LVL_ADMIN, NET, NULL),
    PARAM_UINT(PARAM_ID_NET_STATUS, "net_status", plat_net_status_word, CT_U32, 0, 0xFFFFFFFFu, 0,
               PLAT_LVL_VIEW, PF_RO, NULL),
    PARAM_BIT (PARAM_ID_RESTORE_DEFAULTS, "restore_defaults", p_restore, PLAT_LVL_ADMIN, PF_ANY_MODE | PF_NO_MODBUS),
    PARAM_UINT(PARAM_ID_SERIAL, "serial", p_serial, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_FACTORY,
               PF_PERSIST | PF_FACTORY | PF_ANY_MODE | PF_NO_MODBUS | PF_DEV, NULL),
    PARAM_UINT(PARAM_ID_HW_REV, "hw_rev", p_hw_rev, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_FACTORY,
               PF_PERSIST | PF_FACTORY | PF_ANY_MODE | PF_NO_MODBUS, NULL),
    PARAM_UINT(PARAM_ID_ALARM_ACK, "alarm_ack", p_alarm_ack, CT_U16, 0, 0xFFFFu, 0, PLAT_LVL_VIEW, PF_ANY_MODE, NULL),
    PARAM_UINT(PARAM_ID_ALARM_ACTIVE, "alarm_active", plat_alarm_active_count, CT_U16, 0, 0xFFFFu, 0, PLAT_LVL_VIEW, PF_RO, NULL),
    PARAM_UINT(PARAM_ID_ALARM_UNACKED, "alarm_unacked", plat_alarm_unack_count, CT_U16, 0, 0xFFFFu, 0, PLAT_LVL_VIEW, PF_RO, NULL),
    PARAM_ARRAY_UINT(PARAM_ID_ALARM_ACTIVE_BITS, "alarm_active_bits", plat_alarm_active_bits, CT_U16, 0, 0xFFFFu, PLAT_LVL_VIEW, PF_RO),
    PARAM_ARRAY_UINT(PARAM_ID_ALARM_UNACK_BITS, "alarm_unack_bits", plat_alarm_unack_bits, CT_U16, 0, 0xFFFFu, PLAT_LVL_VIEW, PF_RO),
    PARAM_UINT(PARAM_ID_ALARM_COMMON, "alarm_common", plat_alarm_common, CT_U16, 0, 0xFFFFu, 0, PLAT_LVL_VIEW, PF_RO, NULL),
    PARAM_ARRAY_UINT(PARAM_ID_ALARM_ROUTE, "alarm_route", plat_alarm_route, CT_U8, 0, 0xBFu, PLAT_LVL_ADMIN, NET),
    PARAM_ARRAY_UINT(PARAM_ID_ALARM_RELAY_DO, "alarm_relay_do", plat_alarm_relay_do, CT_U8, 0, PLAT_DO_MAX, PLAT_LVL_ADMIN, NET),
    PARAM_ARRAY_UINT(PARAM_ID_ALARM_RELAY_MODE, "alarm_relay_mode", plat_alarm_relay_mode, CT_U8, 0, 2, PLAT_LVL_ADMIN, NET),
    PARAM_ARRAY_BOOL(PARAM_ID_ALARM_RELAY_INV, "alarm_relay_inv", plat_alarm_relay_inv, PLAT_LVL_ADMIN, NET),
    PARAM_BIT (PARAM_ID_MAIL_ENABLE, "mail_enable", p_mail_en, PLAT_LVL_ADMIN, MAILF),
    PARAM_UINT(PARAM_ID_MAIL_SERVER, "mail_server_ip", p_mail_ip, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_ADMIN, MAILF, NULL),
    PARAM_UINT(PARAM_ID_MAIL_PORT, "mail_port", p_mail_port, CT_U16, 1, 65535, 587, PLAT_LVL_ADMIN, MAILF, NULL),
    PARAM_UINT(PARAM_ID_MAIL_SECURITY, "mail_security", p_mail_sec, CT_U8, 0, 2, MAIL_SEC_STARTTLS, PLAT_LVL_ADMIN, MAILF, NULL),
    PARAM_BYTES(PARAM_ID_MAIL_PIN, "mail_cert_pin", p_mail_pin, PLAT_LVL_ADMIN, MAILF),
    PARAM_TEXT(PARAM_ID_MAIL_USER, "mail_user", p_mail_user, NULL, PLAT_LVL_ADMIN, MAILF),
    PARAM_TEXT(PARAM_ID_MAIL_PASS, "mail_password", p_mail_pass, NULL, PLAT_LVL_ADMIN, MAILF | PF_SECRET),
    PARAM_TEXT(PARAM_ID_MAIL_FROM, "mail_from", p_mail_from, NULL, PLAT_LVL_ADMIN, MAILF),
    PARAM_TEXT(PARAM_ID_MAIL_TO, "mail_to", p_mail_to, NULL, PLAT_LVL_ADMIN, MAILF),
    PARAM_BIT_D(PARAM_ID_MAIL_ON_CLEAR, "mail_on_clear", p_mail_on_clear, true, PLAT_LVL_ADMIN, MAILF),
    PARAM_UINT(PARAM_ID_MAIL_GAP_S, "mail_gap", p_mail_gap, CT_U16, 0, 3600, 10, PLAT_LVL_ADMIN, MAILF, "s"),
    PARAM_TEXT(PARAM_ID_SITE_NAME, "site_name", p_site, NULL, PLAT_LVL_ADMIN, PF_PERSIST | PF_ANY_MODE),
    PARAM_INT (PARAM_ID_TIME_UTC_OFFSET, "time_utc_offset", p_utc_off, CT_I16, -720, 840, 0, PLAT_LVL_ADMIN, NET, "min"),
    PARAM_UINT(PARAM_ID_SNTP_SERVER, "sntp_server_ip", p_sntp_ip, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_ADMIN, NET, NULL),
    PARAM_UINT(PARAM_ID_SNTP_INTERVAL, "sntp_interval", p_sntp_min, CT_U16, 1, 1440, 60, PLAT_LVL_ADMIN, NET, "min"),
    PARAM_UINT(PARAM_ID_CLOCK_SET, "clock_set", p_clock_set, CT_U32, 0, PLAT_CLOCK_MAX_UNIX, 0, PLAT_LVL_OPERATOR, 0, "s"),
    PARAM_UINT(PARAM_ID_CLOCK_UNIX, "clock_unix", plat_clock_unix, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_VIEW, PF_RO, "s"),
    PARAM_BIT (PARAM_ID_FW_LOCAL_CONFIRM, "fw_local_confirm", p_fw_local, PLAT_LVL_ADMIN, NET),
    PARAM_UINT(PARAM_ID_FW_MIN_VERSION, "fw_min_version", p_fw_min, CT_U32, 0, 0xFFFFFFu, 0, PLAT_LVL_FACTORY, NET, NULL),
    PARAM_UINT(PARAM_ID_LOG_INTERVAL_S, "log_interval", plat_log_interval_s, CT_U16, 0, 3600, 60,
               PLAT_LVL_ADMIN, PF_PERSIST | PF_ANY_MODE, "s"),
    PARAM_UINT(PARAM_ID_LOG_STATUS, "log_status", plat_log_status_word, CT_U32, 0, 0xFFFFFFFFu, 0,
               PLAT_LVL_VIEW, PF_RO, NULL),
    PARAM_UINT(PARAM_ID_STORE_STATUS, "store_status", param_store_status_word, CT_U32, 0, 0xFFFFFFFFu, 0,
               PLAT_LVL_VIEW, PF_RO, NULL),
    /* I/O boards on RS-485 (iobus.h, D27): settings at admin (technician) level, LOCAL only */
    PARAM_UINT(PARAM_ID_IOB_TIMEOUT, "iob_timeout", iob_timeout_ms, CT_U16, 100, 5000, 500, PLAT_LVL_ADMIN, PF_PERSIST, "ms"),
    PARAM_UINT(PARAM_ID_IOB_FILTER, "iob_filter", iob_filter_ms, CT_U8, 0, 100, 8, PLAT_LVL_ADMIN, PF_PERSIST, "ms"),
    IOB_DEFS(0, "1"), IOB_DEFS(1, "2"), IOB_DEFS(2, "3"),
};
#define N_PLAT (sizeof plat_defs / sizeof plat_defs[0])

/* ---------------- table ---------------- */
static const param_def_t *tab[PARAM_MAX];     /* sorted by id */
static plat_val_t         shadow[PARAM_MAX];  /* value at end of last scan, index = tab index */
static size_t             n_tab;
static uint16_t           tab_err_id;
static uint32_t           blob_off[PARAM_MAX];       /* offset in blob_shadow, blobs only */
static uint8_t            blob_shadow[PARAM_BLOB_POOL];
static plat_cmdq_t       *bound_q;
static plat_seqlock_t    *bound_lock;
static volatile uint32_t  last_remote_ms;
static volatile bool      remote_entered;     /* mode just switched to REMOTE */
void (*param_on_restore_defaults)(void);
void (*param_on_change)(const param_def_t *d, plat_val_t old_v, plat_val_t new_v, uint8_t src, uint8_t level);

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

static uint8_t elem_ptype(uint8_t ct) {
    switch (ct) {
    case CT_F32: return PT_FLOAT;
    case CT_BOOL: return PT_BOOL;
    case CT_I8: case CT_I16: case CT_I32: return PT_INT;
    default: return PT_UINT;
    }
}

static bool blob_def_ok(const param_def_t *d) {
    if (d->size == 0 || d->ptype > PT_ARRAY) return false;
    if ((d->flags & PF_RECORD) && (d->ptype != PT_BYTES || d->size > PARAM_RECORD_MAX)) return false;
    if (d->ptype != PT_ARRAY) return d->ctype == CT_BLOB && (d->ptype != PT_TEXT16 || d->size % 2u == 0);
    size_t es = param_ctype_size(d->ctype);
    if (es == 0 || d->size % es != 0) return false;
    switch (elem_ptype(d->ctype)) {
    case PT_FLOAT: return isfinite(d->vmin.f) && isfinite(d->vmax.f) && d->vmin.f <= d->vmax.f;
    case PT_INT:   return d->vmin.i <= d->vmax.i;
    case PT_UINT:  return d->vmin.u <= d->vmax.u;
    default:       return true;
    }
}

static bool def_ok(const param_def_t *d) {
    if (d->id == 0 || d->ptr == NULL || d->name == NULL) return false;
    if (PARAM_IS_BLOB(d)) return blob_def_ok(d);
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

static void record_reset(void);

param_err_t param_init(const param_def_t *app_defs, size_t n_app) {
    record_reset();
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
    uint32_t pool = 0;
    for (size_t i = 0; i < n_tab; ++i) {
        if (!def_ok(tab[i]) || (i > 0 && tab[i]->id == tab[i - 1]->id)) { tab_err_id = tab[i]->id; return PERR_TABLE; }
        shadow[i] = tab[i]->vdef;
        if (PARAM_IS_BLOB(tab[i])) {
            if (pool + tab[i]->size > PARAM_BLOB_POOL) { tab_err_id = tab[i]->id; return PERR_TABLE; }
            blob_off[i] = pool;
            if (tab[i]->dflt) memcpy(blob_shadow + pool, tab[i]->dflt, tab[i]->size);
            else memset(blob_shadow + pool, 0, tab[i]->size);
            pool += (tab[i]->size + 3u) & ~3u;
        }
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
        if (PARAM_IS_BLOB(d)) {
            if (d->dflt) memcpy(d->ptr, d->dflt, d->size); else memset(d->ptr, 0, d->size);
            continue;
        }
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

/* who may write: identical for scalars and blocks */
static param_err_t access_ok(const param_def_t *d, uint8_t src, uint8_t level) {
    if (!d) return PERR_NO_ID;
    if (d->flags & PF_RO) return PERR_READONLY;
    if (src == PLAT_SRC_INTERNAL) return PERR_OK;
    if (d->flags & PF_DEV) return PERR_READONLY;        /* only plat_serial_assign (signed token) */
    if (level < d->level) return PERR_ACCESS;
    if (src == PLAT_SRC_MODBUS && ((d->flags & PF_NO_MODBUS) || !p_modbus_write_en)) return PERR_ACCESS;
    if ((d->flags & PF_LOCAL_ONLY) && src != PLAT_SRC_LOCAL) return PERR_MODE;
    if (!(d->flags & PF_ANY_MODE)) {
        uint8_t mode = p_mode;
        if (src == PLAT_SRC_LOCAL && mode != PARAM_MODE_LOCAL) return PERR_MODE;
        if (src != PLAT_SRC_LOCAL && mode != PARAM_MODE_REMOTE) return PERR_MODE;
    }
    return PERR_OK;
}

static param_err_t check_def(const param_def_t *d, uint8_t kind, plat_val_t v, uint8_t src, uint8_t level,
                             plat_val_t *norm) {
    param_err_t a = access_ok(d, src, level);
    if (a != PERR_OK) return a;
    if (PARAM_IS_BLOB(d)) return PERR_TYPE;
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
            if (param_on_change) param_on_change(d, (plat_val_t){ .u = 0 }, n, c->src, c->level);
            if (param_on_restore_defaults) param_on_restore_defaults();
            else param_set_defaults(PARAM_USER_ONLY);
        }
        p_restore = false;
        return PERR_OK;
    }
    if (d->id == PARAM_ID_CLOCK_SET) {                 /* momentary: hand it to the clock (logged there) */
        static const char *const how[] = { "touchscreen", "web", "modbus", "internal" };
        if (n.u && plat_clock_request(n.u, c->src, c->level, how[c->src & 3u]) != 0) { p_clock_set = 0; return PERR_RANGE; }
        p_clock_set = 0;
        return PERR_OK;
    }
    if (d->id == PARAM_ID_ALARM_ACK) {                 /* momentary command, logged by the alarm manager */
        if (param_on_alarm_ack && n.u) param_on_alarm_ack((uint16_t)n.u, c->src, c->level);
        p_alarm_ack = 0;
        return PERR_OK;
    }
    if (d->id == PARAM_ID_CTRL_MODE && n.u == PARAM_MODE_REMOTE && p_mode != PARAM_MODE_REMOTE)
        remote_entered = true;                        /* grace: timing starts at the next service */
    plat_val_t old = param_tag_get(d);
    param_tag_set(d, n);
    if (param_on_change) {
        plat_val_t now_v = param_tag_get(d);
        if (old.u != now_v.u) param_on_change(d, old, now_v, c->src, c->level);
    }
    return PERR_OK;
}

void param_publish(void) {
    for (size_t i = 0; i < n_tab; ++i) {
        if (PARAM_IS_BLOB(tab[i])) memcpy(blob_shadow + blob_off[i], tab[i]->ptr, tab[i]->size);
        else shadow[i] = param_tag_get(tab[i]);
    }
}

bool param_read(uint16_t id, plat_val_t *out) {
    long i = find_index(id);
    if (i < 0 || PARAM_IS_BLOB(tab[i])) return false;
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

/* ---------------- servers: posting writes ---------------- */
void param_bind(plat_cmdq_t *cmdq, plat_seqlock_t *snap_lock) { bound_q = cmdq; bound_lock = snap_lock; }

param_err_t param_post(uint16_t id, uint8_t kind, plat_val_t v, uint8_t src, uint8_t level) {
    param_err_t e = param_check(id, kind, v, src, level);
    if (e != PERR_OK) return e;
    plat_cmd_t c = { .id = id, .src = src, .kind = kind, .level = level, .v = v };
    if (!bound_q || !plat_cmdq_post(bound_q, &c)) return PERR_BUSY;
    return PERR_OK;
}

/* ---------------- blocks ---------------- */
typedef struct {
    uint16_t id, off, len;
    uint8_t  src, level;
    bool     whole;
    uint8_t  data[PARAM_BLOB_MAX_WRITE];
} blobcmd_t;

static blobcmd_t blobq[PARAM_BLOBQ_DEPTH];
static volatile uint32_t bq_head, bq_tail;
_Static_assert((PARAM_BLOBQ_DEPTH & (PARAM_BLOBQ_DEPTH - 1u)) == 0u, "PARAM_BLOBQ_DEPTH must be a power of 2");

/* one element of an array, decoded and range-checked */
static bool elem_ok(const param_def_t *d, const uint8_t *p) {
    param_def_t t = *d;
    t.ptype = elem_ptype(d->ctype);
    union { uint8_t b[4]; uint32_t u; } tmp = { { 0 } };
    memcpy(tmp.b, p, param_ctype_size(d->ctype));
    if (t.ptype == PT_BOOL) return tmp.b[0] <= 1u;     /* check the byte before reading it as a bool */
    t.ptr = tmp.b;
    plat_val_t v = param_tag_get(&t);
    switch (t.ptype) {
    case PT_FLOAT: return isfinite(v.f) && v.f >= d->vmin.f && v.f <= d->vmax.f;
    case PT_BOOL:  return tmp.b[0] <= 1u;
    case PT_INT:   return v.i >= d->vmin.i && v.i <= d->vmax.i;
    default:       return v.u >= d->vmin.u && v.u <= d->vmax.u;
    }
}

bool param_blob_valid(const param_def_t *d, const uint8_t *data, uint16_t off, uint16_t len) {
    switch (d->ptype) {
    case PT_BYTES: return true;
    case PT_TEXT:
        for (uint16_t k = 0; k < len; ++k) if (data[k] < 0x20u || data[k] > 0x7Eu) return false;
        return true;
    case PT_TEXT16:
        if (len % 2u) return false;
        for (uint16_t k = 0; k < len; k += 2) {
            uint16_t u = (uint16_t)(data[k] | (data[k + 1] << 8));
            if (u < 0x20u || (u >= 0x7Fu && u < 0xA0u) || (u >= 0xD800u && u <= 0xDFFFu)) return false;
        }
        return true;
    case PT_ARRAY: {
        size_t es = param_ctype_size(d->ctype);
        if (off % es || len % es) return false;
        for (uint16_t k = 0; k < len; k += (uint16_t)es) if (!elem_ok(d, data + k)) return false;
        return true;
    }
    default: return false;
    }
}

/* A whole stored image of a block (used by param_store on load). */
bool param_blob_image_ok(const param_def_t *d, const uint8_t *data) {
    if (d->ptype == PT_TEXT) {                 /* NUL-terminated within size, printable before it */
        uint16_t n = 0;
        while (n < d->size && data[n]) n++;
        return n < d->size && param_blob_valid(d, data, 0, n);
    }
    if (d->ptype == PT_TEXT16) {
        uint16_t n = 0;
        while (n + 1u < d->size && (data[n] | data[n + 1])) n = (uint16_t)(n + 2);
        return n + 1u < d->size && param_blob_valid(d, data, 0, n);
    }
    return param_blob_valid(d, data, 0, d->size);
}

param_err_t param_blob_check(uint16_t id, uint16_t off, const void *data, uint16_t len, bool whole,
                             uint8_t src, uint8_t level) {
    const param_def_t *d = param_find(id);
    param_err_t a = access_ok(d, src, level);
    if (a != PERR_OK) return a;
    if (!PARAM_IS_BLOB(d)) return PERR_TYPE;
    if (d->flags & PF_RECORD) return PERR_TYPE;        /* records: whole images only (param_record_post) */
    if (len > PARAM_BLOB_MAX_WRITE) return PERR_RANGE;
    if (whole) off = 0;
    if ((d->ptype == PT_TEXT || d->ptype == PT_TEXT16) && !whole) return PERR_TYPE;
    uint32_t room = d->size;
    if (d->ptype == PT_TEXT) room -= 1u;              /* keep the terminator */
    if (d->ptype == PT_TEXT16) room -= 2u;
    if ((uint32_t)off + len > room) return PERR_RANGE;
    if (!param_blob_valid(d, data, off, len)) return PERR_RANGE;
    return PERR_OK;
}

param_err_t param_blob_post_many(const param_blob_write_t *w, size_t n, uint8_t src, uint8_t level) {
    for (size_t k = 0; k < n; ++k) {
        param_err_t e = param_blob_check(w[k].id, w[k].off, w[k].data, w[k].len, w[k].whole, src, level);
        if (e != PERR_OK) return e;
    }
    bool ok;
    PLAT_CRIT_ENTER();
    ok = (uint32_t)(bq_head - bq_tail) + n <= PARAM_BLOBQ_DEPTH;
    for (size_t k = 0; ok && k < n; ++k) {
        blobcmd_t *b = &blobq[bq_head % PARAM_BLOBQ_DEPTH];
        b->id = w[k].id; b->off = w[k].whole ? 0 : w[k].off; b->len = w[k].len; b->src = src; b->level = level;
        b->whole = w[k].whole;
        memcpy(b->data, w[k].data, w[k].len);
        bq_head++;
    }
    PLAT_CRIT_EXIT();
    return ok ? PERR_OK : PERR_BUSY;
}

param_err_t param_blob_post(uint16_t id, uint16_t off, const void *data, uint16_t len, bool whole,
                            uint8_t src, uint8_t level) {
    param_blob_write_t w = { .id = id, .off = off, .len = len, .whole = whole, .data = data };
    return param_blob_post_many(&w, 1, src, level);
}

param_err_t param_post_many(const plat_cmd_t *c, size_t n) {
    for (size_t k = 0; k < n; ++k) {
        param_err_t e = param_check(c[k].id, c[k].kind, c[k].v, c[k].src, c[k].level);
        if (e != PERR_OK) return e;
    }
    if (n == 0) return PERR_OK;
    if (!bound_q || !plat_cmdq_post_batch(bound_q, c, (uint32_t)n)) return PERR_BUSY;
    return PERR_OK;
}

/* ---------------- records (one write in flight) ---------------- */
enum { RS_FREE = 0, RS_FILLING, RS_QUEUED, RS_DONE };
static struct {
    volatile uint8_t state;
    uint16_t   id, len;
    uint8_t    src, level;
    uint32_t   ticket;
    param_err_t result;
    char       why[80];
    uint8_t    data[PARAM_RECORD_MAX];
} rq;
static uint32_t rq_next_ticket = 1;
static void record_reset(void) { memset(&rq, 0, sizeof rq); }

static param_err_t record_access(const param_def_t *d, uint16_t len, uint8_t src, uint8_t level) {
    param_err_t a = access_ok(d, src, level);
    if (a != PERR_OK) return a;
    if (!(d->flags & PF_RECORD) || !PARAM_IS_BLOB(d)) return PERR_TYPE;
    if (len != d->size || len > PARAM_RECORD_MAX) return PERR_RANGE;
    return PERR_OK;
}

param_err_t param_record_check(uint16_t id, uint8_t src, uint8_t level) {
    const param_def_t *d = param_find(id);
    return record_access(d, d ? d->size : 0, src, level);
}

param_err_t param_record_post(uint16_t id, const void *data, uint16_t len, uint8_t src, uint8_t level, uint32_t *ticket) {
    const param_def_t *d = param_find(id);
    param_err_t e = record_access(d, len, src, level);
    if (e != PERR_OK) return e;
    bool ok;
    PLAT_CRIT_ENTER();
    ok = rq.state != RS_FILLING && rq.state != RS_QUEUED;
    if (ok) rq.state = RS_FILLING;
    PLAT_CRIT_EXIT();
    if (!ok) return PERR_BUSY;
    memcpy(rq.data, data, len);
    rq.id = id; rq.len = len; rq.src = src; rq.level = level;
    rq.why[0] = 0;
    PLAT_CRIT_ENTER();
    rq.ticket = rq_next_ticket++;
    if (rq_next_ticket == 0) rq_next_ticket = 1;
    if (ticket) *ticket = rq.ticket;
    rq.state = RS_QUEUED;
    PLAT_CRIT_EXIT();
    return PERR_OK;
}

bool param_record_result(uint32_t ticket, param_err_t *e, char *why, size_t cap) {
    bool done;
    PLAT_CRIT_ENTER();
    done = rq.state == RS_DONE && rq.ticket == ticket;
    PLAT_CRIT_EXIT();
    if (!done) return false;
    if (e) *e = rq.result;
    if (why && cap) { strncpy(why, rq.why, cap - 1u); why[cap - 1u] = 0; }
    return true;
}

void param_record_drain(void) {
    if (rq.state != RS_QUEUED) return;
    const param_def_t *d = param_find(rq.id);
    param_err_t e = record_access(d, rq.len, rq.src, rq.level);   /* the mode may have changed since */
    rq.why[0] = 0;
    if (e == PERR_OK && d->check_block && d->check_block(d->id, rq.data, rq.why, sizeof rq.why) != 0) {
        e = PERR_INTERLOCK;
        if (!rq.why[0]) strcpy(rq.why, "refused by the application");
    }
    rq.why[sizeof rq.why - 1u] = 0;
    if (e == PERR_OK) {
        bool same = memcmp(d->ptr, rq.data, rq.len) == 0;
        memcpy(d->ptr, rq.data, rq.len);
        if (!same && param_on_change) param_on_change(d, (plat_val_t){ .u = 0 }, (plat_val_t){ .u = 0 }, rq.src, rq.level);
    }
    rq.result = e;
    PLAT_CRIT_ENTER();
    rq.state = RS_DONE;
    PLAT_CRIT_EXIT();
}

void param_blob_drain(void) {
    param_record_drain();
    for (unsigned n = 0; n < PARAM_BLOBQ_DEPTH; ++n) {   /* everything queued before this scan */
        blobcmd_t b;
        bool have;
        PLAT_CRIT_ENTER();
        have = bq_head != bq_tail;
        if (have) { b = blobq[bq_tail % PARAM_BLOBQ_DEPTH]; bq_tail++; }
        PLAT_CRIT_EXIT();
        if (!have) return;
        /* checked again: the mode or Modbus permission may have changed since */
        if (param_blob_check(b.id, b.off, b.data, b.len, b.whole, b.src, b.level) != PERR_OK) continue;
        const param_def_t *d = param_find(b.id);
        uint8_t *dst = d->ptr;
        if (param_on_change) {                       /* only report a real change */
            bool same = b.whole ? (memcmp(dst, b.data, b.len) == 0 && (b.len == d->size || dst[b.len] == 0))
                                : memcmp(dst + b.off, b.data, b.len) == 0;
            if (b.whole) memset(dst, 0, d->size);
            memcpy(dst + b.off, b.data, b.len);
            if (!same) param_on_change(d, (plat_val_t){ .u = 0 }, (plat_val_t){ .u = 0 }, b.src, b.level);
            continue;
        }
        if (b.whole) memset(dst, 0, d->size);
        memcpy(dst + b.off, b.data, b.len);
    }
}

static param_err_t read_blob(uint16_t id, uint16_t off, void *buf, uint16_t len, bool secret_ok) {
    long i = find_index(id);
    if (i < 0) return PERR_NO_ID;
    if ((tab[i]->flags & PF_SECRET) && !secret_ok) return PERR_ACCESS;
    if (!PARAM_IS_BLOB(tab[i])) return PERR_TYPE;
    if ((uint32_t)off + len > tab[i]->size) return PERR_RANGE;
    const uint8_t *src = blob_shadow + blob_off[i] + off;
    if (!bound_lock) { memcpy(buf, src, len); return PERR_OK; }
    return plat_seq_read(bound_lock, buf, src, len, 8) ? PERR_OK : PERR_BUSY;
}
param_err_t param_read_blob(uint16_t id, uint16_t off, void *buf, uint16_t len) { return read_blob(id, off, buf, len, false); }
param_err_t param_read_secret(uint16_t id, void *buf, uint16_t len) { return read_blob(id, 0, buf, len, true); }
