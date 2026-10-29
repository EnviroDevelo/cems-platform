/* inst.c - third-party instruments on Modbus TCP (inst.h). Pure C: the network is passed in, so
 * the host tests run it against simulated instruments (platform/bsp/inst_sim.c). */
#include <math.h>
#include <string.h>
#include "inst.h"
#include "plat_port.h"
#include "plat_state.h"

#define EX_TIMEOUT_MS 2000u
#define MBAP_MAX      260u
#define EX_CONN       INST_CONN_MAX   /* the explorer's connection */

/* ---------------- tags (owned by the scan) ---------------- */
inst_tags_t    inst_tags;
inst_pt_tags_t inst_pt;

/* the settings the poller uses (names and units stay with the tags) */
typedef struct {
    bool     enable[INST_MAX];
    uint32_t ip[INST_MAX];
    uint16_t port[INST_MAX];
    uint8_t  unit[INST_MAX];
    uint16_t period_ms[INST_MAX], timeout_ms[INST_MAX];
    uint8_t  max_regs[INST_MAX], gap[INST_MAX], status_pt[INST_MAX];
    uint16_t fault_mask[INST_MAX], maint_mask[INST_MAX], cal_mask[INST_MAX];
    uint8_t  p_inst[INST_POINTS], p_fc[INST_POINTS], p_type[INST_POINTS], p_order[INST_POINTS], p_bit[INST_POINTS];
    uint16_t p_addr[INST_POINTS];
    float    p_gain[INST_POINTS], p_offset[INST_POINTS], p_lo[INST_POINTS], p_hi[INST_POINTS];
} cfg_t;

/* what the poller publishes */
typedef struct {
    float    value[INST_POINTS];
    uint32_t raw[INST_POINTS];
    uint8_t  q[INST_POINTS];
    uint8_t  state[INST_MAX], last_err[INST_MAX], last_exc[INST_MAX];
    uint32_t good[INST_MAX], errors[INST_MAX];
    uint16_t resp_ms[INST_MAX];
} res_t;

/* scan <-> poller, under PLAT_CRIT */
static struct {
    cfg_t    cfg;
    uint32_t gen;               /* +1 when the settings change */
    bool     valid;
    res_t    res;
    /* explorer */
    bool     ex_pending;
    uint32_t ex_ip;
    uint16_t ex_port, ex_addr, ex_count;
    uint8_t  ex_unit, ex_fc;
    inst_ex_view_t ex;
} sh;

/* ---------------- the poller (iobus task) ---------------- */
typedef struct { uint8_t inst, fc; uint16_t start, count; } req_t;

enum { PST_NONE = 0, PST_OK, PST_STALE, PST_EXC };

typedef struct {
    bool     open;
    uint32_t ip;
    uint16_t port;
    uint32_t t0;                 /* connection started */
    uint16_t to_ms;              /* its connect timeout */
    bool     busy;               /* a request is out */
    int8_t   owner;              /* instrument whose request it is */
    uint8_t  req;
    uint16_t tid;
    uint32_t sent;
    uint8_t  rx[MBAP_MAX];       /* one whole frame; the rest waits in the chip */
    uint16_t have;
    uint32_t used;
} conn_t;

typedef struct {
    uint8_t  first, n;           /* its requests in R[] */
    bool     run;                /* enabled, with points and good settings */
    bool     bad;
    bool     polling;
    uint8_t  cur;                /* next request of this poll */
    bool     comm_ok;            /* this poll so far */
    uint32_t t0, next;
    int8_t   conn;
    uint8_t  fails;
} run_t;

static inst_net_t *net;
static cfg_t   C;
static uint32_t gen_seen;
static bool    planned;
static req_t   R[INST_POINTS];
static uint8_t nreq;
static int8_t  pt_req[INST_POINTS];
static run_t   run[INST_MAX];
static conn_t  conn[INST_CONN_MAX + 1];
static uint8_t pst[INST_POINTS];
static res_t   res;
static uint16_t tid;
static uint32_t use;
static unsigned rr;              /* round robin start */
static struct {                  /* explorer in progress */
    bool active, sent;
    uint32_t t0;
    uint16_t tid;
} ex;

static bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

uint16_t inst_type_regs(uint8_t type) { return type == INST_T_U32 || type == INST_T_I32 || type == INST_T_F32 ? 2u : 1u; }

