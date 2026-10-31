/* modbus.c - see modbus.h */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "modbus.h"
#include "plat_config.h"
#include "plat_state.h"
#include "fw_update.h"
#include "plat_log.h"
#include "plat_port.h"
#include "plat_alarm.h"

#ifndef MB_MAX_ENTRIES
#define MB_MAX_ENTRIES 512
#endif
#define MB_MAX_READ_REGS   125u
#define MB_MAX_READ_BITS   2000u
#define MB_MAX_WRITE_REGS  123u
#define MB_MAX_WRITE_BITS  1968u
#define MB_MAX_OPS         128u

enum { EX_FUNC = 1, EX_ADDR = 2, EX_VALUE = 3, EX_FAIL = 4, EX_BUSY = 6 };

typedef struct { const mb_map_t *m; const param_def_t *d; uint16_t span; } ent_t;

static ent_t      idx[MB_MAX_ENTRIES];
static size_t     n_idx;
static mb_ident_t ident = { "CEMS", "CEMS Supervisor", "0.0", PLAT_LVL_OPERATOR, 0 };
static mb_stats_t st;

/* ---------------- the platform's own block (modbus.h) ---------------- */
uint16_t plat_mb_heartbeat, plat_mb_link_age_s = 0xFFFFu, plat_mb_block_version = PLAT_MB_BLOCK_VERSION;
uint16_t plat_mb_map_version, plat_mb_api_version = PLAT_API_VERSION;
uint32_t plat_mb_fw_version, plat_mb_uptime_s;
char     plat_mb_product[32], plat_mb_vendor[16], plat_mb_fw_text[16];
uint16_t plat_mb_forced;

#define B(OFF) (PLAT_MB_BLOCK_ADDR + (OFF))
static const mb_map_t block[] = {
    MB_ENTRY(MB_INPUT, B(0),  PARAM_ID_MB_BLOCK_VERSION, MBF_U16),
    MB_ENTRY(MB_INPUT, B(1),  PARAM_ID_MB_MAP_VERSION,   MBF_U16),
    MB_ENTRY(MB_INPUT, B(2),  PARAM_ID_HEARTBEAT,        MBF_U16),
    MB_ENTRY(MB_INPUT, B(3),  PARAM_ID_STATE,            MBF_U16),
    MB_ENTRY(MB_INPUT, B(4),  PARAM_ID_LINK_AGE_S,       MBF_U16),
    MB_ENTRY(MB_INPUT, B(5),  PARAM_ID_ALARM_ACTIVE,     MBF_U16),
    MB_ENTRY(MB_INPUT, B(6),  PARAM_ID_ALARM_COMMON,     MBF_U16),
    MB_ENTRY(MB_INPUT, B(7),  PARAM_ID_HW_REV,           MBF_U32),
    MB_ENTRY(MB_INPUT, B(9),  PARAM_ID_SERIAL,           MBF_U32),
    MB_ENTRY(MB_INPUT, B(11), PARAM_ID_FW_VERSION_NUM,   MBF_U32),
    MB_ENTRY(MB_INPUT, B(13), PARAM_ID_API_VERSION,      MBF_U16),
    MB_ENTRY(MB_INPUT, B(14), PARAM_ID_UPTIME_S,         MBF_U32),
    MB_ENTRY(MB_INPUT, B(16), PARAM_ID_SUPPLY_MV,        MBF_U32),
    MB_ENTRY(MB_INPUT, B(18), PARAM_ID_STATE_SINCE,      MBF_U32),
    MB_ENTRY(MB_INPUT, B(20), PARAM_ID_MEAS_Q,           MBF_ARRAY),
    MB_ENTRY(MB_INPUT, B(52), PARAM_ID_PRODUCT,          MBF_BYTES),
    MB_ENTRY(MB_INPUT, B(68), PARAM_ID_VENDOR,           MBF_BYTES),
    MB_ENTRY(MB_INPUT, B(76), PARAM_ID_FW_TEXT,          MBF_BYTES),
    MB_ENTRY(MB_INPUT, B(84), PARAM_ID_CLOCK_UNIX,       MBF_U32),     /* layout 2 (CIP-02.6) */
    MB_ENTRY(MB_INPUT, B(86), PARAM_ID_CLOCK_SYNCED,     MBF_U16),
    MB_ENTRY(MB_INPUT, B(87), PARAM_ID_CLOCK_SOURCE,     MBF_U16),
    MB_ENTRY(MB_INPUT, B(88), PARAM_ID_CLOCK_SYNC_TIME,  MBF_U32),
    MB_ENTRY(MB_INPUT, B(90), PARAM_ID_MB_FORCED,        MBF_U16),     /* layout 3 */
};
#define N_BLOCK (sizeof block / sizeof block[0])

static void copy_text(char *dst, size_t cap, const char *src) {
    memset(dst, 0, cap);
    for (size_t k = 0; src && src[k] && k < cap; ++k) dst[k] = src[k];   /* NUL padded, not terminated when full */
}

static void block_ident(void) {
    copy_text(plat_mb_product, sizeof plat_mb_product, ident.product);
    copy_text(plat_mb_vendor, sizeof plat_mb_vendor, ident.vendor);
    copy_text(plat_mb_fw_text, sizeof plat_mb_fw_text, ident.revision);
    plat_mb_fw_version = ident.revision ? fw_parse_version(ident.revision) : 0u;
    plat_mb_map_version = ident.map_version;
    plat_mb_block_version = PLAT_MB_BLOCK_VERSION;
    plat_mb_api_version = PLAT_API_VERSION;
}

/* the standard CEMS block's sources (below) */
enum {                                       /* the emissions engine's parameter IDs (param.h with emis.h) */
    STD_ENABLE = 400, STD_T_REF = 401, STD_P_REF = 402, STD_DILUENT = 403, STD_REF_PCT = 404,
    STD_PERIOD = 416, STD_OPERATING = 422, STD_CFG_ERROR = 423, STD_UNIT = 431,
    STD_BANK0 = 450, STD_Q_IN = 460, STD_Q_CALC = 461, STD_Q_MASS = 462, STD_Q_AVG = 463,
    STD_Q_DAY = 464, STD_Q_RUN = 465, STD_STATUS = 466, STD_DISCARDED = 467, STD_DAYS_INVALID = 468
};
#define STD_SLOTS       32u
#define STD_COMPONENTS  30u                  /* 43/14 component objects: 0x85 + 4n stays below 0x100 */
#define STD_VERSION     1u
#define STD_EMIS_ST_ABOVE_CAL   0x0040u      /* emis.h EMIS_ST_* bits the status word uses */
#define STD_EMIS_ST_OVER_RANGE  0x0080u
#define STD_EMIS_ST_QAL2_REVIEW 0x8000u

/* ---------------- forced values (modbus.h) ---------------- */
typedef struct { uint16_t id, index; double x; plat_val_t v; uint32_t until; uint8_t src, level; } force_t;
static force_t  forces[MB_FORCE_MAX];
static unsigned n_forces;                       /* PLAT_CRIT around every use: several tasks */

static bool ct_signed(uint8_t ct) { return ct == CT_I8 || ct == CT_I16 || ct == CT_I32; }

