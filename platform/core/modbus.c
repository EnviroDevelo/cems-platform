/* modbus.c - see modbus.h */
#include <math.h>
#include <string.h>
#include "modbus.h"
#include "plat_config.h"
#include "plat_state.h"
#include "fw_update.h"

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

void mb_block_scan(uint32_t now_ms) {
    static uint32_t beat_ms;
    if (!beat_ms || (uint32_t)(now_ms - beat_ms) >= 100u) { beat_ms = now_ms ? now_ms : 1u; plat_mb_heartbeat++; }
    uint32_t age = plat_link_age_ms(now_ms);
    plat_mb_link_age_s = (uint16_t)(age / 1000u > 0xFFFFu ? 0xFFFFu : age / 1000u);
    plat_mb_uptime_s = now_ms / 1000u;
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
        bits[0] = e->d->ptype == PT_FLOAT ? (v.f != 0.0f) : e->d->ptype == PT_BOOL ? v.b : (v.u != 0);
        return PERR_OK;
    }
    return param_read_blob(e->d->id, b0, bits, n);          /* bool array: one byte per element */
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
        const ent_t *e = find(table, (uint16_t)(a + k));
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
        const ent_t *e = find(table, (uint16_t)(a + k));
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

/* ---------------- device identification (43/14) ---------------- */
static uint16_t dev_id(const uint8_t *req, uint16_t len, uint8_t *resp, uint16_t cap) {
    if (len < 4 || req[1] != 0x0E) return exc(0x2B, EX_FUNC, resp);
    uint8_t code = req[2], obj = req[3];
    const char *objs[3] = { ident.vendor, ident.product, ident.revision };
    if (code != 1 && code != 4) return exc(0x2B, EX_VALUE, resp);
    if (obj > 2) { if (code == 4) return exc(0x2B, EX_ADDR, resp); obj = 0; }
    uint16_t n = 7;
    resp[0] = 0x2B; resp[1] = 0x0E; resp[2] = code; resp[3] = 0x81; resp[4] = 0; resp[5] = 0; resp[6] = 0;
    for (uint8_t o = obj; o <= (code == 4 ? obj : 2); ++o) {
        size_t sl = strlen(objs[o] ? objs[o] : "");
        if (sl > 64) sl = 64;
        if (n + 2u + sl > cap) break;
        resp[n++] = o; resp[n++] = (uint8_t)sl; memcpy(resp + n, objs[o], sl); n = (uint16_t)(n + sl);
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