float inst_decode(uint8_t type, uint8_t order, uint8_t bit, const uint16_t *w, uint32_t *raw) {
    uint8_t a = (uint8_t)(w[0] >> 8), b = (uint8_t)w[0];
    uint32_t v;
    if (inst_type_regs(type) == 1u) {
        v = order == INST_O_BADC || order == INST_O_DCBA ? (uint32_t)b << 8 | a : (uint32_t)a << 8 | b;
    } else {
        uint8_t c = (uint8_t)(w[1] >> 8), d = (uint8_t)w[1], m[4];
        switch (order) {
        case INST_O_CDAB: m[0] = c; m[1] = d; m[2] = a; m[3] = b; break;
        case INST_O_BADC: m[0] = b; m[1] = a; m[2] = d; m[3] = c; break;
        case INST_O_DCBA: m[0] = d; m[1] = c; m[2] = b; m[3] = a; break;
        default:          m[0] = a; m[1] = b; m[2] = c; m[3] = d; break;
        }
        v = (uint32_t)m[0] << 24 | (uint32_t)m[1] << 16 | (uint32_t)m[2] << 8 | m[3];
    }
    if (raw) *raw = v;
    switch (type) {
    case INST_T_I16: return (float)(int16_t)(uint16_t)v;
    case INST_T_U32: return (float)v;
    case INST_T_I32: return (float)(int32_t)v;
    case INST_T_F32: { float f; memcpy(&f, &v, sizeof f); return f; }
    case INST_T_BIT: return (float)((v >> (bit & 15u)) & 1u);
    default:         return (float)v;
    }
}

static bool is_bits(uint8_t fc) { return fc == INST_FC_COILS || fc == INST_FC_DISCRETE; }
static uint16_t span_of(unsigned p) { return is_bits(C.p_fc[p]) ? 1u : inst_type_regs(C.p_type[p]); }

static void close_conn(unsigned c) {
    if (conn[c].open && net) net->close(net, c);
    conn[c].open = false; conn[c].busy = false; conn[c].have = 0; conn[c].owner = -1;
    for (unsigned k = 0; k < INST_MAX; ++k) if (run[k].conn == (int8_t)c) run[k].conn = -1;
}

/* the requests for every instrument, from the settings */
static void plan(uint32_t now) {
    for (unsigned c = 0; c < INST_CONN_MAX; ++c) close_conn(c);
    memset(run, 0, sizeof run);
    memset(pst, 0, sizeof pst);
    memset(&res, 0, sizeof res);
    nreq = 0;
    for (unsigned p = 0; p < INST_POINTS; ++p) { pt_req[p] = -1; res.value[p] = NAN; }
    for (unsigned k = 0; k < INST_MAX; ++k) {
        run_t *r = &run[k];
        r->conn = -1;
        r->first = nreq;
        if (!C.enable[k]) continue;
        uint8_t idx[INST_POINTS];
        unsigned n = 0;
        for (unsigned p = 0; p < INST_POINTS; ++p) {
            if (C.p_inst[p] != k + 1u) continue;
            unsigned j = n++;            /* insertion sort by function, then address */
            while (j > 0 && (C.p_fc[idx[j - 1]] > C.p_fc[p] ||
                             (C.p_fc[idx[j - 1]] == C.p_fc[p] && C.p_addr[idx[j - 1]] > C.p_addr[p]))) { idx[j] = idx[j - 1]; --j; }
            idx[j] = (uint8_t)p;
        }
        if (!n) continue;
        if (!C.ip[k]) { r->bad = true; continue; }
        for (unsigned i = 0; i < n && !r->bad; ++i) {
            unsigned p = idx[i];
            uint8_t fc = C.p_fc[p];
            uint32_t maxc = is_bits(fc) ? (uint32_t)C.max_regs[k] * 16u : C.max_regs[k];
            if (maxc > 2000u) maxc = 2000u;
            uint32_t a = C.p_addr[p], e = a + span_of(p);
            if (e - a > maxc || e > 65536u) { r->bad = true; break; }
            req_t *q = nreq > r->first ? &R[nreq - 1] : NULL;
            uint32_t qe = q ? (uint32_t)q->start + q->count : 0;
            if (q && q->fc == fc && a <= qe + C.gap[k] && (e > qe ? e : qe) - q->start <= maxc) {
                if (e > qe) q->count = (uint16_t)(e - q->start);
            } else {
                R[nreq] = (req_t){ .inst = (uint8_t)k, .fc = fc, .start = (uint16_t)a, .count = (uint16_t)(e - a) };
                nreq++;
            }
            pt_req[p] = (int8_t)(nreq - 1u);
        }
        if (r->bad) {
            for (unsigned p = 0; p < INST_POINTS; ++p) if (pt_req[p] >= (int)r->first) pt_req[p] = -1;
            nreq = r->first;
            continue;
        }
        r->n = (uint8_t)(nreq - r->first);
        r->run = true;
        r->next = now;
    }
    planned = true;
}

