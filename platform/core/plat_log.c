/* plat_log.c - see plat_log.h */
#include <math.h>
#include <string.h>
#include "plat_log.h"
#include "plat_port.h"
#include "param.h"
#include "param_store.h"
#include "ul_system.h"
#include "bsp.h"

extern uint16_t plat_log_interval_s;      /* PARAM_ID_LOG_INTERVAL_S (param.c) */
extern uint32_t plat_log_status_word;     /* PARAM_ID_LOG_STATUS */

#define MAX_REC_SLOTS (1u + (LOG_MAX_CHANS + 1u) / 2u)   /* the schema: 17 */
#define DRAIN_PER_POLL 32u
#define FPTR_MAGIC 0x4C4F4750u                           /* "PGOL" */

/* ---------------- little-endian helpers ---------------- */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void putf64(uint8_t *p, double v) { uint64_t u; memcpy(&u, &v, 8); put32(p, (uint32_t)u); put32(p + 4, (uint32_t)(u >> 32)); }
static double getf64(const uint8_t *p) { uint64_t u = get32(p) | ((uint64_t)get32(p + 4) << 32); double v; memcpy(&v, &u, 8); return v; }

/* ---------------- calendar ---------------- */
uint32_t plat_unix_from_civil(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s) {
    int yy = (int)y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153u * (mo + (mo > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    if (days < 0) return 0;
    return (uint32_t)days * 86400u + h * 3600u + mi * 60u + s;
}

void plat_civil_from_unix(uint32_t t, unsigned *y, unsigned *mo, unsigned *d, unsigned *h, unsigned *mi, unsigned *s) {
    uint32_t days = t / 86400u, rem = t % 86400u;
    *h = rem / 3600u; *mi = (rem / 60u) % 60u; *s = rem % 60u;
    long z = (long)days + 719468L;
    long era = z / 146097L;
    unsigned doe = (unsigned)(z - era * 146097L);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *mo = mp < 10u ? mp + 3u : mp - 9u;
    *y = (unsigned)(yoe + era * 400L) + (*mo <= 2u);
}

/* ---------------- slots ---------------- */
static uint32_t slot_crc(const uint8_t *s) { return plat_crc32(0, s, LOG_SLOT - 4u); }

bool plat_log_slot_ok(const uint8_t *s) {
    return s[0] == LOG_MAGIC && s[3] >= 1u && s[2] < s[3] && get32(s + 60) == slot_crc(s);
}

static bool slot_erased(const uint8_t *s) {
    for (unsigned i = 0; i < LOG_SLOT; ++i) if (s[i] != 0xFFu) return false;
    return true;
}

static void slot_head(uint8_t *s, uint8_t type, bool clock_set, uint8_t part, uint8_t nparts, uint32_t t) {
    memset(s, 0, LOG_SLOT);
    s[0] = LOG_MAGIC;
    s[1] = (uint8_t)(type | (clock_set ? 0u : LOG_F_NOCLOCK));
    s[2] = part;
    s[3] = nparts;
    put32(s + 8, t);
}

/* ---------------- RAM queue (many producers, the store task consumes) ---------------- */
static struct {
    uint8_t  q[PLAT_LOG_QSLOTS][LOG_SLOT];
    uint32_t head, tail;            /* free-running slot counters */
    uint32_t lost;                  /* records lost since last reported */
    uint32_t clk_t, clk_edge_ms;    /* log clock: time at the last second edge */
    bool     clk_set;
} Q;

static bool q_push(const uint8_t *slots, unsigned n) {
    bool ok = false;
    PLAT_CRIT_ENTER();
    if (PLAT_LOG_QSLOTS - (Q.head - Q.tail) >= n) {
        for (unsigned i = 0; i < n; ++i) memcpy(Q.q[(Q.head + i) % PLAT_LOG_QSLOTS], slots + i * LOG_SLOT, LOG_SLOT);
        Q.head += n;
        ok = true;
    } else {
        Q.lost++;
    }
    PLAT_CRIT_EXIT();
    return ok;
}

/* one whole record out of the queue; returns its slot count, 0 if empty */
static unsigned q_pop(uint8_t *out) {
    unsigned n = 0;
    PLAT_CRIT_ENTER();
    if (Q.head != Q.tail) {
        n = Q.q[Q.tail % PLAT_LOG_QSLOTS][3];
        if (n == 0 || n > MAX_REC_SLOTS || n > Q.head - Q.tail) {   /* cannot happen; resynchronise */
            Q.tail = Q.head;
            n = 0;
        } else {
            for (unsigned i = 0; i < n; ++i) memcpy(out + i * LOG_SLOT, Q.q[(Q.tail + i) % PLAT_LOG_QSLOTS], LOG_SLOT);
            Q.tail += n;
        }
    }
    PLAT_CRIT_EXIT();
    return n;
}

static void clock_now(uint32_t *t, uint16_t *ms, bool *set) {
    uint32_t now = bsp_ms();
    PLAT_CRIT_ENTER();
    uint32_t d = now - Q.clk_edge_ms;
    if (d > 0x80000000u) d = 0;                 /* edge set from a later reading than now */
    *t = Q.clk_t + d / 1000u;
    *ms = (uint16_t)(d % 1000u);
    *set = Q.clk_set;
    PLAT_CRIT_EXIT();
}

/* ---------------- events ---------------- */
void plat_log_event(uint16_t code, uint8_t src, uint8_t level, uint32_t arg,
                    double old_v, double new_v, const char *text) {
    uint8_t s[LOG_SLOT];
    uint32_t t; uint16_t ms; bool set;
    clock_now(&t, &ms, &set);
    slot_head(s, LOG_T_EVENT, set, 0, 1, t);
    put16(s + 12, code);
    s[14] = src;
    s[15] = level;
    put32(s + 16, arg);
    putf64(s + 20, old_v);
    putf64(s + 28, new_v);
    put16(s + 36, ms);
    if (text) for (unsigned i = 0; i < 22u && text[i]; ++i) s[38 + i] = (uint8_t)text[i];
    q_push(s, 1);
}

bool plat_log_decode_event(const uint8_t *s, log_event_t *e) {
    if (!plat_log_slot_ok(s) || (s[1] & 0x7Fu) != LOG_T_EVENT) return false;
    e->seq = get32(s + 4);
    e->time = get32(s + 8);
    e->clock_set = !(s[1] & LOG_F_NOCLOCK);
    e->code = get16(s + 12);
    e->src = s[14];
    e->level = s[15];
    e->arg = get32(s + 16);
    e->old_v = getf64(s + 20);
    e->new_v = getf64(s + 28);
    e->ms = get16(s + 36);
    memcpy(e->text, s + 38, 22);
    e->text[22] = 0;
    return true;
}

const char *plat_log_event_name(uint16_t c) {
    static const char *const n[] = {
        [LOG_EV_POWER_UP] = "power_up", [LOG_EV_PARAM] = "param", [LOG_EV_MODE] = "mode",
        [LOG_EV_RESTORE_DEFAULTS] = "restore_defaults", [LOG_EV_TIME_SET] = "time_set",
        [LOG_EV_LOGIN] = "login", [LOG_EV_LOGIN_FAIL] = "login_fail", [LOG_EV_LOGOUT] = "logout",
        [LOG_EV_PASSWORD] = "password", [LOG_EV_RESTORE_BACKUP] = "restore_backup",
        [LOG_EV_ALARM_RAISE] = "alarm", [LOG_EV_ALARM_CLEAR] = "alarm_clear", [LOG_EV_ALARM_ACK] = "alarm_ack",
        [LOG_EV_FW_UPDATE] = "fw_update", [LOG_EV_FW_ROLLBACK] = "fw_rollback",
        [LOG_EV_LOG_FAULT] = "log_fault", [LOG_EV_STORE_FAULT] = "store_fault", [LOG_EV_LOST] = "events_lost",
        [LOG_EV_IP_CONFLICT] = "ip_conflict", [LOG_EV_TLS_KEY] = "tls_new_key",
        [LOG_EV_MAIL] = "mail", [LOG_EV_SERIAL] = "serial", [LOG_EV_IOB] = "iob", [LOG_EV_STATE] = "state", [LOG_EV_POWER_FAIL] = "power_fail", [LOG_EV_POWER_RESTORED] = "power_restored", [LOG_EV_CAL] = "cal", [LOG_EV_SERVICE] = "service", [LOG_EV_SELFTEST] = "selftest", [LOG_EV_FORCE] = "force", [LOG_EV_ETH_SPEED] = "eth_speed", [LOG_EV_QAL3] = "qal3",
    };
    if (c < sizeof n / sizeof n[0] && n[c]) return n[c];
    return c >= LOG_EV_APP ? "app" : "unknown";
}

static double val_double(const param_def_t *d, plat_val_t v) {
    switch (d->ptype) {
    case PT_BOOL:  return v.b ? 1.0 : 0.0;
    case PT_INT:   return (double)v.i;
    case PT_UINT:  return (double)v.u;
    case PT_FLOAT: return (double)v.f;
    default:       return NAN;
    }
}

static void on_param_change(const param_def_t *d, plat_val_t o, plat_val_t n, uint8_t src, uint8_t level) {
    if (src == PLAT_SRC_INTERNAL) return;
    uint16_t code = d->id == PARAM_ID_CTRL_MODE ? LOG_EV_MODE
                  : d->id == PARAM_ID_RESTORE_DEFAULTS ? LOG_EV_RESTORE_DEFAULTS : LOG_EV_PARAM;
    double ov = val_double(d, o), nv = val_double(d, n);
    if (code == LOG_EV_RESTORE_DEFAULTS) ov = nv = NAN;
    plat_log_event(code, src, level, d->id, ov, nv, d->name);
}

/* ---------------- data channels (scan task) ---------------- */
static const plat_log_chan_t *g_chans;
static unsigned g_nch;

static struct { double sum; uint32_t n; uint8_t q; } acc[LOG_MAX_CHANS];
static struct {
    bool     started, last_set;
    uint32_t period, last_t, last_ms;
    uint32_t iv;                 /* interval the current schema was written for; 0xFFFFFFFF = none yet */
} S;
static volatile bool scan_no_clock;
uint32_t plat_log_expected_time;          /* plat_clock.c: a requested step, already logged with its source */

void plat_log_put(unsigned ch, float v, uint8_t q) {
    if (ch >= g_nch) return;
    if (q == LQ_NA) return;
    if (!isfinite(v)) { acc[ch].q = LQ_BAD; return; }
    acc[ch].sum += (double)v;
    acc[ch].n++;
    if (q > acc[ch].q) acc[ch].q = q > LQ_BAD ? LQ_BAD : q;
}

uint16_t plat_log_schema_hash(const plat_log_chan_t *c, unsigned n, uint16_t iv) {
    uint8_t h[4] = { (uint8_t)n, (uint8_t)iv, (uint8_t)(iv >> 8), 0 };
    uint32_t crc = plat_crc32(0, h, 3);
    for (unsigned i = 0; i < n; ++i) {
        if (c[i].name) crc = plat_crc32(crc, c[i].name, (uint32_t)strlen(c[i].name));
        crc = plat_crc32(crc, "|", 1);
        if (c[i].unit) crc = plat_crc32(crc, c[i].unit, (uint32_t)strlen(c[i].unit));
        crc = plat_crc32(crc, &c[i].dec, 1);
    }
    return (uint16_t)(crc ^ (crc >> 16));
}

static void copy_text(uint8_t *dst, const char *s, unsigned cap) {
    if (s) for (unsigned i = 0; i < cap && s[i]; ++i) dst[i] = (uint8_t)s[i];
}

static void emit_schema(uint32_t t, bool set, uint16_t iv) {
    static uint8_t buf[MAX_REC_SLOTS * LOG_SLOT];   /* scan task only */
    unsigned np = 1u + (g_nch + 1u) / 2u;
    uint16_t hash = plat_log_schema_hash(g_chans, g_nch, iv);
    slot_head(buf, LOG_T_SCHEMA, set, 0, (uint8_t)np, t);
    put16(buf + 12, hash);
    buf[14] = (uint8_t)g_nch;
    put16(buf + 16, iv);
    for (unsigned p = 1; p < np; ++p) {
        uint8_t *s = buf + p * LOG_SLOT;
        slot_head(s, LOG_T_SCHEMA, set, (uint8_t)p, (uint8_t)np, t);
        for (unsigned k = 0; k < 2u; ++k) {
            unsigned ch = (p - 1u) * 2u + k;
            if (ch >= g_nch) break;
            uint8_t *e = s + 12 + 24u * k;
            copy_text(e, g_chans[ch].name, 16);
            copy_text(e + 16, g_chans[ch].unit, 6);
            e[22] = g_chans[ch].dec;
            e[23] = 1;                  /* present */
        }
    }
    q_push(buf, np);
}

static void emit_data(uint32_t t, bool set, uint16_t iv) {
    uint8_t buf[(LOG_MAX_CHANS / LOG_VALS_PER_SLOT) * LOG_SLOT];
    unsigned np = (g_nch + LOG_VALS_PER_SLOT - 1u) / LOG_VALS_PER_SLOT;
    uint16_t hash = plat_log_schema_hash(g_chans, g_nch, iv);
    for (unsigned p = 0; p < np; ++p) {
        uint8_t *s = buf + p * LOG_SLOT;
        slot_head(s, LOG_T_DATA, set, (uint8_t)p, (uint8_t)np, t);
        put16(s + 12, hash);
        s[14] = (uint8_t)(p * LOG_VALS_PER_SLOT);
        unsigned k = 0;
        for (; k < LOG_VALS_PER_SLOT; ++k) {
            unsigned ch = p * LOG_VALS_PER_SLOT + k;
            if (ch >= g_nch) break;
            float v = acc[ch].n ? (float)(acc[ch].sum / (double)acc[ch].n) : NAN;
            uint8_t q = acc[ch].n ? acc[ch].q : (acc[ch].q == LQ_BAD ? LQ_BAD : LQ_NA);
            uint32_t u; memcpy(&u, &v, 4);
            put32(s + 16 + 5u * k, u);
            s[20 + 5u * k] = q;
        }
        s[15] = (uint8_t)k;
    }
    if (np) q_push(buf, np);
}

static void acc_reset(void) { memset(acc, 0, sizeof acc); }

void plat_log_scan(uint32_t now_ms) {
    bool set = RTC_Date.Year >= 2000u && RTC_Date.Month >= 1u && RTC_Date.Month <= 12u
            && RTC_Date.Day_In_Month >= 1u;
    uint32_t t = set ? plat_unix_from_civil(RTC_Date.Year, RTC_Date.Month, RTC_Date.Day_In_Month,
                                            RTC_Time.Hours, RTC_Time.Minutes, RTC_Time.Seconds)
                     : now_ms / 1000u;
    scan_no_clock = !set;

    PLAT_CRIT_ENTER();
    if (t != Q.clk_t || set != Q.clk_set) { Q.clk_t = t; Q.clk_edge_ms = now_ms; Q.clk_set = set; }
    PLAT_CRIT_EXIT();

    uint16_t iv = plat_log_interval_s;
    if (S.started) {
        /* clock set or stepped? (a late scan may legitimately move it by more than 1 s) */
        uint32_t el = (now_ms - S.last_ms + 999u) / 1000u;
        bool jump = set != S.last_set || t < S.last_t || t > S.last_t + el + 2u;
        bool expected = plat_log_expected_time && t + 2u >= plat_log_expected_time && t <= plat_log_expected_time + 2u;
        if (jump) plat_log_expected_time = 0;
        if (jump && set && !expected) {
            plat_log_event(LOG_EV_TIME_SET, PLAT_SRC_INTERNAL, 0, 0,
                           S.last_set ? (double)S.last_t : NAN, (double)t, NULL);
        }
        if (jump || iv != S.iv) {           /* close the interval so far, start a new one */
            if (S.iv != 0xFFFFFFFFu && S.iv != 0 && g_nch) emit_data(S.last_t, S.last_set, (uint16_t)S.iv);
            acc_reset();
            S.started = false;
        }
    }
    if (iv != S.iv) {
        if (iv != 0 && g_nch) emit_schema(t, set, iv);
        S.iv = iv;
    }
    if (iv == 0 || g_nch == 0) {
        acc_reset();
    } else if (!S.started) {
        S.period = t / iv;
        S.started = true;
    } else if (t / iv != S.period) {
        emit_data((S.period + 1u) * iv, set, iv);
        acc_reset();
        S.period = t / iv;
    }
    S.started = true;
    S.last_t = t;
    S.last_ms = now_ms;
    S.last_set = set;
}

/* ---------------- flash rings (store task) ---------------- */
typedef struct {
    uint32_t first_sector, nsect, nslots;
    uint32_t wr, oldest, seq;          /* next slot, oldest record's first slot, next seq */
    int32_t  pre_erased;               /* sector known erased ahead, -1 = unknown */
} ring_t;

static struct {
    bool       on;
    nor_dev_t *nor;
    nvm_dev_t *fram;
    uint32_t   fram_addr;
    uint32_t   status;
    uint32_t   gen;
    uint32_t   sps;                    /* slots per sector */
    ring_t     r[LOG_RINGS];
    bool       ptr_dirty;
    uint32_t   last_pstat;
    /* read job */
    volatile uint8_t job;              /* 0 idle, 1 pending, 2 done */
    uint8_t    job_ring;
    uint32_t   job_pos, job_n;
    uint8_t   *job_buf;
    int32_t    job_result;
} L;

static uint32_t slot_addr(const ring_t *r, uint32_t idx) {
    return r->first_sector * L.nor->sector + idx * LOG_SLOT;
}
static int read_slot(const ring_t *r, uint32_t idx, uint8_t *s) {
    return L.nor->read(L.nor, slot_addr(r, idx), s, LOG_SLOT);
}
static void fault(uint32_t bits) {
    if (!(L.status & bits)) plat_log_event(LOG_EV_LOG_FAULT, PLAT_SRC_INTERNAL, 0, bits, NAN, NAN, NULL);
    L.status |= bits;
}

static bool erase_sector(ring_t *r, uint32_t x) {
    if (r->oldest != r->wr && r->oldest / L.sps == x)        /* the oldest data goes */
        r->oldest = ((x + 1u) % r->nsect) * L.sps;
    L.ptr_dirty = true;
    uint32_t a = (r->first_sector + x) * L.nor->sector;
    if (L.nor->erase(L.nor, a) != 0) { fault(LSTAT_NOR_FAULT); return false; }
    uint8_t s[LOG_SLOT];                                     /* spot-check first and last slot */
    if (L.nor->read(L.nor, a, s, LOG_SLOT) != 0 || !slot_erased(s) ||
        L.nor->read(L.nor, a + L.nor->sector - LOG_SLOT, s, LOG_SLOT) != 0 || !slot_erased(s)) {
        fault(LSTAT_NOR_FAULT);
        return false;
    }
    return true;
}

/* make slot r->wr writable; returns false if no usable slot was found */
static bool prepare(ring_t *r) {
    uint8_t s[LOG_SLOT];
    for (uint32_t tries = 0; tries < 3u * L.sps; ++tries) {
        if (r->wr % L.sps == 0) {                            /* entering a sector */
            uint32_t sec = r->wr / L.sps;
            bool ok = (r->pre_erased == (int32_t)sec) || erase_sector(r, sec);
            r->pre_erased = -1;
            if (!ok) { r->wr = ((sec + 1u) % r->nsect) * L.sps; continue; }
            uint32_t nx = (sec + 1u) % r->nsect;             /* erase ahead: the next sector */
            if (erase_sector(r, nx)) r->pre_erased = (int32_t)nx;
            return true;
        }
        if (read_slot(r, r->wr, s) == 0 && slot_erased(s)) return true;
        r->wr = (r->wr + 1u) % r->nslots;                    /* torn or unknown slot: skip it */
    }
    return false;
}

static bool put_slot(ring_t *r, const uint8_t *s) {
    for (unsigned tries = 0; tries < 3u; ++tries) {
        if (!prepare(r)) { fault(LSTAT_NOR_FAULT); return false; }
        uint32_t a = slot_addr(r, r->wr);
        uint8_t chk[LOG_SLOT];
        bool ok = L.nor->prog(L.nor, a, s, LOG_SLOT) == 0 &&
                  L.nor->read(L.nor, a, chk, LOG_SLOT) == 0 && memcmp(chk, s, LOG_SLOT) == 0;
        r->wr = (r->wr + 1u) % r->nslots;
        L.ptr_dirty = true;
        if (ok) return true;
        fault(LSTAT_NOR_FAULT);
    }
    return false;
}

static void write_record(uint8_t *rec, unsigned n) {
    ring_t *r = &L.r[(rec[1] & 0x7Fu) == LOG_T_EVENT ? LOG_RING_EVENTS : LOG_RING_DATA];
    uint32_t seq = r->seq++;
    for (unsigned i = 0; i < n; ++i) {
        uint8_t *s = rec + i * LOG_SLOT;
        put32(s + 4, seq);
        put32(s + 60, slot_crc(s));
        if (!put_slot(r, s)) return;                         /* rest of this record is lost */
    }
}

/* ---- write pointer in FRAM: two copies, the newer valid one wins ---- */
typedef struct { uint32_t magic, gen, geom; struct { uint32_t wr, oldest, seq; } r[LOG_RINGS]; uint32_t crc; } fptr_t;

static uint32_t geom_word(void) { return L.r[0].nslots ^ (L.r[1].nslots << 8) ^ (L.r[1].first_sector << 20); }

static void ptr_save(void) {
    L.ptr_dirty = false;
    if (!L.fram) return;
    fptr_t p = { .magic = FPTR_MAGIC, .gen = ++L.gen, .geom = geom_word() };
    for (unsigned i = 0; i < LOG_RINGS; ++i) { p.r[i].wr = L.r[i].wr; p.r[i].oldest = L.r[i].oldest; p.r[i].seq = L.r[i].seq; }
    p.crc = plat_crc32(0, &p, (uint32_t)offsetof(fptr_t, crc));
    L.fram->write(L.fram, L.fram_addr + (L.gen & 1u) * 64u, &p, sizeof p);
}

static bool ptr_load(fptr_t *out) {
    bool have = false;
    if (!L.fram) return false;
    for (unsigned c = 0; c < 2u; ++c) {
        fptr_t p;
        if (L.fram->read(L.fram, L.fram_addr + c * 64u, &p, sizeof p) != 0) continue;
        if (p.magic != FPTR_MAGIC || p.crc != plat_crc32(0, &p, (uint32_t)offsetof(fptr_t, crc))) continue;
        if (p.geom != geom_word()) continue;
        if (!have || (int32_t)(p.gen - out->gen) > 0) { *out = p; have = true; }
    }
    return have;
}

/* trust the FRAM position only if the flash agrees; then step over anything newer */
static bool ring_from_ptr(ring_t *r, uint32_t wr, uint32_t oldest, uint32_t seq) {
    uint8_t s[LOG_SLOT];
    if (wr >= r->nslots || oldest >= r->nslots || seq == 0) return false;
    if (seq > 1u) {
        uint32_t prev = (wr + r->nslots - 1u) % r->nslots;
        if (read_slot(r, prev, s) != 0 || !plat_log_slot_ok(s) || get32(s + 4) != seq - 1u) return false;
    }
    for (uint32_t k = 0; k < r->nslots; ++k) {                 /* written after the pointer was saved */
        if (read_slot(r, wr, s) != 0) break;
        if (plat_log_slot_ok(s)) {
            uint32_t q = get32(s + 4);
            bool newer = q >= seq || (s[2] > 0 && q + 1u == seq);   /* (later parts share the seq) */
            if (!newer) break;
            seq = q + 1u;
        } else if (slot_erased(s) || wr % L.sps == 0) {
            break;      /* end; or a sector that may be half erased: let prepare() erase it */
        }                                                       /* torn slot: step over it */
        wr = (wr + 1u) % r->nslots;
    }
    (void)oldest;                       /* recomputed by find_oldest(): erases after the save move it */
    r->wr = wr; r->seq = seq;
    return true;
}

/* first valid slot of a sector: 1 = found (seq, idx), 0 = sector empty, -1 = nothing valid */
static int sector_first(const ring_t *r, uint32_t sec, uint32_t *seq, uint32_t *idx) {
    uint8_t s[LOG_SLOT];
    for (uint32_t k = 0; k < L.sps; ++k) {
        uint32_t i = sec * L.sps + k;
        if (read_slot(r, i, s) != 0) return -1;
        if (k == 0 && slot_erased(s)) return 0;
        if (plat_log_slot_ok(s)) { *seq = get32(s + 4); *idx = i; return 1; }
        if (slot_erased(s)) return -1;
    }
    return -1;
}

static bool ring_rebuild(ring_t *r) {   /* returns true if the flash held records */
    uint32_t best = 0, wsec = 0, seq, idx;
    bool any = false;
    for (uint32_t sec = 0; sec < r->nsect; ++sec) {
        if (sector_first(r, sec, &seq, &idx) == 1 && (!any || seq > best)) { best = seq; wsec = sec; any = true; }
    }
    r->pre_erased = -1;
    if (!any) { r->wr = 0; r->oldest = 0; r->seq = 1; return false; }
    uint8_t s[LOG_SLOT];
    uint32_t last = wsec * L.sps, last_seq = best;
    for (uint32_t k = 0; k < L.sps; ++k) {
        uint32_t i = wsec * L.sps + k;
        if (read_slot(r, i, s) == 0 && plat_log_slot_ok(s) && get32(s + 4) >= last_seq) { last = i; last_seq = get32(s + 4); }
    }
    r->wr = (last + 1u) % r->nslots;
    r->seq = last_seq + 1u;
    return true;
}

/* oldest record: the first sector holding data, going round from the write position
 * (the sector being written comes last; at a sector start that sector comes first,
 * since it still holds the oldest data until it is entered and erased) */
static void find_oldest(ring_t *r) {
    uint32_t seq, idx, wsec = r->wr / L.sps;
    uint32_t start = (r->wr % L.sps == 0) ? wsec : (wsec + 1u) % r->nsect;
    for (uint32_t k = 0; k < r->nsect; ++k) {
        uint32_t sec = (start + k) % r->nsect;
        if (sector_first(r, sec, &seq, &idx) == 1) {
            if (sec == wsec && idx >= r->wr && r->wr % L.sps != 0) continue;   /* (stale slot past wr) */
            r->oldest = idx;
            return;
        }
    }
    r->oldest = r->wr;                                         /* empty */
}

bool plat_log_init(const plat_log_cfg_t *cfg, uint32_t now_ms) {
    memset(&L, 0, sizeof L);
    g_chans = cfg->chans;
    g_nch = cfg->n_chans > LOG_MAX_CHANS ? LOG_MAX_CHANS : cfg->n_chans;
    PLAT_CRIT_ENTER();
    Q.clk_set = cfg->unix_now != 0;
    Q.clk_t = Q.clk_set ? cfg->unix_now : now_ms / 1000u;
    Q.clk_edge_ms = now_ms;
    PLAT_CRIT_EXIT();
    param_on_change = on_param_change;

    char txt[23] = "fw ";
    size_t k = 3;
    for (const char *p = cfg->fw_version; p && *p && k < 13u; ++p) txt[k++] = *p;
    if (cfg->reset_note && cfg->reset_note[0]) {
        txt[k++] = ' ';
        for (const char *p = cfg->reset_note; *p && k < 22u; ++p) txt[k++] = *p;
    }
    txt[k] = 0;
    plat_log_event(LOG_EV_POWER_UP, PLAT_SRC_INTERNAL, 0, cfg->reset_cause, NAN, NAN, txt);

    nor_dev_t *nor = cfg->nor;
    if (!nor || nor->sector == 0 || nor->sector % LOG_SLOT || nor->page % LOG_SLOT) { plat_log_status_word = L.status; return false; }
    L.nor = nor;
    L.fram = cfg->fram;
    L.fram_addr = cfg->fram_addr;
    L.sps = nor->sector / LOG_SLOT;
    uint32_t total = nor->size / nor->sector;
    uint32_t ev = total / 8u;
    if (ev < 3u) ev = 3u;
    if (ev > 256u) ev = 256u;
    if (total < ev + 3u) { L.status |= LSTAT_TOO_SMALL; plat_log_status_word = L.status; return false; }
    L.r[0] = (ring_t){ .first_sector = 0, .nsect = ev, .nslots = ev * L.sps, .pre_erased = -1 };
    L.r[1] = (ring_t){ .first_sector = ev, .nsect = total - ev, .nslots = (total - ev) * L.sps, .pre_erased = -1 };
    if (!nor->persistent) L.status |= LSTAT_VOLATILE;
    if (!L.fram || !L.fram->persistent) L.fram = NULL;

    fptr_t p;
    bool have = ptr_load(&p);
    if (have) L.gen = p.gen;
    for (unsigned i = 0; i < LOG_RINGS; ++i) {
        if (!have || !ring_from_ptr(&L.r[i], p.r[i].wr, p.r[i].oldest, p.r[i].seq)) {
            if (ring_rebuild(&L.r[i]) && nor->persistent && L.fram) L.status |= LSTAT_REBUILT;
        }
        find_oldest(&L.r[i]);
    }
    L.last_pstat = param_store_status();
    L.on = true;
    ptr_save();
    plat_log_status_word = L.status;
    return true;
}

uint32_t plat_log_status(void) { return L.status | (scan_no_clock ? LSTAT_NO_CLOCK : 0u); }

void plat_log_poll(uint32_t now_ms) {
    (void)now_ms;
    if (!L.on) return;
    uint32_t ps = param_store_status();
    uint32_t newbits = ps & ~L.last_pstat & (PSTAT_CAL_INVALID | PSTAT_WRITE_ERROR);
    L.last_pstat = ps;
    if (newbits) plat_log_event(LOG_EV_STORE_FAULT, PLAT_SRC_INTERNAL, 0, newbits, NAN, NAN, NULL);

    static uint8_t rec[MAX_REC_SLOTS * LOG_SLOT];
    for (unsigned k = 0; k < DRAIN_PER_POLL; ++k) {
        unsigned n = q_pop(rec);
        if (!n) break;
        write_record(rec, n);
    }

    uint32_t lost;                                  /* report losses once there is room again */
    PLAT_CRIT_ENTER();
    lost = Q.lost; Q.lost = 0;
    PLAT_CRIT_EXIT();
    if (lost) {
        L.status |= LSTAT_LOST;
        uint8_t s[LOG_SLOT];
        uint32_t t; uint16_t ms; bool set;
        clock_now(&t, &ms, &set);
        slot_head(s, LOG_T_EVENT, set, 0, 1, t);
        put16(s + 12, LOG_EV_LOST);
        s[14] = PLAT_SRC_INTERNAL;
        put32(s + 16, lost);
        putf64(s + 20, NAN); putf64(s + 28, NAN);
        put16(s + 36, ms);
        write_record(s, 1);                         /* straight to flash: the queue may be full again */
    }

    if (L.job == 1) {
        const ring_t *r = &L.r[L.job_ring];
        int32_t res = (int32_t)L.job_n;
        uint32_t pos = L.job_pos % r->nslots, done = 0;    /* (n = 0: just a barrier after the drain) */
        while (done < L.job_n) {                         /* at most two runs (the wrap) */
            uint32_t run = r->nslots - pos;
            if (run > L.job_n - done) run = L.job_n - done;
            if (L.nor->read(L.nor, slot_addr(r, pos), L.job_buf + done * LOG_SLOT, run * LOG_SLOT) != 0) { res = -1; break; }
            done += run;
            pos = (pos + run) % r->nslots;
        }
        L.job_result = res;
        PLAT_DMB();
        L.job = 2;
    }

    if (L.ptr_dirty) ptr_save();
    plat_log_status_word = plat_log_status();
}

bool plat_log_info(uint8_t ring, log_ring_info_t *o) {
    if (!L.on || ring >= LOG_RINGS) return false;
    PLAT_CRIT_ENTER();
    const ring_t *r = &L.r[ring];
    o->nslots = r->nslots;
    o->wr = r->wr;
    o->oldest = r->oldest;
    o->next_seq = r->seq;
    PLAT_CRIT_EXIT();
    o->used = (o->wr + o->nslots - o->oldest) % o->nslots;
    return true;
}

int plat_log_read_start(uint8_t ring, uint32_t pos, uint32_t n, uint8_t *buf) {
    if (!L.on || ring >= LOG_RINGS || (!buf && n)) return -1;
    int rc = -1;
    PLAT_CRIT_ENTER();
    if (L.job != 1) {             /* idle, or a finished job nobody collected */
        L.job_ring = ring; L.job_pos = pos; L.job_n = n; L.job_buf = buf;
        PLAT_DMB();
        L.job = 1;
        rc = 0;
    }
    PLAT_CRIT_EXIT();
    return rc;
}

bool plat_log_read_done(int32_t *result) {
    if (L.job != 2) return false;
    PLAT_DMB();
    *result = L.job_result;
    L.job = 0;
    return true;
}

void plat_log_reset_for_test(void) {
    memset(&Q, 0, sizeof Q);
    memset(&L, 0, sizeof L);
    memset(&S, 0, sizeof S);
    S.iv = 0xFFFFFFFFu;
    acc_reset();
    g_chans = NULL;
    g_nch = 0;
    scan_no_clock = false;
}