/* x as a value of type ct (CT_BOOL .. CT_F32): false when it is not one */
static bool force_val(uint8_t ct, double x, plat_val_t *v) {
    if (!isfinite(x)) return false;
    if (ct == CT_BOOL) { v->u = 0; v->b = x != 0.0; return true; }
    if (ct == CT_F32) { if (fabs(x) > 3.4e38) return false; v->f = (float)x; return true; }
    if (x != floor(x)) return false;
    double bits = 8.0 * (double)param_ctype_size(ct);
    if (ct_signed(ct)) {
        double lim = ldexp(1.0, (int)bits - 1);
        if (x < -lim || x >= lim) return false;
        v->i = (int32_t)x;
    } else {
        if (x < 0.0 || x >= ldexp(1.0, (int)bits)) return false;
        v->u = (uint32_t)x;
    }
    return true;
}

/* a forced value of (id, index), if any: one copy taken under the lock */
static bool forced(uint16_t id, uint16_t index, plat_val_t *v) {
    bool hit = false;
    PLAT_CRIT_ENTER();
    for (unsigned k = 0; k < n_forces; ++k)
        if (forces[k].id == id && forces[k].index == index) { *v = forces[k].v; hit = true; break; }
    PLAT_CRIT_EXIT();
    return hit;
}

/* array elements [first, first+n) as stored (es bytes each): forced ones replaced */
static void force_patch(const param_def_t *d, uint16_t first, uint16_t n, uint8_t *b) {
    if (!n_forces) return;
    uint8_t es = (uint8_t)param_ctype_size(d->ctype);
    for (uint16_t k = 0; k < n; ++k) {
        plat_val_t v;
        if (!forced(d->id, (uint16_t)(first + k), &v)) continue;
        uint8_t *p = b + (size_t)k * es;
        switch (d->ctype) {
        case CT_BOOL: p[0] = v.b ? 1u : 0u; break;
        case CT_U8:  p[0] = (uint8_t)v.u; break;
        case CT_I8:  { int8_t t = (int8_t)v.i; memcpy(p, &t, 1); break; }
        case CT_U16: { uint16_t t = (uint16_t)v.u; memcpy(p, &t, 2); break; }
        case CT_I16: { int16_t t = (int16_t)v.i; memcpy(p, &t, 2); break; }
        case CT_I32: memcpy(p, &v.i, 4); break;
        case CT_F32: memcpy(p, &v.f, 4); break;
        default:     memcpy(p, &v.u, 4); break;
        }
    }
}


static double real_value(const param_def_t *d, uint16_t index) {
    plat_val_t v;
    uint8_t ct = d->ctype;
    if (index == MB_FORCE_SCALAR) { if (!param_read(d->id, &v)) return NAN; }
    else {
        uint8_t b[4] = { 0 };
        if (param_read_blob(d->id, (uint16_t)(index * param_ctype_size(ct)), b, (uint16_t)param_ctype_size(ct)) != PERR_OK) return NAN;
        switch (ct) {
        case CT_BOOL: case CT_U8: return b[0];
        case CT_I8:  { int8_t t; memcpy(&t, b, 1); return t; }
        case CT_U16: { uint16_t t; memcpy(&t, b, 2); return t; }
        case CT_I16: { int16_t t; memcpy(&t, b, 2); return t; }
        case CT_I32: { int32_t t; memcpy(&t, b, 4); return t; }
        case CT_F32: { float t; memcpy(&t, b, 4); return t; }
        default:     { uint32_t t; memcpy(&t, b, 4); return t; }
        }
    }
    return ct == CT_F32 ? (double)v.f : ct == CT_BOOL ? (v.b ? 1.0 : 0.0) : ct_signed(ct) ? (double)v.i : (double)v.u;
}

static void force_log(const param_def_t *d, uint16_t index, uint8_t src, uint8_t level, double real, double to) {
    char t[24];
    if (index == MB_FORCE_SCALAR) snprintf(t, sizeof t, "%s", d->name);
    else snprintf(t, sizeof t, "%s[%u]", d->name, (unsigned)index);
    plat_log_event(LOG_EV_FORCE, src, level, (uint32_t)d->id | (uint32_t)index << 16, real, to, t);
}

bool mb_forceable(uint16_t id) {
    static const uint16_t never[] = { PARAM_ID_MB_BLOCK_VERSION, PARAM_ID_MB_MAP_VERSION, PARAM_ID_MB_FORCED,   /* identity and time */
                                      PARAM_ID_SERIAL, PARAM_ID_HW_REV, PARAM_ID_FW_VERSION_NUM, PARAM_ID_API_VERSION,
                                      PARAM_ID_CLOCK_UNIX, PARAM_ID_CLOCK_SYNC_TIME };
    for (size_t k = 0; k < sizeof never / sizeof never[0]; ++k) if (id == never[k]) return false;
    const param_def_t *d = param_find(id);
    if (!d || d->ctype > CT_F32 || (PARAM_IS_BLOB(d) && d->ptype != PT_ARRAY)) return false;
    bool on = id >= STD_BANK0 && id < STD_BANK0 + 8u && d->ctype == CT_F32 && (d->flags & PF_RO);   /* the standard block */
    for (size_t i = 0; i < n_idx; ++i) {
        if (idx[i].d != d) continue;
        if (idx[i].m->fmt == MBF_BYTES) return false;
        if (!(d->flags & PF_RO) && idx[i].m->table != MB_INPUT && idx[i].m->table != MB_DISCRETE) return false;   /* the PLC writes it */
        on = true;
    }
    return on;
}

int mb_force_set(uint16_t id, uint16_t index, double x, uint8_t src, uint8_t level, uint32_t now_ms) {
    if (level < PLAT_LVL_ADMIN || src == PLAT_SRC_MODBUS) return PERR_ACCESS;
    if (!mb_forceable(id)) return PERR_NO_ID;
    const param_def_t *d = param_find(id);
    if (d->ptype == PT_ARRAY ? index >= d->size / param_ctype_size(d->ctype) : index != MB_FORCE_SCALAR) return PERR_RANGE;
    plat_val_t v = { 0 };
    if (!force_val(d->ctype, x, &v)) return PERR_RANGE;
    if (d->ctype == CT_BOOL) x = v.b ? 1.0 : 0.0;
    int rc = PERR_OK;
    PLAT_CRIT_ENTER();
    unsigned k = 0;
    while (k < n_forces && !(forces[k].id == id && forces[k].index == index)) ++k;
    if (k == n_forces && n_forces >= MB_FORCE_MAX) rc = PERR_BUSY;
    else {
        if (k == n_forces) n_forces++;
        forces[k] = (force_t){ id, index, x, v, now_ms + PLAT_MB_FORCE_MS, src, level };
        plat_mb_forced = (uint16_t)n_forces;
    }
    PLAT_CRIT_EXIT();
    if (rc == PERR_OK) force_log(d, index, src, level, real_value(d, index), x);
    return rc;
}

static void force_drop(unsigned k) {               /* under the lock */
    forces[k] = forces[--n_forces];
    plat_mb_forced = (uint16_t)n_forces;
}

int mb_force_clear(uint16_t id, uint16_t index, uint8_t src, uint8_t level) {
    if (level < PLAT_LVL_ADMIN || src == PLAT_SRC_MODBUS) return PERR_ACCESS;
    bool hit = false;
    PLAT_CRIT_ENTER();
    for (unsigned k = 0; k < n_forces; ++k)
        if (forces[k].id == id && forces[k].index == index) { force_drop(k); hit = true; break; }
    PLAT_CRIT_EXIT();
    if (!hit) return PERR_NO_ID;
    force_log(param_find(id), index, src, level, 0.0, NAN);
    return PERR_OK;
}

