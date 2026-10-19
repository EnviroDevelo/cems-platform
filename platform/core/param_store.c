/* param_store.c - see param_store.h */
#include <math.h>
#include <string.h>
#include "param_store.h"
#include "plat_port.h"

#define MAGIC        0x504D4543u   /* "CEMP" little-endian */
#define XMAGIC       0x584D4543u   /* "CEMX" backup file */
#define REGION_FAC   ((uint8_t)0)
#define REGION_USR   ((uint8_t)1)
#define MAX_ITEMS    (PARAM_MAX + 256)

const store_layout_t store_layout_default = {
    .fac_a = STORE_FAC_A, .fac_b = STORE_FAC_B, .fac_slot = STORE_FAC_SLOT,
    .usr_a = STORE_USR_A, .usr_b = STORE_USR_B, .usr_slot = STORE_USR_SLOT,
};
/* the map must not overlap and must fit the part */
_Static_assert(STORE_FAC_A + STORE_FAC_SLOT <= STORE_FAC_B, "FAC A overlaps FAC B");
_Static_assert(STORE_FAC_B + STORE_FAC_SLOT <= STORE_USR_A, "FAC B overlaps USR A");
_Static_assert(STORE_USR_A + STORE_USR_SLOT <= STORE_USR_B, "USR A overlaps USR B");
_Static_assert(STORE_USR_B + STORE_USR_SLOT <= STORE_META_ADDR, "USR B overlaps the logger area");
_Static_assert(STORE_META_ADDR < PLAT_FRAM_SIZE, "map larger than the FRAM");

extern uint32_t param_store_status_word;    /* storage of PARAM_ID_STORE_STATUS (param.c) */

/* Platform-owned retained blocks (e.g. password hashes): kept in FRAM, never
 * visible through the parameter table. Provided by web_auth.c; none otherwise. */
__attribute__((weak)) const retain_def_t *plat_store_blocks(size_t *n) { *n = 0; return NULL; }

/* one persisted item: a PF_PERSIST parameter or a retained block */
typedef struct {
    uint16_t id, size;
    uint8_t  region;
    const param_def_t *pdef;    /* non-NULL for parameters (range-checked on load) */
    const retain_def_t *rdef;   /* non-NULL for blocks */
} item_t;

typedef struct {
    uint32_t addr[2], slot;
    uint8_t *img;               /* staging image: header + payload */
    int      active;            /* copy holding the newest valid image, -1 = none */
    uint32_t seq;
    uint32_t saved_crc;         /* payload CRC of what is in NVM (or the baseline) */
    bool     have_saved;
    bool     valid_at_boot;
    uint32_t payload_len;       /* of the last snapshot */
} region_t;

static uint8_t img_fac[STORE_FAC_SLOT], img_usr[STORE_USR_SLOT];

static struct {
    nvm_dev_t *dev;
    item_t items[MAX_ITEMS];
    size_t n_items;
    region_t rg[2];
    uint16_t layout_version;
    param_migrate_fn migrate;
    uint32_t status;
    store_stats_t st;
    uint32_t last_save_ms;
    bool started;               /* snapshot requested, result not yet processed */
    bool first_done;
    /* scan <-> store handshake */
    volatile uint8_t req;        /* REQ_* set by store task, cleared by scan when done */
    /* other tasks -> store task */
    volatile uint8_t job, job_state;
    uint8_t *job_buf; uint32_t job_len; uint8_t job_level; volatile int32_t job_result;
    const uint8_t *imp_buf[2];   /* import: payloads to apply */
    uint32_t imp_len[2];
    uint16_t imp_ver[2];
} S;

enum { REQ_NONE = 0, REQ_SNAPSHOT, REQ_IMPORT };

static void set_status(uint32_t bits) { S.status |= bits; param_store_status_word = S.status; }
static void clr_status(uint32_t bits) { S.status &= ~bits; param_store_status_word = S.status; }