/* ---- the result of one request ---- */
static void end_poll(unsigned k, uint32_t now) {
    run_t *r = &run[k];
    r->polling = false;
    if (r->comm_ok) {
        r->fails = 0;
        res.good[k]++;
        res.resp_ms[k] = (uint16_t)((now - r->t0) > 65535u ? 65535u : now - r->t0);
        r->next = r->t0 + C.period_ms[k];
        if (due(now, r->next)) r->next = now;
    } else {
        if (r->fails < 255u) r->fails++;
        uint32_t wait = C.period_ms[k];
        if (r->fails >= INST_LOST_FAILS && wait < INST_RETRY_MS) wait = INST_RETRY_MS;
        r->next = now + wait;
    }
}

static void mark_req(unsigned q, uint8_t st) {
    for (unsigned p = 0; p < INST_POINTS; ++p)
        if (pt_req[p] == (int8_t)q && !(st == PST_STALE && pst[p] == PST_NONE)) pst[p] = st;
}

/* a request failed: a communication failure ends the poll, an exception only fails its points */
static void req_fail(unsigned k, uint8_t err, uint8_t exc, uint32_t now) {
    run_t *r = &run[k];
    res.errors[k]++;
    res.last_err[k] = err;
    if (err == INST_E_EXCEPTION) res.last_exc[k] = exc;
    bool comm = err != INST_E_EXCEPTION || exc == 0x0Au || exc == 0x0Bu;
    if (comm) {
        for (unsigned q = r->first + r->cur; q < (unsigned)r->first + r->n; ++q) mark_req(q, PST_STALE);
        r->comm_ok = false;
        end_poll(k, now);
        return;
    }
    mark_req(r->first + r->cur, PST_EXC);
    if (++r->cur >= r->n) end_poll(k, now);
}

static void req_ok(unsigned k, const uint8_t *pdu, uint32_t now) {
    run_t *r = &run[k];
    unsigned q = r->first + r->cur;
    const req_t *rq = &R[q];
    const uint8_t *d = pdu + 2;                      /* after function and byte count */
    for (unsigned p = 0; p < INST_POINTS; ++p) {
        if (pt_req[p] != (int8_t)q) continue;
        unsigned off = (unsigned)(C.p_addr[p] - rq->start);
        uint32_t raw;
        float v;
        if (is_bits(rq->fc)) {
            raw = (uint32_t)(d[off / 8u] >> (off % 8u)) & 1u;
            v = (float)raw;
        } else {
            uint16_t w[2] = { (uint16_t)(d[2 * off] << 8 | d[2 * off + 1]), 0 };
            if (inst_type_regs(C.p_type[p]) == 2u) w[1] = (uint16_t)(d[2 * off + 2] << 8 | d[2 * off + 3]);
            v = inst_decode(C.p_type[p], C.p_order[p], C.p_bit[p], w, &raw);
            v = v * C.p_gain[p] + C.p_offset[p];
        }
        res.value[p] = v;
        res.raw[p] = raw;
        pst[p] = PST_OK;
    }
    if (++r->cur >= r->n) end_poll(k, now);
}

/* A whole MBAP frame for request (fc, count) with transaction id t: 0 = fine and *pdu points at
 * the PDU; else INST_E_EXCEPTION (with *exc) or INST_E_REPLY. -1: not ours (an old reply). */
static int check_reply(const uint8_t *f, unsigned n, uint16_t t, uint8_t fc, uint16_t count, const uint8_t **pdu, uint8_t *exc) {
    if ((uint16_t)(f[0] << 8 | f[1]) != t) return -1;
    const uint8_t *p = f + 7;
    unsigned pl = n - 7u;
    if (pl >= 2u && p[0] == (fc | 0x80u)) { *exc = p[1]; return INST_E_EXCEPTION; }
    unsigned want = is_bits(fc) ? (count + 7u) / 8u : 2u * count;
    if (pl < 2u || p[0] != fc || p[1] != want || pl != 2u + want) return INST_E_REPLY;
    *pdu = p;
    return 0;
}