int mb_force_clear_all(uint8_t src, uint8_t level) {
    if (level < PLAT_LVL_ADMIN || src == PLAT_SRC_MODBUS) return PERR_ACCESS;
    for (;;) {
        force_t f;
        PLAT_CRIT_ENTER();
        bool any = n_forces > 0;
        if (any) { f = forces[n_forces - 1]; force_drop(n_forces - 1); }
        PLAT_CRIT_EXIT();
        if (!any) return PERR_OK;
        force_log(param_find(f.id), f.index, src, level, 0.0, NAN);
    }
}

static void force_expire(uint32_t now_ms) {
    for (;;) {
        force_t f;
        bool hit = false;
        PLAT_CRIT_ENTER();
        for (unsigned k = 0; k < n_forces; ++k)
            if ((int32_t)(now_ms - forces[k].until) >= 0) { f = forces[k]; force_drop(k); hit = true; break; }
        PLAT_CRIT_EXIT();
        if (!hit) return;
        force_log(param_find(f.id), f.index, f.src, f.level, 1.0, NAN);
    }
}

unsigned mb_force_count(void) { return n_forces; }

bool mb_force_active(uint16_t id, uint16_t index) { plat_val_t v; return forced(id, index, &v); }

bool mb_force_get(unsigned i, mb_force_t *out, uint32_t now_ms) {
    bool ok = false;
    PLAT_CRIT_ENTER();
    if (i < n_forces) {
        const force_t *f = &forces[i];
        int32_t left = (int32_t)(f->until - now_ms);
        *out = (mb_force_t){ f->id, f->index, f->x, left > 0 ? (uint32_t)left : 0u, f->src, f->level };
        ok = true;
    }
    PLAT_CRIT_EXIT();
    return ok;
}

/* ---------------- the standard CEMS block (VDI 4201-3) ----------------
 * Input registers 7000-7999 and the same in the holding registers, read-only: built here on each
 * read, not from map entries. Layout: docs/MODBUS_STANDARD_BLOCK.md. The values come from the
 * emissions engine's parameters (emis.h); a build without them reads every slot as not fitted. */
uint8_t plat_mb_std_words;                   /* param 335 */

static const char *const std_name[STD_SLOTS] = {
    "O2 dry", "O2 wet", "CO2", "CO", "NO", "NO2", "NOx as NO2", "SO2", "HCl", "HF", "NH3", "H2O",
    "TOC as C", "CH4", "N2O", "H2S", "Hg", "Dust", "Flue gas flow", "Flue gas velocity",
    "Stack temperature", "Stack pressure abs", "Sample flow", "Probe/line temperature",
    "Cooler temperature", "Analyser cell temperature", "Cabinet temperature", "Air/cal gas pressure",
    "Opacity", "TRS", "spare", "spare" };
static const char *const std_unit[STD_SLOTS] = {
    "% vol", "% vol", "% vol", "mg/m3", "mg/m3", "mg/m3", "mg/m3", "mg/m3", "mg/m3", "mg/m3", "mg/m3",
    "% vol", "mg/m3", "mg/m3", "mg/m3", "mg/m3", "ug/m3", "mg/m3", "m3/h", "m/s", "degC", "hPa",
    "l/min", "degC", "degC", "degC", "degC", "kPa", "%", "mg/m3", "", "" };

/* The measuring range of slot n for 43/14 (the product's measurement lo/hi); none: NaN. */
__attribute__((weak)) bool mb_std_range(unsigned n, float *lo, float *hi) { (void)n; (void)lo; (void)hi; return false; }
/* The stream on line (7945): 0 for a single-stream product. */
__attribute__((weak)) uint16_t mb_std_stream(void) { return 0; }

/* element index of a parameter as Modbus serves it (a force replaces it); NaN if there is none */
static double std_elem(uint16_t id, uint16_t index) {
    const param_def_t *d = param_find(id);
    if (!d) return NAN;
    plat_val_t v;
    if (forced(id, index, &v))
        return d->ctype == CT_F32 ? (double)v.f : d->ctype == CT_BOOL ? (v.b ? 1.0 : 0.0) : ct_signed(d->ctype) ? (double)v.i : (double)v.u;
    return real_value(d, index);
}

static bool std_fitted(unsigned n) { double u = std_elem(STD_UNIT, (uint16_t)n); return isfinite(u) && u != 0.0; }

static uint8_t std_q(uint16_t id, unsigned n) {
    double q = std_elem(id, (uint16_t)n);
    return isfinite(q) && q >= 0.0 && q < PLAT_Q_COUNT ? (uint8_t)q : PLAT_Q_UNAVAILABLE;
}
static uint8_t std_q_worst(uint8_t a, uint8_t b) {         /* the emissions engine's ranking */
    static const uint8_t rank[PLAT_Q_COUNT] = {
        [PLAT_Q_VALID] = 0, [PLAT_Q_RANGE] = 1, [PLAT_Q_WARMUP] = 2, [PLAT_Q_MAINT] = 3,
        [PLAT_Q_CAL] = 4, [PLAT_Q_STALE] = 5, [PLAT_Q_UNAVAILABLE] = 6, [PLAT_Q_FAULT] = 7 };
    return rank[a] >= rank[b] ? a : b;
}

/* the slot is forced: any of its bank values, bank 0 first; *x = that forced value */
static bool std_forced(unsigned n, float *x) {
    for (uint16_t b = 0; b < 8u; ++b) {
        const param_def_t *d = param_find((uint16_t)(STD_BANK0 + b));
        plat_val_t v;
        if (d && d->ctype == CT_F32 && forced(d->id, (uint16_t)n, &v)) { *x = v.f; return true; }
    }
    return false;
}

static uint32_t std_status(unsigned n) {
    float fx;
    bool frc = std_forced(n, &fx);
    uint8_t q = std_q(STD_Q_IN, n);
    double sd = std_elem(STD_STATUS, (uint16_t)n);
    uint32_t es = isfinite(sd) ? (uint32_t)sd : 0u, w = 0;
    if (q == PLAT_Q_FAULT || q == PLAT_Q_UNAVAILABLE) w |= 1u << 0;
    if (q == PLAT_Q_MAINT || q == PLAT_Q_CAL || q == PLAT_Q_WARMUP) w |= 1u << 1;
    if (alarm_active(ALARM_ID_CAL_DUE) || (es & STD_EMIS_ST_QAL2_REVIEW)) w |= 1u << 2;
    if (q == PLAT_Q_RANGE || q == PLAT_Q_STALE || (es & (STD_EMIS_ST_ABOVE_CAL | STD_EMIS_ST_OVER_RANGE))) w |= 1u << 3;
    if (frc) w |= 1u << 4 | 1u << 16;
    w |= ((es >> 5) & 0x7FFu) << 17;                     /* engine bits 5-15 -> 17-27 */
    return w;
}

static uint16_t std_u16(double x) { return isfinite(x) && x > 0.0 ? (x >= 65535.0 ? 0xFFFFu : (uint16_t)x) : 0u; }
static uint32_t std_f32(double x) { float f = isfinite(x) ? (float)x : NAN; uint32_t u; memcpy(&u, &f, 4); if (!isfinite(x)) u = 0x7FC00000u; return u; }