/* ---------------- little-endian helpers ---------------- */
static void     put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void     put32(uint8_t *p, uint32_t v) { for (int k = 0; k < 4; ++k) p[k] = (uint8_t)(v >> (8 * k)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* header layout (32 bytes):
 *  0 magic | 4 hdr_size u16 | 6 layout_version u16 | 8 region u8, 3 rsvd | 12 seq
 * 16 payload_len | 20 payload_crc | 24 rsvd | 28 header_crc (over bytes 0..27) */
typedef struct { uint16_t version; uint8_t region; uint32_t seq, len, crc; } hdr_t;

static void hdr_write(uint8_t *h, const hdr_t *x) {
    memset(h, 0, STORE_HDR_SIZE);
    put32(h + 0, MAGIC); put16(h + 4, STORE_HDR_SIZE); put16(h + 6, x->version); h[8] = x->region;
    put32(h + 12, x->seq); put32(h + 16, x->len); put32(h + 20, x->crc);
    put32(h + 28, plat_crc32(0, h, 28));
}

static bool hdr_read(const uint8_t *h, uint8_t region, uint32_t slot, hdr_t *x) {
    if (get32(h) != MAGIC || get16(h + 4) != STORE_HDR_SIZE) return false;
    if (get32(h + 28) != plat_crc32(0, h, 28)) return false;
    x->version = get16(h + 6); x->region = h[8]; x->seq = get32(h + 12); x->len = get32(h + 16); x->crc = get32(h + 20);
    return x->region == region && x->len <= slot - STORE_HDR_SIZE;
}

/* ---------------- registry ---------------- */
static const item_t *find_item(uint16_t id) {
    for (size_t k = 0; k < S.n_items; ++k) if (S.items[k].id == id) return &S.items[k];
    return NULL;
}

static uint32_t region_need(uint8_t region) {
    uint32_t n = STORE_HDR_SIZE;
    for (size_t k = 0; k < S.n_items; ++k)
        if (S.items[k].region == region) n += 4u + ((S.items[k].size + 3u) & ~3u);
    return n;
}

bool param_store_init(nvm_dev_t *dev, const store_layout_t *lay, const retain_def_t *blocks, size_t n_blocks,
                      uint16_t layout_version, param_migrate_fn migrate) {
    memset(&S, 0, sizeof S);
    param_store_status_word = 0;
    if (!lay) lay = &store_layout_default;
    S.dev = dev;
    S.layout_version = layout_version;
    S.migrate = migrate;
    S.rg[REGION_FAC] = (region_t){ .addr = { lay->fac_a, lay->fac_b }, .slot = lay->fac_slot, .img = img_fac, .active = -1 };
    S.rg[REGION_USR] = (region_t){ .addr = { lay->usr_a, lay->usr_b }, .slot = lay->usr_slot, .img = img_usr, .active = -1 };
    bool ok = lay->fac_slot <= STORE_FAC_SLOT && lay->usr_slot <= STORE_USR_SLOT;
    if (dev && (lay->fac_b + lay->fac_slot > dev->size || lay->usr_b + lay->usr_slot > dev->size ||
                lay->fac_a + lay->fac_slot > dev->size || lay->usr_a + lay->usr_slot > dev->size)) ok = false;

    for (size_t i = 0; i < param_count(); ++i) {
        const param_def_t *d = param_at(i);
        if (!(d->flags & PF_PERSIST)) continue;
        if (S.n_items >= MAX_ITEMS) { ok = false; break; }
        S.items[S.n_items++] = (item_t){ .id = d->id,
                                         .size = PARAM_IS_BLOB(d) ? d->size : (uint16_t)param_ctype_size(d->ctype),
                                         .region = (uint8_t)((d->flags & PF_FACTORY) ? 0u : 1u), .pdef = d };
    }
    size_t n_plat = 0;
    const retain_def_t *plat = plat_store_blocks(&n_plat);
    for (size_t k = 0; k < n_plat + n_blocks; ++k) {
        bool is_plat = k < n_plat;
        const retain_def_t *b = is_plat ? &plat[k] : &blocks[k - n_plat];
        bool id_ok = is_plat ? (b->id >= PARAM_ID_PLAT_BLOCK_FIRST && b->id < PARAM_ID_APP_FIRST) : b->id >= PARAM_ID_APP_FIRST;
        if (S.n_items >= MAX_ITEMS || !id_ok || b->size == 0 || !b->ptr ||
            find_item(b->id) || param_find(b->id)) { ok = false; continue; }
        S.items[S.n_items++] = (item_t){ .id = b->id, .size = b->size,
                                         .region = (uint8_t)((b->flags & RF_FACTORY) ? 0u : 1u), .rdef = b };
    }
    if (region_need(REGION_FAC) > S.rg[REGION_FAC].slot || region_need(REGION_USR) > S.rg[REGION_USR].slot) ok = false;
    if (!ok) set_status(PSTAT_TABLE_ERROR);
    if (!dev || !dev->persistent) set_status(PSTAT_NVM_VOLATILE);   /* no FRAM answered, or RAM fallback */
    return ok;
}

/* ---------------- payload build / apply ---------------- */
static uint32_t build_payload(uint8_t region, uint8_t *out, uint32_t cap) {
    uint32_t n = 0;
    for (size_t k = 0; k < S.n_items; ++k) {
        const item_t *it = &S.items[k];
        if (it->region != region) continue;
        uint32_t padded = (it->size + 3u) & ~3u;
        if (n + 4u + padded > cap) break;              /* cannot happen after init check */
        put16(out + n, it->id); put16(out + n + 2, it->size);
        const void *src = it->pdef ? it->pdef->ptr : it->rdef->ptr;
        memcpy(out + n + 4, src, it->size);
        memset(out + n + 4 + it->size, 0, padded - it->size);
        n += 4u + padded;
    }
    return n;
}

/* Is a stored parameter value acceptable? (decoded exactly as the tag holds it) */
static bool param_bytes_ok(const param_def_t *d, const uint8_t *data) {
    if (PARAM_IS_BLOB(d)) return param_blob_image_ok(d, data);
    param_def_t tmp = *d;
    union { uint8_t b[4]; float f; } scratch = { { 0 } };
    memcpy(scratch.b, data, param_ctype_size(d->ctype));
    tmp.ptr = scratch.b;
    plat_val_t v = param_tag_get(&tmp);
    if (d->ptype == PT_FLOAT) return isfinite(v.f) && v.f >= d->vmin.f && v.f <= d->vmax.f;
    if (d->ptype == PT_BOOL) return scratch.b[0] <= 1u;
    if (d->ptype == PT_UINT) return v.u >= d->vmin.u && v.u <= d->vmax.u;
    return v.i >= d->vmin.i && v.i <= d->vmax.i;
}

/* Apply records to tags. Returns false if the payload is malformed (nothing is
 * applied in that case: a first pass checks the structure). */
static bool apply_payload(uint8_t region, const uint8_t *p, uint32_t len, uint16_t version, bool dry_run) {
    /* pass 1: structure */
    for (uint32_t n = 0; n < len;) {
        if (len - n < 4u) return false;
        uint16_t sz = get16(p + n + 2);
        uint32_t padded = ((uint32_t)sz + 3u) & ~3u;
        if (padded > len - n - 4u) return false;
        n += 4u + padded;
    }
    if (dry_run) return true;
    /* pass 2: values */
    for (uint32_t n = 0; n < len;) {
        uint16_t id = get16(p + n), sz = get16(p + n + 2);
        uint32_t padded = ((uint32_t)sz + 3u) & ~3u;
        uint8_t rec[512];
        const uint8_t *data = p + n + 4;
        n += 4u + padded;

        if (version < S.layout_version && S.migrate) {
            if (sz > sizeof rec) { S.st.skipped_records++; set_status(PSTAT_REC_SKIPPED); continue; }
            memcpy(rec, data, sz);
            if (!S.migrate(version, id, rec, &sz, (uint16_t)sizeof rec)) continue;   /* dropped on purpose */
            data = rec;
        }
        const item_t *it = find_item(id);
        if (!it || it->region != region || it->size != sz ||
            (it->pdef && !param_bytes_ok(it->pdef, data))) {
            S.st.skipped_records++; set_status(PSTAT_REC_SKIPPED);
            continue;                                   /* keep the default for this one */
        }
        memcpy(it->pdef ? it->pdef->ptr : it->rdef->ptr, data, sz);
    }
    return true;
}

static void block_defaults(int which) {
    for (size_t k = 0; k < S.n_items; ++k) {
        const item_t *it = &S.items[k];
        if (!it->rdef) continue;
        if (which == PARAM_USER_ONLY && it->region == REGION_FAC) continue;
        if (it->rdef->dflt) memcpy(it->rdef->ptr, it->rdef->dflt, it->size);
        else memset(it->rdef->ptr, 0, it->size);
    }
}

static void restore_defaults(void) {          /* scan context, via param_apply */
    param_set_defaults(PARAM_USER_ONLY);
    block_defaults(PARAM_USER_ONLY);
}

/* ---------------- NVM image I/O ---------------- */

/* Read and validate copy k of a region into its staging buffer. */
static bool read_copy(region_t *r, uint8_t region, int k, hdr_t *h) {
    if (!S.dev) return false;
    if (S.dev->read(S.dev, r->addr[k], r->img, STORE_HDR_SIZE) != 0) return false;
    if (!hdr_read(r->img, region, r->slot, h)) return false;
    if (S.dev->read(S.dev, r->addr[k] + STORE_HDR_SIZE, r->img + STORE_HDR_SIZE, h->len) != 0) return false;
    return plat_crc32(0, r->img + STORE_HDR_SIZE, h->len) == h->crc;
}

static void load_region(uint8_t region) {
    region_t *r = &S.rg[region];
    hdr_t h[2]; bool ok[2];
    ok[0] = read_copy(r, region, 0, &h[0]);
    ok[1] = read_copy(r, region, 1, &h[1]);
    int pick = -1;
    if (ok[0] && ok[1]) pick = ((int32_t)(h[1].seq - h[0].seq) > 0) ? 1 : 0;
    else if (ok[0]) pick = 0;
    else if (ok[1]) pick = 1;
    if (pick < 0) { r->active = -1; r->seq = 0; r->valid_at_boot = false; return; }
    /* The staging buffer holds whatever copy 1 contained (valid or not): if copy 0
     * was chosen, read it again, and treat a failure now as no valid image. */
    if (pick == 0 && !read_copy(r, region, 0, &h[0])) pick = ok[1] ? 1 : -1;
    if (pick == 1 && !ok[1]) pick = -1;
    if (pick < 0) { r->active = -1; r->seq = 0; r->valid_at_boot = false; return; }
    r->active = pick; r->seq = h[pick].seq; r->valid_at_boot = true;
    r->saved_crc = h[pick].crc; r->have_saved = true;
    if (h[pick].version < S.layout_version) { set_status(PSTAT_MIGRATED); r->have_saved = false; /* rewrite */ }
    if (h[pick].version > S.layout_version) set_status(PSTAT_NEWER_LAYOUT);
    if (!apply_payload(region, r->img + STORE_HDR_SIZE, h[pick].len, h[pick].version, false)) {
        r->active = -1; r->valid_at_boot = false; r->have_saved = false;
    }
}

void param_store_boot(void) {
    param_set_defaults(PARAM_ALL);
    block_defaults(PARAM_ALL);
    param_on_restore_defaults = restore_defaults;
    load_region(REGION_FAC);
    load_region(REGION_USR);
    /* Fail safe for the factory region: with no valid image, the defaults now in
     * RAM become the baseline, so they are written only once a factory value is
     * actually changed. The user region is simply saved (have_saved = false). */
    region_t *f = &S.rg[REGION_FAC];
    if (!f->valid_at_boot) {
        f->payload_len = build_payload(REGION_FAC, f->img + STORE_HDR_SIZE, f->slot - STORE_HDR_SIZE);
        f->saved_crc = plat_crc32(0, f->img + STORE_HDR_SIZE, f->payload_len);
        f->have_saved = true;
    }
    if (S.rg[REGION_FAC].valid_at_boot) set_status(PSTAT_FAC_VALID); else set_status(PSTAT_CAL_INVALID);
    if (S.rg[REGION_USR].valid_at_boot) set_status(PSTAT_USR_VALID); else set_status(PSTAT_USR_DEFAULTED);
    S.st.seq_fac = S.rg[REGION_FAC].seq; S.st.seq_usr = S.rg[REGION_USR].seq;
    param_publish();          /* readers see the loaded values from the start */
}

static bool write_region(uint8_t region, uint32_t payload_len, uint32_t crc) {
    region_t *r = &S.rg[region];
    int target = (r->active == 0) ? 1 : 0;
    hdr_t h = { .version = S.layout_version, .region = region, .seq = r->seq + 1u, .len = payload_len, .crc = crc };
    hdr_write(r->img, &h);
    uint32_t a = r->addr[target];
    /* payload first, header last: a cut at any point leaves this copy invalid
     * and the other copy untouched */
    bool ok = S.dev->write(S.dev, a + STORE_HDR_SIZE, r->img + STORE_HDR_SIZE, payload_len) == 0 &&
              S.dev->write(S.dev, a, r->img, STORE_HDR_SIZE) == 0;
    if (ok) {                                  /* verify by reading back */
        uint8_t buf[128]; uint32_t c = 0;
        hdr_t back;
        ok = S.dev->read(S.dev, a, buf, STORE_HDR_SIZE) == 0 && hdr_read(buf, region, r->slot, &back) &&
             back.seq == h.seq && back.len == payload_len && back.crc == crc;
        for (uint32_t n = 0; ok && n < payload_len; n += sizeof buf) {
            uint32_t m = payload_len - n < sizeof buf ? payload_len - n : (uint32_t)sizeof buf;
            ok = S.dev->read(S.dev, a + STORE_HDR_SIZE + n, buf, m) == 0;
            c = plat_crc32(c, buf, m);
        }
        ok = ok && c == crc;
    }
    if (!ok) { S.st.write_errors++; set_status(PSTAT_WRITE_ERROR); return false; }
    r->active = target; r->seq = h.seq; r->saved_crc = crc; r->have_saved = true;
    if (region == REGION_FAC) { S.st.saves_fac++; S.st.seq_fac = r->seq; clr_status(PSTAT_CAL_INVALID); set_status(PSTAT_FAC_VALID); }
    else                      { S.st.saves_usr++; S.st.seq_usr = r->seq; }
    clr_status(PSTAT_WRITE_ERROR);
    return true;
}

/* ---------------- scan side ---------------- */
void param_store_scan_service(void) {
    uint8_t req = S.req;
    if (req == REQ_NONE) return;
    PLAT_DMB();
    if (req == REQ_SNAPSHOT) {
        for (uint8_t g = 0; g < 2; ++g)
            S.rg[g].payload_len = build_payload(g, S.rg[g].img + STORE_HDR_SIZE, S.rg[g].slot - STORE_HDR_SIZE);
    } else if (req == REQ_IMPORT) {
        for (uint8_t g = 0; g < 2; ++g)
            if (S.imp_buf[g]) (void)apply_payload(g, S.imp_buf[g], S.imp_len[g], S.imp_ver[g], false);
        S.imp_buf[0] = S.imp_buf[1] = NULL;
    }
    PLAT_DMB();
    S.req = REQ_NONE;
}

/* ---------------- store task side ---------------- */
/* Jobs from other tasks (the web server), run here so only the store task
 * touches the NVM device. */
enum { JOB_NONE = 0, JOB_EXPORT, JOB_IMPORT };
enum { JS_IDLE = 0, JS_PENDING, JS_RUNNING, JS_DONE };

static void run_job(void) {
    if (S.job_state == JS_PENDING) {
        PLAT_DMB();
        if (S.job == JOB_EXPORT) {
            S.job_result = (int32_t)param_store_export(S.job_buf, S.job_len);
            S.job_state = JS_DONE;
        } else {
            int rc = param_store_import(S.job_buf, S.job_len, S.job_level);
            if (rc != 0) { S.job_result = rc; S.job_state = JS_DONE; }
            else S.job_state = JS_RUNNING;                    /* applied by the next scan */
        }
    } else if (S.job_state == JS_RUNNING && !param_store_import_busy()) {
        S.job_result = 0;
        PLAT_DMB();
        S.job_state = JS_DONE;
    }
}

int param_store_job_start(bool is_export, uint8_t *buf, uint32_t len, uint8_t level) {
    if (S.job_state != JS_IDLE) return -1;
    S.job = is_export ? JOB_EXPORT : JOB_IMPORT;
    S.job_buf = buf; S.job_len = len; S.job_level = level;
    PLAT_DMB();
    S.job_state = JS_PENDING;
    return 0;
}

bool param_store_job_done(int32_t *result) {
    if (S.job_state != JS_DONE) return false;
    PLAT_DMB();
    *result = S.job_result;
    S.job_state = JS_IDLE;
    return true;
}

/* State machine, store task:
 *   idle --(period elapsed)--> request snapshot --(scan builds payloads)-->
 *   compare CRC with what NVM holds --> write changed regions --> idle */
void param_store_poll(uint32_t now_ms) {
    if (!S.dev || (S.status & PSTAT_TABLE_ERROR)) {
        if (S.job_state == JS_PENDING) { S.job_result = -10; S.job_state = JS_DONE; }   /* never leave a caller waiting */
        return;
    }
    if (!S.started) run_job();                          /* between saves: staging buffers are not in use */
    if (S.req != REQ_NONE) return;                      /* the scan has not serviced the request yet */

    if (S.started) {                                    /* snapshot complete: staging buffers are ours */
        S.started = false;
        for (uint8_t g = 0; g < 2; ++g) {
            region_t *r = &S.rg[g];
            uint32_t crc = plat_crc32(0, r->img + STORE_HDR_SIZE, r->payload_len);
            if (g == REGION_FAC) S.st.payload_fac = r->payload_len; else S.st.payload_usr = r->payload_len;
            if (r->have_saved && crc == r->saved_crc) continue;
            (void)write_region(g, r->payload_len, crc);
        }
        S.last_save_ms = now_ms;
        S.first_done = true;
        return;
    }
    if (S.first_done && (uint32_t)(now_ms - S.last_save_ms) < STORE_SAVE_PERIOD_MS) return;
    S.started = true;
    PLAT_DMB();
    S.req = REQ_SNAPSHOT;
}

bool          param_store_calibration_valid(void) { return !(S.status & PSTAT_CAL_INVALID); }
uint32_t      param_store_status(void) { return S.status; }
store_stats_t param_store_stats(void) { return S.st; }

/* ---------------- backup file ----------------
 * 0 "CEMX" | 4 version u16 | 6 rsvd | 8 fac_len | 12 usr_len | 16 crc of bytes 20.. | 20 fac image | usr image */
uint32_t param_store_export(uint8_t *buf, uint32_t cap) {
    uint32_t len[2] = { 0, 0 };
    uint32_t n = 20;
    if (!S.dev) return 0;
    for (uint8_t g = 0; g < 2; ++g) {
        region_t *r = &S.rg[g];
        if (r->active < 0) continue;
        uint8_t h[STORE_HDR_SIZE]; hdr_t x;
        if (S.dev->read(S.dev, r->addr[r->active], h, STORE_HDR_SIZE) != 0 || !hdr_read(h, g, r->slot, &x)) continue;
        uint32_t total = STORE_HDR_SIZE + x.len;
        if (n + total > cap) return 0;
        if (S.dev->read(S.dev, r->addr[r->active], buf + n, total) != 0) return 0;
        if (plat_crc32(0, buf + n + STORE_HDR_SIZE, x.len) != x.crc) return 0;
        len[g] = total; n += total;
    }
    if (len[0] + len[1] == 0 || cap < 20) return 0;
    put32(buf, XMAGIC); put16(buf + 4, 1); put16(buf + 6, 0);
    put32(buf + 8, len[0]); put32(buf + 12, len[1]);
    put32(buf + 16, plat_crc32(0, buf + 20, n - 20));
    return n;
}

int param_store_import(const uint8_t *buf, uint32_t len, uint8_t level) {
    if (S.req != REQ_NONE || S.started) return -1;     /* busy */
    if (len < 20 || get32(buf) != XMAGIC || get16(buf + 4) != 1) return -2;
    uint32_t l[2] = { get32(buf + 8), get32(buf + 12) };
    if (l[0] > len || l[1] > len || 20u + l[0] + l[1] != len) return -2;
    if (get32(buf + 16) != plat_crc32(0, buf + 20, len - 20)) return -3;
    const uint8_t *p = buf + 20;
    const uint8_t *pay[2] = { NULL, NULL }; uint32_t pl[2] = { 0, 0 }; uint16_t ver[2] = { 0, 0 };
    for (uint8_t g = 0; g < 2; ++g) {
        if (l[g] == 0) continue;
        hdr_t x;
        if (l[g] < STORE_HDR_SIZE || !hdr_read(p, g, S.rg[g].slot, &x) || STORE_HDR_SIZE + x.len != l[g]) return -4;
        if (plat_crc32(0, p + STORE_HDR_SIZE, x.len) != x.crc) return -4;
        if (!apply_payload(g, p + STORE_HDR_SIZE, x.len, x.version, true)) return -4;
        if (g == REGION_FAC && level < PLAT_LVL_FACTORY) { p += l[g]; continue; }
        pay[g] = p + STORE_HDR_SIZE; pl[g] = x.len; ver[g] = x.version;
        p += l[g];
    }
    if (!pay[0] && !pay[1]) return -5;                 /* nothing this level may restore */
    for (uint8_t g = 0; g < 2; ++g) { S.imp_buf[g] = pay[g]; S.imp_len[g] = pl[g]; S.imp_ver[g] = ver[g]; }
    PLAT_DMB();
    S.req = REQ_IMPORT;
    return 0;
}

bool param_store_import_busy(void) { return S.req == REQ_IMPORT; }