/* complete frame length at the start of the buffer: 0 = not yet, -1 = not Modbus TCP */
static int frame_len(const uint8_t *b, unsigned n) {
    if (n < 7u) return 0;
    unsigned len = (unsigned)(b[4] << 8 | b[5]);
    if (b[2] || b[3] || len < 2u || len > 254u) return -1;
    return n >= 6u + len ? (int)(6u + len) : 0;
}

static unsigned build(uint8_t *f, uint16_t t, uint8_t unit, uint8_t fc, uint16_t addr, uint16_t count) {
    f[0] = (uint8_t)(t >> 8); f[1] = (uint8_t)t; f[2] = 0; f[3] = 0; f[4] = 0; f[5] = 6;
    f[6] = unit; f[7] = fc;
    f[8] = (uint8_t)(addr >> 8); f[9] = (uint8_t)addr; f[10] = (uint8_t)(count >> 8); f[11] = (uint8_t)count;
    return 12u;
}

/* read what has arrived on connection c; returns a complete frame's length (0: none yet,
 * -1: the stream is broken) with the frame at the start of rx */
static int take_frame(unsigned c) {
    conn_t *cn = &conn[c];
    uint16_t got = net->recv(net, c, cn->rx + cn->have, (uint16_t)(sizeof cn->rx - cn->have));
    cn->have = (uint16_t)(cn->have + got);
    return frame_len(cn->rx, cn->have);
}
static void consume(unsigned c, unsigned n) {
    conn_t *cn = &conn[c];
    memmove(cn->rx, cn->rx + n, cn->have - n);
    cn->have = (uint16_t)(cn->have - n);
}

static void service_conn(unsigned c, uint32_t now) {
    conn_t *cn = &conn[c];
    if (!cn->open) return;
    uint8_t st = net->state(net, c);
    if (st == INST_L_CONNECTING) {
        if (due(now, cn->t0 + cn->to_ms)) {
            for (unsigned k = 0; k < INST_MAX; ++k) if (run[k].polling && run[k].conn == (int8_t)c) req_fail(k, INST_E_CONNECT, 0, now);
            close_conn(c);
        }
        return;
    }
    if (st != INST_L_UP) {
        for (unsigned k = 0; k < INST_MAX; ++k)
            if (run[k].polling && run[k].conn == (int8_t)c)
                req_fail(k, cn->busy && cn->owner == (int8_t)k ? INST_E_CLOSED : INST_E_CONNECT, 0, now);
        close_conn(c);
        return;
    }
    if (!cn->busy) return;
    unsigned k = (unsigned)cn->owner;
    const req_t *rq = &R[run[k].first + cn->req];
    int fl;
    while ((fl = take_frame(c)) > 0) {
        const uint8_t *pdu = NULL;
        uint8_t exc = 0;
        int e = check_reply(cn->rx, (unsigned)fl, cn->tid, rq->fc, rq->count, &pdu, &exc);
        if (e < 0) { consume(c, (unsigned)fl); continue; }      /* an old reply */
        cn->busy = false;
        if (e == 0) req_ok(k, pdu, now); else req_fail(k, (uint8_t)e, exc, now);
        consume(c, (unsigned)fl);
        return;
    }
    if (fl < 0) {                                   /* not Modbus TCP: start again */
        cn->busy = false;
        req_fail(k, INST_E_REPLY, 0, now);
        close_conn(c);
        return;
    }
    if (due(now, cn->sent + C.timeout_ms[k])) {     /* the connection stays: a late reply is dropped by its id */
        cn->busy = false;
        req_fail(k, INST_E_TIMEOUT, 0, now);
    }
}

/* the connection for instrument k: its endpoint's, a free one, or the least recently used idle
 * one (taken over). -1: none free now; -2: the connection could not be started */