/* a 32-bit value starting at base: the register a - base (0 or 1) of it, in the block's word order */
static uint16_t std_half(uint32_t v, uint16_t a, uint16_t base) {
    bool hi = (a == base) != (plat_mb_std_words == 1u);
    return (uint16_t)(hi ? v >> 16 : v);
}

static uint16_t std_reg(uint16_t a) {
    if (a < 7128u) {                                     /* BasisM: value, status */
        unsigned n = (a - 7000u) / 4u, k = (a - 7000u) % 4u;
        bool fit = std_fitted(n);
        if (k < 2) return std_half(std_f32(fit ? std_elem(STD_BANK0, (uint16_t)n) : NAN), a, (uint16_t)(7000u + 4u * n));
        return std_half(fit ? std_status(n) : 0u, a, (uint16_t)(7002u + 4u * n));
    }
    if (a < 7256u) {                                     /* BasisS: the force in effect */
        unsigned n = (a - 7128u) / 4u, k = (a - 7128u) % 4u;
        float x = 0.0f;
        bool frc = std_fitted(n) && std_forced(n, &x);
        if (k < 2) return std_half(std_f32(frc ? x : 0.0), a, (uint16_t)(7128u + 4u * n));
        return std_half(frc ? 1u << 4 : 0u, a, (uint16_t)(7130u + 4u * n));
    }
    if (a < 7300u) return 0;                             /* BasisR (none defined yet), reserved */
    if (a < 7748u) {                                     /* calculated banks 1-7 */
        unsigned k = (a - 7300u) / 64u + 1u, n = ((a - 7300u) % 64u) / 2u;
        double x = std_fitted(n) ? std_elem((uint16_t)(STD_BANK0 + k), (uint16_t)n) : NAN;
        return std_half(std_f32(x), a, (uint16_t)(7300u + 64u * (k - 1u) + 2u * n));
    }
    if (a < 7876u) {                                     /* qualities */
        unsigned g = (a - 7748u) / 32u, n = (a - 7748u) % 32u;
        if (!std_fitted(n)) return PLAT_Q_UNAVAILABLE;
        switch (g) {
        case 0:  return std_q(STD_Q_IN, n);
        case 1:  return std_q_worst(std_q(STD_Q_CALC, n), std_q(STD_Q_MASS, n));
        case 2:  return std_q_worst(std_q(STD_Q_AVG, n), std_q(STD_Q_RUN, n));
        default: return std_q(STD_Q_DAY, n);
        }
    }
    if (a < 7940u) {                                     /* counters */
        unsigned n = (a - 7876u) % 32u;
        if (!std_fitted(n)) return 0;
        return std_u16(std_elem(a < 7908u ? STD_DISCARDED : STD_DAYS_INVALID, (uint16_t)n));
    }
    switch (a) {                                         /* globals */
    case 7940: return STD_VERSION;
    case 7941: case 7942: {
        uint32_t m = 0;
        for (unsigned n = 0; n < STD_SLOTS; ++n) if (std_fitted(n)) m |= 1u << n;
        return std_half(m, a, 7941u);
    }
    case 7943: return (uint16_t)mb_force_count();
    case 7944: return plat_mb_std_words;
    case 7945: return mb_std_stream();
    case 7946: return std_u16(std_elem(STD_OPERATING, MB_FORCE_SCALAR));
    case 7947: return std_u16(std_elem(STD_DILUENT, MB_FORCE_SCALAR));
    case 7948: case 7949: return std_half(std_f32(std_elem(STD_REF_PCT, MB_FORCE_SCALAR)), a, 7948u);
    case 7950: case 7951: return std_half(std_f32(std_elem(STD_T_REF, MB_FORCE_SCALAR)), a, 7950u);
    case 7952: case 7953: return std_half(std_f32(std_elem(STD_P_REF, MB_FORCE_SCALAR)), a, 7952u);
    case 7954: return std_u16(std_elem(STD_PERIOD, MB_FORCE_SCALAR));
    case 7955: return std_u16(std_elem(STD_ENABLE, MB_FORCE_SCALAR));
    case 7956: return std_u16(std_elem(STD_CFG_ERROR, MB_FORCE_SCALAR));
    default:   return 0;                                 /* reserved */
    }
}

void mb_block_scan(uint32_t now_ms) {
    static uint32_t beat_ms;
    if (!beat_ms || (uint32_t)(now_ms - beat_ms) >= 100u) { beat_ms = now_ms ? now_ms : 1u; plat_mb_heartbeat++; }
    uint32_t age = plat_link_age_ms(now_ms);
    plat_mb_link_age_s = (uint16_t)(age / 1000u > 0xFFFFu ? 0xFFFFu : age / 1000u);
    plat_mb_uptime_s = now_ms / 1000u;
    force_expire(now_ms);
    if (!plat_mb_product[0]) block_ident();              /* after a restore of defaults cleared the texts */
}

/* ---------------- map ---------------- */
static uint8_t elem_size(const param_def_t *d) { return (uint8_t)param_ctype_size(d->ctype); }

static uint16_t span_of(const mb_map_t *e, const param_def_t *d) {
    if (!d) return 0;
    bool bits = e->table == MB_COILS || e->table == MB_DISCRETE;
    bool blob = PARAM_IS_BLOB(d);
    switch (e->fmt) {
    case MBF_BIT:   return (bits && !blob) ? 1u : 0u;
    case MBF_U16: case MBF_I16: return (!bits && !blob) ? 1u : 0u;
    case MBF_U32: case MBF_I32: case MBF_F32: return (!bits && !blob) ? 2u : 0u;
    case MBF_BYTES:
        if (bits || !blob || d->ptype == PT_ARRAY) return 0;
        return (uint16_t)((d->size + 1u) / 2u);
    case MBF_ARRAY: {
        if (!blob || d->ptype != PT_ARRAY) return 0;
        uint16_t n = (uint16_t)(d->size / elem_size(d));
        if (bits) return d->ctype == CT_BOOL ? n : 0u;
        return (uint16_t)(n * (elem_size(d) == 4 ? 2u : 1u));
    }
    default: return 0;
    }
}

uint16_t mb_entry_span(const mb_map_t *e) { return span_of(e, param_find(e->id)); }

int mb_init(const mb_map_t *map, size_t n, const mb_ident_t *id) {
    n_idx = 0;
    memset(&st, 0, sizeof st);
    PLAT_CRIT_ENTER(); n_forces = 0; plat_mb_forced = 0; PLAT_CRIT_EXIT();
    if (id) ident = *id;
    block_ident();
    if (n + N_BLOCK > MB_MAX_ENTRIES) return (int)MB_MAX_ENTRIES + 1;
    for (size_t k = 0; k < N_BLOCK; ++k) {
        const param_def_t *d = param_find(block[k].id);
        if (d) idx[n_idx++] = (ent_t){ &block[k], d, span_of(&block[k], d) };
    }
    for (size_t k = 0; k < n; ++k) {
        const param_def_t *d = param_find(map[k].id);
        uint16_t sp = span_of(&map[k], d);
        if (map[k].table > MB_INPUT || sp == 0 || (uint32_t)map[k].addr + sp > 65536u) return (int)k + 1;
        if (map[k].table == MB_INPUT && map[k].addr <= PLAT_MB_BLOCK_END && (uint32_t)map[k].addr + sp > PLAT_MB_BLOCK_ADDR)
            return (int)k + 1;                         /* the platform's block (R5: reserved in every product) */
        if ((map[k].table == MB_INPUT || map[k].table == MB_HOLDING) && map[k].addr <= PLAT_MB_STD_END &&
            (uint32_t)map[k].addr + sp > PLAT_MB_STD_ADDR)
            return (int)k + 1;                         /* the standard CEMS block */
        if (map[k].table == MB_DISCRETE && map[k].addr <= PLAT_DIA_MB_END && (uint32_t)map[k].addr + sp > PLAT_DIA_MB_ADDR)
            return (int)k + 1;                         /* the digital input alarms (plat_alarm.h) */
        idx[n_idx++] = (ent_t){ &map[k], d, sp };
    }
    for (size_t i = 1; i < n_idx; ++i) {          /* sort by (table, addr) */
        ent_t x = idx[i]; size_t j = i;
        while (j > 0 && (idx[j - 1].m->table > x.m->table ||
                         (idx[j - 1].m->table == x.m->table && idx[j - 1].m->addr > x.m->addr))) { idx[j] = idx[j - 1]; --j; }
        idx[j] = x;
    }
    for (size_t i = 1; i < n_idx; ++i)
        if (idx[i].m->table == idx[i - 1].m->table && (uint32_t)idx[i - 1].m->addr + idx[i - 1].span > idx[i].m->addr)
            return (int)((idx[i].m >= map && idx[i].m < map + n ? idx[i].m : idx[i - 1].m) - map) + 1;
    return 0;
}

mb_stats_t mb_stats(void) { return st; }

size_t mb_entry_count(void) { return n_idx; }
const mb_map_t *mb_entry_at(size_t i, uint16_t *span) {
    if (i >= n_idx) return NULL;
    if (span) *span = idx[i].span;
    return idx[i].m;
}

static const ent_t *find(uint8_t table, uint16_t a) {
    size_t lo = 0, hi = n_idx;                     /* last entry with (table, addr) <= (table, a) */
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const mb_map_t *m = idx[mid].m;
        if (m->table < table || (m->table == table && m->addr <= a)) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return NULL;
    const ent_t *e = &idx[lo - 1];
    if (e->m->table != table || (uint32_t)a >= (uint32_t)e->m->addr + e->span) return NULL;
    return e;
}

static uint8_t ex_of(param_err_t e) {
    switch (e) {
    case PERR_NO_ID: case PERR_READONLY: return EX_ADDR;
    case PERR_ACCESS: return EX_FUNC;
    case PERR_MODE: case PERR_INTERLOCK: return EX_FAIL;
    case PERR_BUSY: return EX_BUSY;
    default: return EX_VALUE;
    }
}

/* ---------------- value <-> registers ---------------- */
static double p10(int8_t dec) { double f = 1.0; for (int k = 0; k < (dec < 0 ? -dec : dec); ++k) f *= 10.0; return dec < 0 ? 1.0 / f : f; }

static int64_t clamp64(int64_t x, int64_t lo, int64_t hi) { return x < lo ? lo : x > hi ? hi : x; }

static void put32(const mb_map_t *m, uint32_t w, uint16_t *r) {
    uint16_t hi = (uint16_t)(w >> 16), lo = (uint16_t)w;
    if (m->flags & MBF_WORD_LO_FIRST) { r[0] = lo; r[1] = hi; } else { r[0] = hi; r[1] = lo; }
}
static uint32_t get32(const mb_map_t *m, const uint16_t *r) {
    return (m->flags & MBF_WORD_LO_FIRST) ? ((uint32_t)r[1] << 16 | r[0]) : ((uint32_t)r[0] << 16 | r[1]);
}
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float    bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* scalar entry -> its 1 or 2 registers */
static param_err_t scalar_regs(const ent_t *e, uint16_t *r) {
    plat_val_t v;
    if (!param_read(e->d->id, &v)) return PERR_NO_ID;
    (void)forced(e->d->id, MB_FORCE_SCALAR, &v);
    const mb_map_t *m = e->m;
    bool isf = e->d->ptype == PT_FLOAT;
    double x = isf ? (double)v.f : e->d->ptype == PT_INT ? (double)v.i : e->d->ptype == PT_UINT ? (double)v.u : (v.b ? 1.0 : 0.0);
    int64_t ix = isf ? (int64_t)llround(x * p10(m->dec)) : (int64_t)x;
    if (isf && !isfinite(x)) ix = 0;
    switch (m->fmt) {
    case MBF_U16: r[0] = (uint16_t)clamp64(ix, 0, 65535); break;
    case MBF_I16: r[0] = (uint16_t)(int16_t)clamp64(ix, -32768, 32767); break;
    case MBF_U32: put32(m, (uint32_t)clamp64(ix, 0, 4294967295LL), r); break;
    case MBF_I32: put32(m, (uint32_t)(int32_t)clamp64(ix, -2147483648LL, 2147483647LL), r); break;
    case MBF_F32: put32(m, fbits((float)x), r); break;
    default: return PERR_TYPE;
    }
    return PERR_OK;
}

/* registers of an array element */
static void elem_regs(const ent_t *e, const uint8_t *p, uint16_t *r) {
    union { uint8_t b[4]; uint8_t u8; int8_t i8; uint16_t u16; int16_t i16; uint32_t u32; } t = { { 0 } };
    memcpy(t.b, p, elem_size(e->d));
    switch (e->d->ctype) {
    case CT_BOOL: r[0] = t.b[0] ? 1u : 0u; break;
    case CT_U8:  r[0] = t.u8; break;
    case CT_I8:  r[0] = (uint16_t)(int16_t)t.i8; break;
    case CT_U16: r[0] = t.u16; break;
    case CT_I16: r[0] = (uint16_t)t.i16; break;
    default:     put32(e->m, t.u32, r); break;     /* U32, I32, F32: raw bits */
    }
}

/* Registers [r0, r0+n) of one entry. */
static param_err_t entry_regs(const ent_t *e, uint16_t r0, uint16_t n, uint16_t *out) {
    const mb_map_t *m = e->m;
    if (m->fmt == MBF_BYTES) {
        uint8_t b[2 * MB_MAX_READ_REGS] = { 0 };
        uint32_t off = 2u * r0, len = 2u * n;
        if (off + len > e->d->size) len = e->d->size - off;            /* odd-size block: last half empty */
        param_err_t err = param_read_blob(e->d->id, (uint16_t)off, b, (uint16_t)len);
        if (err != PERR_OK) return err;
        for (uint16_t k = 0; k < n; ++k)
            out[k] = (uint16_t)((m->flags & MBF_BYTE_LO_FIRST) ? (b[2 * k] | (b[2 * k + 1] << 8))
                                                               : ((b[2 * k] << 8) | b[2 * k + 1]));
        return PERR_OK;
    }
    if (m->fmt == MBF_ARRAY) {
        uint8_t es = elem_size(e->d), rpe = es == 4 ? 2 : 1;
        uint16_t first = (uint16_t)(r0 / rpe), last = (uint16_t)((r0 + n - 1u) / rpe);
        uint8_t b[4 * MB_MAX_READ_REGS];
        param_err_t err = param_read_blob(e->d->id, (uint16_t)(first * es), b, (uint16_t)((last - first + 1u) * es));
        if (err != PERR_OK) return err;
        force_patch(e->d, first, (uint16_t)(last - first + 1u), b);
        for (uint16_t k = 0; k < n; ++k) {
            uint16_t reg = (uint16_t)(r0 + k), el = (uint16_t)(reg / rpe), two[2];
            elem_regs(e, b + (el - first) * es, two);
            out[k] = two[reg % rpe];
        }
        return PERR_OK;
    }
    uint16_t two[2];
    param_err_t err = scalar_regs(e, two);
    if (err != PERR_OK) return err;
    memcpy(out, two + r0, n * sizeof(uint16_t));
    return PERR_OK;
}