static int conn_for(unsigned k, uint32_t now) {
    int pick = -1;
    for (unsigned c = 0; c < INST_CONN_MAX; ++c)
        if (conn[c].open && conn[c].ip == C.ip[k] && conn[c].port == C.port[k]) return (int)c;
    for (unsigned c = 0; c < INST_CONN_MAX && pick < 0; ++c) if (!conn[c].open) pick = (int)c;
    if (pick < 0) {
        for (unsigned c = 0; c < INST_CONN_MAX; ++c) {
            if (conn[c].busy) continue;
            bool held = false;
            for (unsigned j = 0; j < INST_MAX; ++j) if (run[j].polling && run[j].conn == (int8_t)c) held = true;
            if (held) continue;
            if (pick < 0 || conn[c].used < conn[pick].used) pick = (int)c;
        }
        if (pick < 0) return -1;
        close_conn((unsigned)pick);
    }
    conn_t *cn = &conn[pick];
    if (!net->connect(net, (unsigned)pick, C.ip[k], C.port[k])) return -2;
    cn->open = true; cn->ip = C.ip[k]; cn->port = C.port[k];
    cn->t0 = now; cn->to_ms = C.timeout_ms[k]; cn->busy = false; cn->have = 0; cn->owner = -1;
    cn->used = ++use;
    return pick;
}

static void poll_inst(unsigned k, uint32_t now) {
    run_t *r = &run[k];
    if (!r->polling) {
        if (!due(now, r->next)) return;
        r->polling = true; r->cur = 0; r->comm_ok = true; r->t0 = now;
    }
    if (r->conn < 0) {
        int c = conn_for(k, now);
        if (c == -1) return;                         /* every connection busy: next cycle */
        if (c == -2) { req_fail(k, INST_E_CONNECT, 0, now); return; }
        r->conn = (int8_t)c;
    }
    conn_t *cn = &conn[r->conn];
    if (cn->busy || net->state(net, (unsigned)r->conn) != INST_L_UP) return;
    const req_t *rq = &R[r->first + r->cur];
    uint8_t f[12];
    tid = (uint16_t)(tid + 1u);
    unsigned n = build(f, tid, C.unit[k], rq->fc, rq->start, rq->count);
    cn->have = 0;                                     /* anything left over is old */
    (void)net->recv(net, (unsigned)r->conn, cn->rx, sizeof cn->rx);
    if (!net->send(net, (unsigned)r->conn, f, (uint16_t)n)) {
        req_fail(k, INST_E_CLOSED, 0, now);
        close_conn((unsigned)r->conn);
        return;
    }
    cn->busy = true; cn->owner = (int8_t)k; cn->req = r->cur; cn->tid = tid; cn->sent = now; cn->used = ++use;
}

/* ---- explorer ---- */
static void ex_finish(uint8_t result, uint8_t exc, const uint8_t *data, unsigned n, uint32_t now) {
    close_conn(EX_CONN);
    PLAT_CRIT_ENTER();
    sh.ex.result = result;
    sh.ex.exception = exc;
    sh.ex.ms = (uint16_t)((now - ex.t0) > 65535u ? 65535u : now - ex.t0);
    sh.ex.nbytes = (uint16_t)(data ? n : 0u);
    if (data && n <= sizeof sh.ex.data) memcpy(sh.ex.data, data, n);
    sh.ex.seq++;
    PLAT_CRIT_EXIT();
    ex.active = false;
}