/* Bits [b0, b0+n) of one entry. */
static param_err_t entry_bits(const ent_t *e, uint16_t b0, uint16_t n, uint8_t *bits /* one byte per bit */) {
    if (e->m->fmt == MBF_BIT) {
        plat_val_t v;
        if (!param_read(e->d->id, &v)) return PERR_NO_ID;
        (void)forced(e->d->id, MB_FORCE_SCALAR, &v);
        bits[0] = e->d->ptype == PT_FLOAT ? (v.f != 0.0f) : e->d->ptype == PT_BOOL ? v.b : (v.u != 0);
        return PERR_OK;
    }
    param_err_t err = param_read_blob(e->d->id, b0, bits, n);   /* bool array: one byte per element */
    if (err == PERR_OK) force_patch(e->d, b0, n, bits);
    return err;
}

/* ---------------- reads ---------------- */
static uint16_t exc(uint8_t fc, uint8_t code, uint8_t *resp) {
    resp[0] = (uint8_t)(fc | 0x80u); resp[1] = code;
    st.exceptions++;
    return 2;
}

static uint16_t rd_regs(uint8_t fc, uint8_t table, uint16_t a, uint16_t q, uint8_t *resp, uint16_t cap) {
    if (q < 1 || q > MB_MAX_READ_REGS) return exc(fc, EX_VALUE, resp);
    if ((uint32_t)a + q > 65536u) return exc(fc, EX_ADDR, resp);
    if (cap < 2u + 2u * q) return exc(fc, EX_FAIL, resp);
    uint16_t regs[MB_MAX_READ_REGS];
    for (uint16_t k = 0; k < q;) {
        uint16_t ak = (uint16_t)(a + k);
        if (ak >= PLAT_MB_STD_ADDR && ak <= PLAT_MB_STD_END) { regs[k++] = std_reg(ak); continue; }   /* input or holding */
        const ent_t *e = find(table, ak);
        if (!e) return exc(fc, EX_ADDR, resp);
        uint16_t r0 = (uint16_t)(a + k - e->m->addr), n = (uint16_t)(e->span - r0);
        if (n > q - k) n = (uint16_t)(q - k);
        param_err_t err = entry_regs(e, r0, n, regs + k);
        if (err != PERR_OK) return exc(fc, ex_of(err), resp);
        k = (uint16_t)(k + n);
    }
    resp[0] = fc; resp[1] = (uint8_t)(2u * q);
    for (uint16_t k = 0; k < q; ++k) { resp[2 + 2 * k] = (uint8_t)(regs[k] >> 8); resp[3 + 2 * k] = (uint8_t)regs[k]; }
    return (uint16_t)(2u + 2u * q);
}

static uint16_t rd_bits(uint8_t fc, uint8_t table, uint16_t a, uint16_t q, uint8_t *resp, uint16_t cap) {
    if (q < 1 || q > MB_MAX_READ_BITS) return exc(fc, EX_VALUE, resp);
    if ((uint32_t)a + q > 65536u) return exc(fc, EX_ADDR, resp);
    uint16_t nb = (uint16_t)((q + 7u) / 8u);
    if (cap < 2u + nb) return exc(fc, EX_FAIL, resp);
    uint8_t bits[MB_MAX_READ_BITS];
    for (uint16_t k = 0; k < q;) {
        uint16_t ak = (uint16_t)(a + k);
        if (table == MB_DISCRETE && ak >= PLAT_DIA_MB_ADDR && ak <= PLAT_DIA_MB_END) { bits[k++] = plat_dia_mb_bit(ak); continue; }
        const ent_t *e = find(table, ak);
        if (!e) return exc(fc, EX_ADDR, resp);
        uint16_t b0 = (uint16_t)(a + k - e->m->addr), n = (uint16_t)(e->span - b0);
        if (n > q - k) n = (uint16_t)(q - k);
        param_err_t err = entry_bits(e, b0, n, bits + k);
        if (err != PERR_OK) return exc(fc, ex_of(err), resp);
        k = (uint16_t)(k + n);
    }
    resp[0] = fc; resp[1] = (uint8_t)nb;
    memset(resp + 2, 0, nb);
    for (uint16_t k = 0; k < q; ++k) if (bits[k]) resp[2 + k / 8] |= (uint8_t)(1u << (k % 8));
    return (uint16_t)(2u + nb);
}

/* ---------------- writes: plan, check everything, then queue ---------------- */
typedef struct {
    uint16_t id;
    bool     blob, whole;
    uint8_t  kind;
    plat_val_t v;
    uint16_t off, len;
    uint8_t *data;                /* blocks: one of blob_data[] */
} op_t;

#define MB_MAX_BLOB_OPS 16u       /* = the block write queue depth */
static op_t    ops[MB_MAX_OPS];   /* only the Modbus task writes; static to keep it off the stack */
static uint8_t blob_data[MB_MAX_BLOB_OPS][PARAM_BLOB_MAX_WRITE];
static size_t  n_blob_data;

static uint8_t *claim_blob(void) { return n_blob_data < MB_MAX_BLOB_OPS ? blob_data[n_blob_data++] : NULL; }

static param_err_t scalar_from_regs(const ent_t *e, const uint16_t *r, op_t *op) {
    const mb_map_t *m = e->m;
    const param_def_t *d = e->d;
    int64_t raw;
    op->id = d->id; op->blob = false;
    switch (m->fmt) {
    case MBF_U16: raw = r[0]; break;
    case MBF_I16: raw = (int16_t)r[0]; break;
    case MBF_U32: raw = get32(m, r); break;
    case MBF_I32: raw = (int32_t)get32(m, r); break;
    case MBF_F32: op->kind = PLAT_VAL_F32; op->v.f = bitsf(get32(m, r)); return PERR_OK;
    default: return PERR_TYPE;
    }
    if (d->ptype == PT_FLOAT) { op->kind = PLAT_VAL_F32; op->v.f = (float)((double)raw / p10(m->dec)); return PERR_OK; }
    if (raw > 2147483647LL) { op->kind = PLAT_VAL_U32; op->v.u = (uint32_t)raw; }
    else { op->kind = PLAT_VAL_I32; op->v.i = (int32_t)raw; }
    return PERR_OK;
}

/* registers [r0, r0+n) of one entry -> one op. Returns an exception code or 0. */
static uint8_t plan_regs(const ent_t *e, uint16_t r0, uint16_t n, const uint16_t *regs, op_t *op) {
    const mb_map_t *m = e->m;
    const param_def_t *d = e->d;
    memset(op, 0, sizeof *op);
    if ((m->fmt == MBF_BYTES || m->fmt == MBF_ARRAY) && (op->data = claim_blob()) == NULL) return EX_BUSY;
    if (m->fmt == MBF_BYTES) {
        bool text = d->ptype == PT_TEXT || d->ptype == PT_TEXT16;
        if (text && (r0 != 0 || n != e->span)) return EX_VALUE;          /* text: whole entry only */
        uint32_t off = 2u * r0, len = 2u * n;
        if (off + len > d->size) len = d->size - off;
        if (len > PARAM_BLOB_MAX_WRITE) return EX_VALUE;
        uint8_t b[2 * MB_MAX_WRITE_REGS];
        for (uint16_t k = 0; k < n; ++k) {
            if (m->flags & MBF_BYTE_LO_FIRST) { b[2 * k] = (uint8_t)regs[k]; b[2 * k + 1] = (uint8_t)(regs[k] >> 8); }
            else { b[2 * k] = (uint8_t)(regs[k] >> 8); b[2 * k + 1] = (uint8_t)regs[k]; }
        }
        if (text) {                                                      /* up to the terminator */
            uint32_t t = 0;
            if (d->ptype == PT_TEXT) { while (t < len && b[t]) t++; }
            else { while (t + 1 < len && (b[t] | b[t + 1])) t += 2; }
            len = t;
        }
        op->id = d->id; op->blob = true; op->whole = text; op->off = (uint16_t)off; op->len = (uint16_t)len;
        memcpy(op->data, b, len);
        return 0;
    }
    if (m->fmt == MBF_ARRAY) {
        uint8_t es = elem_size(d), rpe = es == 4 ? 2 : 1;
        if (r0 % rpe || n % rpe) return EX_ADDR;                          /* half a 32-bit element */
        uint16_t first = (uint16_t)(r0 / rpe), cnt = (uint16_t)(n / rpe);
        if ((uint32_t)cnt * es > PARAM_BLOB_MAX_WRITE) return EX_VALUE;
        for (uint16_t k = 0; k < cnt; ++k) {
            const uint16_t *rr = regs + k * rpe;
            uint8_t *p = op->data + k * es;
            union { uint8_t b[4]; uint8_t u8; int8_t i8; uint16_t u16; int16_t i16; uint32_t u32; } t = { { 0 } };
            switch (d->ctype) {
            case CT_BOOL: if (rr[0] > 1u) return EX_VALUE; t.u8 = (uint8_t)rr[0]; break;
            case CT_U8:  if (rr[0] > 255u) return EX_VALUE; t.u8 = (uint8_t)rr[0]; break;
            case CT_I8:  { int16_t x = (int16_t)rr[0]; if (x < -128 || x > 127) return EX_VALUE; t.i8 = (int8_t)x; } break;
            case CT_U16: t.u16 = rr[0]; break;
            case CT_I16: t.i16 = (int16_t)rr[0]; break;
            default:     t.u32 = get32(m, rr); break;
            }
            memcpy(p, t.b, es);
        }
        op->id = d->id; op->blob = true; op->off = (uint16_t)(first * es); op->len = (uint16_t)(cnt * es);
        return 0;
    }
    if (r0 != 0 || n != e->span) return EX_ADDR;                          /* half a 32-bit value */
    param_err_t err = scalar_from_regs(e, regs, op);
    return err == PERR_OK ? 0 : ex_of(err);
}

static uint8_t plan_bits(const ent_t *e, uint16_t b0, uint16_t n, const uint8_t *bits, op_t *op) {
    memset(op, 0, sizeof *op);
    op->id = e->d->id;
    if (e->m->fmt == MBF_BIT) { op->kind = PLAT_VAL_BOOL; op->v.b = bits[0] != 0; return 0; }
    if (n > PARAM_BLOB_MAX_WRITE) return EX_VALUE;
    if ((op->data = claim_blob()) == NULL) return EX_BUSY;
    op->blob = true; op->off = b0; op->len = n;
    for (uint16_t k = 0; k < n; ++k) op->data[k] = bits[k] ? 1u : 0u;
    return 0;
}

static plat_cmd_t          q_cmds[MB_MAX_OPS];
static param_blob_write_t  q_blobs[MB_MAX_OPS];

static uint8_t check_and_queue(size_t n_ops) {
    uint8_t lvl = ident.write_level;
    size_t ns = 0, nb = 0;
    for (size_t k = 0; k < n_ops; ++k) {
        op_t *o = &ops[k];
        if (o->blob) q_blobs[nb++] = (param_blob_write_t){ .id = o->id, .off = o->off, .len = o->len,
                                                           .whole = o->whole, .data = o->data };
        else q_cmds[ns++] = (plat_cmd_t){ .id = o->id, .src = PLAT_SRC_MODBUS, .kind = o->kind, .level = lvl, .v = o->v };
    }
    /* check both batches completely before queueing either */
    for (size_t k = 0; k < ns; ++k) {
        param_err_t e = param_check(q_cmds[k].id, q_cmds[k].kind, q_cmds[k].v, PLAT_SRC_MODBUS, lvl);
        if (e != PERR_OK) return ex_of(e);
    }
    for (size_t k = 0; k < nb; ++k) {
        param_err_t e = param_blob_check(q_blobs[k].id, q_blobs[k].off, q_blobs[k].data, q_blobs[k].len,
                                         q_blobs[k].whole, PLAT_SRC_MODBUS, lvl);
        if (e != PERR_OK) return ex_of(e);
    }
    param_err_t e = param_post_many(q_cmds, ns);
    if (e != PERR_OK) return ex_of(e);
    e = param_blob_post_many(q_blobs, nb, PLAT_SRC_MODBUS, lvl);
    if (e != PERR_OK) return ex_of(e);       /* scalars already queued: only if the block queue is full */
    st.writes_queued += (uint32_t)(ns + nb);
    return 0;
}

static uint16_t wr_regs(uint8_t fc, uint16_t a, uint16_t q, const uint16_t *regs, uint8_t *resp) {
    if ((uint32_t)a + q > 65536u) return exc(fc, EX_ADDR, resp);
    size_t n_ops = 0;
    n_blob_data = 0;
    for (uint16_t k = 0; k < q;) {
        const ent_t *e = find(MB_HOLDING, (uint16_t)(a + k));
        if (!e) return exc(fc, EX_ADDR, resp);
        uint16_t r0 = (uint16_t)(a + k - e->m->addr), n = (uint16_t)(e->span - r0);
        if (n > q - k) n = (uint16_t)(q - k);
        if (n_ops >= MB_MAX_OPS) return exc(fc, EX_VALUE, resp);
        uint8_t x = plan_regs(e, r0, n, regs + k, &ops[n_ops++]);
        if (x) return exc(fc, x, resp);
        k = (uint16_t)(k + n);
    }
    uint8_t x = check_and_queue(n_ops);
    return x ? exc(fc, x, resp) : 0;
}