static void ex_cycle(uint32_t now) {
    if (!ex.active) {
        bool go;
        PLAT_CRIT_ENTER(); go = sh.ex_pending; sh.ex_pending = false; PLAT_CRIT_EXIT();
        if (!go) return;
        ex.active = true; ex.sent = false; ex.t0 = now;
        if (!net || !net->ready(net)) { ex_finish(INST_EX_NO_PORT, 0, NULL, 0, now); return; }
        close_conn(EX_CONN);
        if (!net->connect(net, EX_CONN, sh.ex_ip, sh.ex_port)) { ex_finish(INST_EX_CONNECT, 0, NULL, 0, now); return; }
        conn[EX_CONN].open = true; conn[EX_CONN].have = 0;
    }
    uint8_t st = net->state(net, EX_CONN);
    if (st == INST_L_CLOSED) { ex_finish(ex.sent ? INST_EX_REPLY : INST_EX_CONNECT, 0, NULL, 0, now); return; }
    if (due(now, ex.t0 + EX_TIMEOUT_MS)) { ex_finish(ex.sent ? INST_EX_TIMEOUT : INST_EX_CONNECT, 0, NULL, 0, now); return; }
    if (st != INST_L_UP) return;
    if (!ex.sent) {
        uint8_t f[12];
        tid = (uint16_t)(tid + 1u);
        ex.tid = tid;
        unsigned n = build(f, tid, sh.ex_unit, sh.ex_fc, sh.ex_addr, sh.ex_count);
        if (!net->send(net, EX_CONN, f, (uint16_t)n)) { ex_finish(INST_EX_CONNECT, 0, NULL, 0, now); return; }
        ex.sent = true;
        return;
    }
    int fl;
    while ((fl = take_frame(EX_CONN)) > 0) {
        const uint8_t *pdu = NULL;
        uint8_t exc = 0;
        int e = check_reply(conn[EX_CONN].rx, (unsigned)fl, ex.tid, sh.ex_fc, sh.ex_count, &pdu, &exc);
        if (e < 0) { consume(EX_CONN, (unsigned)fl); continue; }
        if (e == 0) ex_finish(INST_EX_OK, 0, pdu + 2, pdu[1], now);
        else ex_finish(e == INST_E_EXCEPTION ? INST_EX_EXCEPTION : INST_EX_REPLY, exc, NULL, 0, now);
        return;
    }
    if (fl < 0) ex_finish(INST_EX_REPLY, 0, NULL, 0, now);
}

/* ---- quality ---- */
static uint8_t comm_q(unsigned p) {
    switch (pst[p]) {
    case PST_OK:    return PLAT_Q_VALID;
    case PST_STALE: return PLAT_Q_STALE;
    case PST_EXC:   return PLAT_Q_FAULT;
    default:        return PLAT_Q_UNAVAILABLE;
    }
}

static uint8_t point_q(unsigned p) {
    unsigned k1 = C.p_inst[p];
    if (k1 < 1u || k1 > INST_MAX || pt_req[p] < 0 || !run[k1 - 1u].run) return PLAT_Q_UNAVAILABLE;
    unsigned k = k1 - 1u;
    if (run[k].fails >= INST_LOST_FAILS) return PLAT_Q_UNAVAILABLE;
    uint8_t q = comm_q(p);
    if (q != PLAT_Q_VALID) return q;
    unsigned sp = C.status_pt[k];
    if (sp >= 1u && sp <= INST_POINTS && sp - 1u != p) {
        unsigned s = sp - 1u;
        if (C.p_inst[s] != k1 || pt_req[s] < 0) return PLAT_Q_FAULT;     /* the status point is set up wrong */
        uint8_t sq = comm_q(s);
        if (sq != PLAT_Q_VALID) return sq;
        uint32_t bits = res.raw[s];
        if (bits & C.fault_mask[k]) return PLAT_Q_FAULT;
        if (bits & C.cal_mask[k])   return PLAT_Q_CAL;
        if (bits & C.maint_mask[k]) return PLAT_Q_MAINT;
    }
    float v = res.value[p];
    if (!isfinite(v)) return PLAT_Q_RANGE;
    if (C.p_lo[p] < C.p_hi[p] && (v < C.p_lo[p] || v > C.p_hi[p])) return PLAT_Q_RANGE;
    return PLAT_Q_VALID;
}

static uint8_t inst_state(unsigned k) {
    const run_t *r = &run[k];
    if (r->bad) return INST_S_BAD_SETTINGS;
    if (!r->run) return INST_S_OFF;
    if (r->fails >= INST_LOST_FAILS) return INST_S_LOST;
    if (r->fails) return INST_S_FAILING;
    return res.good[k] ? INST_S_OK : INST_S_CONNECTING;
}

void inst_init(inst_net_t *n) { net = n; }