static uint16_t wr_bits(uint8_t fc, uint16_t a, uint16_t q, const uint8_t *bits, uint8_t *resp) {
    if ((uint32_t)a + q > 65536u) return exc(fc, EX_ADDR, resp);
    size_t n_ops = 0;
    n_blob_data = 0;
    for (uint16_t k = 0; k < q;) {
        const ent_t *e = find(MB_COILS, (uint16_t)(a + k));
        if (!e) return exc(fc, EX_ADDR, resp);
        uint16_t b0 = (uint16_t)(a + k - e->m->addr), n = (uint16_t)(e->span - b0);
        if (n > q - k) n = (uint16_t)(q - k);
        if (n_ops >= MB_MAX_OPS) return exc(fc, EX_VALUE, resp);
        uint8_t x = plan_bits(e, b0, n, bits + k, &ops[n_ops++]);
        if (x) return exc(fc, x, resp);
        k = (uint16_t)(k + n);
    }
    uint8_t x = check_and_queue(n_ops);
    return x ? exc(fc, x, resp) : 0;
}

/* ---------------- device identification (43/14) ----------------
 * Conformity 0x83: basic (0x00-0x02), regular (0x04 product name) and extended objects, read as a
 * stream (codes 1-3: from an object to the end of its category, "more follows" when the PDU is
 * full) or one at a time (code 4). The extended objects are VDI 4201-3's self-description of the
 * standard block (docs/MODBUS_STANDARD_BLOCK.md section 5). */
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* object id's value into out (at least 64 bytes): its length, or -1 when there is no such object */
static int dev_obj(uint8_t id, uint8_t *out) {
    const char *t = NULL;
    char b[24];
    switch (id) {
    case 0x00: t = ident.vendor; break;
    case 0x01: t = ident.product; break;
    case 0x02: t = ident.revision; break;
    case 0x04: t = ident.product; break;
    case 0x80: { plat_val_t v; snprintf(b, sizeof b, "%lu", param_read(PARAM_ID_SERIAL, &v) ? (unsigned long)v.u : 0ul); t = b; break; }
    case 0x81: out[0] = 0; out[1] = STD_COMPONENTS; return 2;
    case 0x82: out[0] = 7000u >> 8; out[1] = 7000u & 0xFFu; return 2;
    case 0x83: out[0] = 7128u >> 8; out[1] = 7128u & 0xFFu; return 2;
    case 0x84: out[0] = 7256u >> 8; out[1] = 7256u & 0xFFu; return 2;
    default: {
        if (id < 0x85u || id >= 0x85u + 4u * STD_COMPONENTS) return -1;
        unsigned n = (id - 0x85u) / 4u, k = (id - 0x85u) % 4u;
        bool fit = std_fitted(n);
        float lo = NAN, hi = NAN;
        if (k == 1 || k == 2) {
            if (!fit || !mb_std_range(n, &lo, &hi)) lo = hi = NAN;
            put_be32(out, std_f32(k == 1 ? lo : hi));
            return 4;
        }
        t = k == 0 ? (fit ? std_name[n] : "not fitted") : std_unit[n];
    }
    }
    size_t sl = strlen(t ? t : "");
    if (sl > 64) sl = 64;
    memcpy(out, t ? t : "", sl);
    return (int)sl;
}

static uint16_t dev_id(const uint8_t *req, uint16_t len, uint8_t *resp, uint16_t cap) {
    if (len < 4 || req[1] != 0x0E) return exc(0x2B, EX_FUNC, resp);
    uint8_t code = req[2], obj = req[3], val[64];
    if (code < 1 || code > 4) return exc(0x2B, EX_VALUE, resp);
    if (dev_obj(obj, val) < 0) { if (code == 4) return exc(0x2B, EX_ADDR, resp); obj = 0; }
    unsigned last = code == 1 ? 0x02u : code == 2 ? 0x7Fu : 0xFFu;
    if (code != 4 && obj > last) obj = 0;
    uint16_t n = 7;
    resp[0] = 0x2B; resp[1] = 0x0E; resp[2] = code; resp[3] = 0x83; resp[4] = 0; resp[5] = 0; resp[6] = 0;
    for (unsigned o = obj; o <= (code == 4 ? obj : last); ++o) {
        int vl = dev_obj((uint8_t)o, val);
        if (vl < 0) continue;
        if (n + 2u + (unsigned)vl > cap) { resp[4] = 0xFF; resp[5] = (uint8_t)o; break; }   /* more follows from o */
        resp[n++] = (uint8_t)o; resp[n++] = (uint8_t)vl; memcpy(resp + n, val, (size_t)vl); n = (uint16_t)(n + vl);
        resp[6]++;
    }
    return n;
}

/* ---------------- dispatcher ---------------- */
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

uint16_t mb_pdu(const uint8_t *req, uint16_t len, uint8_t *resp, uint16_t cap) {
    if (len < 1 || cap < 8) return 0;
    st.requests++;
    uint8_t fc = req[0];
    switch (fc) {
    case 1: case 2: case 3: case 4: {
        if (len != 5) return exc(fc, EX_VALUE, resp);
        uint16_t a = be16(req + 1), q = be16(req + 3);
        if (fc == 1) return rd_bits(fc, MB_COILS, a, q, resp, cap);
        if (fc == 2) return rd_bits(fc, MB_DISCRETE, a, q, resp, cap);
        return rd_regs(fc, fc == 3 ? MB_HOLDING : MB_INPUT, a, q, resp, cap);
    }
    case 5: {
        if (len != 5) return exc(fc, EX_VALUE, resp);
        uint16_t a = be16(req + 1), v = be16(req + 3);
        if (v != 0xFF00u && v != 0x0000u) return exc(fc, EX_VALUE, resp);
        uint8_t bit = v ? 1u : 0u;
        uint16_t r = wr_bits(fc, a, 1, &bit, resp);
        if (r) return r;
        memcpy(resp, req, 5); return 5;                                  /* echo */
    }
    case 6: {
        if (len != 5) return exc(fc, EX_VALUE, resp);
        uint16_t a = be16(req + 1), v = be16(req + 3);
        uint16_t r = wr_regs(fc, a, 1, &v, resp);
        if (r) return r;
        memcpy(resp, req, 5); return 5;
    }
    case 15: {
        if (len < 6) return exc(fc, EX_VALUE, resp);
        uint16_t a = be16(req + 1), q = be16(req + 3);
        uint8_t nb = req[5];
        if (q < 1 || q > MB_MAX_WRITE_BITS || nb != (q + 7u) / 8u || len != 6u + nb) return exc(fc, EX_VALUE, resp);
        static uint8_t bits[MB_MAX_WRITE_BITS];
        for (uint16_t k = 0; k < q; ++k) bits[k] = (uint8_t)((req[6 + k / 8] >> (k % 8)) & 1u);
        uint16_t r = wr_bits(fc, a, q, bits, resp);
        if (r) return r;
        memcpy(resp, req, 5); return 5;
    }
    case 16: {
        if (len < 6) return exc(fc, EX_VALUE, resp);
        uint16_t a = be16(req + 1), q = be16(req + 3);
        uint8_t nb = req[5];
        if (q < 1 || q > MB_MAX_WRITE_REGS || nb != 2u * q || len != 6u + nb) return exc(fc, EX_VALUE, resp);
        uint16_t regs[MB_MAX_WRITE_REGS];
        for (uint16_t k = 0; k < q; ++k) regs[k] = be16(req + 6 + 2 * k);
        uint16_t r = wr_regs(fc, a, q, regs, resp);
        if (r) return r;
        memcpy(resp, req, 5); return 5;
    }
    case 0x2B:
        return dev_id(req, len, resp, cap);
    default:
        return exc(fc, EX_FUNC, resp);
    }
}