void inst_cycle(uint32_t now) {
    bool valid;
    uint32_t gen;
    PLAT_CRIT_ENTER();
    valid = sh.valid;
    gen = sh.gen;
    if (valid && (!planned || gen != gen_seen)) memcpy(&C, &sh.cfg, sizeof C);
    PLAT_CRIT_EXIT();
    if (!valid) return;
    if (!planned || gen != gen_seen) { gen_seen = gen; plan(now); }

    bool work = false;               /* the I/O port is not touched while no instrument is set up */
    for (unsigned k = 0; k < INST_MAX; ++k) work = work || run[k].run;
    if (!work) {
        for (unsigned c = 0; c < INST_CONN_MAX; ++c) if (conn[c].open) close_conn(c);
    } else if (!net || !net->ready(net)) {
        for (unsigned c = 0; c <= INST_CONN_MAX; ++c) { conn[c].open = false; conn[c].busy = false; }
        for (unsigned k = 0; k < INST_MAX; ++k) {
            run[k].conn = -1;
            if (!run[k].run || !due(now, run[k].next) || run[k].polling) continue;
            run[k].polling = true; run[k].cur = 0; run[k].comm_ok = true; run[k].t0 = now;
            req_fail(k, INST_E_NO_PORT, 0, now);
        }
    } else {
        for (unsigned c = 0; c < INST_CONN_MAX; ++c) service_conn(c, now);
        for (unsigned i = 0; i < INST_MAX; ++i) {
            unsigned k = (rr + i) % INST_MAX;
            if (run[k].run) poll_inst(k, now);
        }
        rr = (rr + 1u) % INST_MAX;
    }
    ex_cycle(now);

    for (unsigned p = 0; p < INST_POINTS; ++p) {
        res.q[p] = point_q(p);
        if (C.p_inst[p] == 0u || pt_req[p] < 0) res.value[p] = NAN;
    }
    for (unsigned k = 0; k < INST_MAX; ++k) res.state[k] = inst_state(k);
    PLAT_CRIT_ENTER();
    memcpy(&sh.res, &res, sizeof res);
    PLAT_CRIT_EXIT();
}

/* ---------------- scan side ---------------- */
static uint8_t clamp8(uint8_t v, uint8_t lo, uint8_t hi, uint8_t dflt) { return v < lo || v > hi ? dflt : v; }

void inst_scan(uint32_t now_ms) {
    (void)now_ms;
    static cfg_t c;          /* static: 1 KB off the scan task's stack */
    const inst_tags_t *t = &inst_tags;
    const inst_pt_tags_t *pt = &inst_pt;
    memset(&c, 0, sizeof c);
    for (unsigned k = 0; k < INST_MAX; ++k) {
        c.enable[k] = t->enable[k];
        c.ip[k] = t->ip[k];
        c.port[k] = t->port[k] ? t->port[k] : 502u;
        c.unit[k] = t->unit[k];
        c.period_ms[k] = t->period_ms[k] >= 100u ? t->period_ms[k] : 1000u;
        c.timeout_ms[k] = t->timeout_ms[k] >= 50u ? t->timeout_ms[k] : 1000u;
        c.max_regs[k] = clamp8(t->max_regs[k], 1u, 125u, 64u);
        c.gap[k] = t->gap[k] <= 32u ? t->gap[k] : 0u;
        c.status_pt[k] = t->status_pt[k] <= INST_POINTS ? t->status_pt[k] : 0u;
        c.fault_mask[k] = t->fault_mask[k]; c.maint_mask[k] = t->maint_mask[k]; c.cal_mask[k] = t->cal_mask[k];
    }
    for (unsigned p = 0; p < INST_POINTS; ++p) {
        c.p_inst[p] = pt->inst[p] <= INST_MAX ? pt->inst[p] : 0u;
        c.p_fc[p] = clamp8(pt->fc[p], INST_FC_COILS, INST_FC_INPUT, INST_FC_HOLDING);
        c.p_addr[p] = pt->addr[p];
        c.p_type[p] = pt->type[p] < INST_T_COUNT ? pt->type[p] : INST_T_U16;
        c.p_order[p] = pt->order[p] < INST_O_COUNT ? pt->order[p] : INST_O_ABCD;
        c.p_bit[p] = pt->bit[p] & 15u;
        c.p_gain[p] = isfinite(pt->gain[p]) ? pt->gain[p] : 1.0f;
        c.p_offset[p] = isfinite(pt->offset[p]) ? pt->offset[p] : 0.0f;
        c.p_lo[p] = pt->lo[p]; c.p_hi[p] = pt->hi[p];
    }
    PLAT_CRIT_ENTER();
    if (!sh.valid || memcmp(&c, &sh.cfg, sizeof c) != 0) { memcpy(&sh.cfg, &c, sizeof c); sh.gen++; }
    bool had = sh.valid;
    sh.valid = true;
    const res_t *r = &sh.res;
    for (unsigned p = 0; p < INST_POINTS; ++p) {
        inst_pt.value[p] = had ? r->value[p] : NAN;          /* nothing polled yet: unavailable */
        inst_pt.q[p] = had ? r->q[p] : (uint8_t)PLAT_Q_UNAVAILABLE;
        inst_pt.raw[p] = had ? r->raw[p] : 0u;
    }
    for (unsigned k = 0; k < INST_MAX; ++k) {
        inst_tags.state[k] = r->state[k]; inst_tags.good[k] = r->good[k]; inst_tags.errors[k] = r->errors[k];
        inst_tags.last_err[k] = r->last_err[k]; inst_tags.last_exc[k] = r->last_exc[k]; inst_tags.resp_ms[k] = r->resp_ms[k];
    }
    PLAT_CRIT_EXIT();
}

bool inst_point(unsigned p, float *value, uint8_t *q) {
    if (p >= INST_POINTS) return false;
    PLAT_CRIT_ENTER();
    if (value) *value = inst_pt.value[p];
    if (q) *q = inst_pt.q[p];
    PLAT_CRIT_EXIT();
    return true;
}

/* The emissions engine's source hook (emis_bind.h, PR #15: parameter 430 = 200 + p). It overrides
 * the engine's weak stub; scan task. */
bool emis_inst_point(unsigned p, float *value, uint8_t *q);
bool emis_inst_point(unsigned p, float *value, uint8_t *q) { return inst_point(p, value, q); }

/* ---------------- explorer (any task) ---------------- */
bool inst_ex_start(uint32_t ip, uint16_t port, uint8_t unit, uint8_t fc, uint16_t addr, uint16_t count) {
    if (fc < INST_FC_COILS || fc > INST_FC_INPUT || ip == 0 || port == 0 || count == 0) return false;
    if (count > (is_bits(fc) ? 2000u : INST_EX_MAX) || (uint32_t)addr + count > 65536u) return false;
    bool ok = false;
    PLAT_CRIT_ENTER();
    if (!sh.ex_pending && sh.ex.result != INST_EX_BUSY) {
        sh.ex_pending = true;
        sh.ex_ip = ip; sh.ex_port = port; sh.ex_unit = unit; sh.ex_fc = fc; sh.ex_addr = addr; sh.ex_count = count;
        sh.ex.result = INST_EX_BUSY; sh.ex.exception = 0; sh.ex.nbytes = 0;
        sh.ex.ip = ip; sh.ex.port = port; sh.ex.unit = unit; sh.ex.fc = fc; sh.ex.addr = addr; sh.ex.count = count;
        ok = true;
    }
    PLAT_CRIT_EXIT();
    return ok;
}

void inst_ex_view(inst_ex_view_t *v) {
    PLAT_CRIT_ENTER();
    *v = sh.ex;
    PLAT_CRIT_EXIT();
}

/* ---------------- names ---------------- */
const char *inst_state_name(uint8_t s) {
    static const char *const n[] = { "off", "connecting", "ok", "failing", "lost", "bad settings" };
    return s < sizeof n / sizeof n[0] ? n[s] : "?";
}
const char *inst_err_name(uint8_t e) {
    static const char *const n[] = { "", "no connection", "no reply", "exception", "bad reply", "closed", "no I/O port" };
    return e < sizeof n / sizeof n[0] ? n[e] : "?";
}
const char *inst_type_name(uint8_t t) {
    static const char *const n[] = { "u16", "i16", "u32", "i32", "f32", "bit" };
    return t < sizeof n / sizeof n[0] ? n[t] : "?";
}
const char *inst_order_name(uint8_t o) {
    static const char *const n[] = { "ABCD", "CDAB", "BADC", "DCBA" };
    return o < sizeof n / sizeof n[0] ? n[o] : "?";
}
const char *inst_ex_result_name(uint8_t r) {
    static const char *const n[] = { "idle", "busy", "ok", "no I/O port", "no connection", "no reply", "exception", "bad reply" };
    return r < sizeof n / sizeof n[0] ? n[r] : "?";
}

void inst_reset(void) {
    for (unsigned c = 0; c <= INST_CONN_MAX; ++c) close_conn(c);
    memset(&sh, 0, sizeof sh);
    memset(&C, 0, sizeof C);
    memset(run, 0, sizeof run);
    memset(&ex, 0, sizeof ex);
    memset(pst, 0, sizeof pst);
    memset(&res, 0, sizeof res);
    planned = false; gen_seen = 0; nreq = 0; rr = 0;
}
