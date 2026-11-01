/* emis_qal3.c - see emis_qal3.h. No parameters, no HAL: emis_bind.c feeds it. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "emis_qal3.h"

emis_q3_res_t emis_q3;
#define Q emis_q3

typedef struct {                 /* a channel's chart */
    float  s, zr, sr;            /* the settings it started with */
    int8_t z_run, s_run;         /* consecutive beyond the same action limit: + above, - below */
    int8_t z_side, s_side;       /* consecutive on the same side of the reference */
} chart_t;
static chart_t K[EMIS_CH_MAX];

#define OUT_BITS (EMIS_Q3_ZERO_OUT | EMIS_Q3_SPAN_OUT)

static void start(unsigned i, const emis_q3_set_t *s) {
    K[i] = (chart_t){ .s = s ? s->s_ams[i] : 0.0f, .zr = s ? s->zero_ref[i] : 0.0f, .sr = s ? s->span_ref[i] : 0.0f };
    Q.zero_dev[i] = Q.span_dev[i] = 0.0f;
    Q.last[i] = 0u; Q.n[i] = 0u;
    Q.status[i] = K[i].s > 0.0f ? (uint8_t)(EMIS_Q3_ON | EMIS_Q3_DUE) : 0u;
}

void emis_qal3_init(void) {
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) start(i, NULL);
}

/* the chart follows the settings: a new s_AMS or reference starts it again */
static void sync(const emis_q3_set_t *s, unsigned i) {
    if (s->s_ams[i] != K[i].s || s->zero_ref[i] != K[i].zr || s->span_ref[i] != K[i].sr) start(i, s);
}

static int8_t step_run(int8_t r, int dir) {      /* dir +1, -1 or 0 */
    if (dir == 0) return 0;
    if (dir > 0) return (int8_t)(r > 0 ? (r < 127 ? r + 1 : r) : 1);
    return (int8_t)(r < 0 ? (r > -127 ? r - 1 : r) : -1);
}

/* one point on one chart; true = out of control */
static bool point(float dev, float lim, int8_t *run, int8_t *side, bool *warn) {
    int beyond = dev > lim ? 1 : dev < -lim ? -1 : 0;
    *run = step_run(*run, beyond);
    *side = step_run(*side, dev > 0.0f ? 1 : dev < 0.0f ? -1 : 0);
    *warn = beyond != 0;
    return fabsf(dev) > EMIS_Q3_HARD * lim || abs(*run) >= (int)EMIS_Q3_RUN_OUT || abs(*side) >= (int)EMIS_Q3_RUN_SIDE;
}

int emis_qal3_record(const emis_q3_set_t *s, unsigned ch, float zero, float span, uint32_t clock_s) {
    if (ch >= EMIS_CH_MAX) return -1;
    sync(s, ch);
    if (!(K[ch].s > 0.0f)) return -2;
    if (!isfinite(zero) || !isfinite(span)) return -3;
    if (clock_s == 0u) return -4;
    float lim = s->k * K[ch].s;
    chart_t *c = &K[ch];
    bool zw, sw;
    bool zo = point(zero - c->zr, lim, &c->z_run, &c->z_side, &zw);
    bool so = point(span - c->sr, lim, &c->s_run, &c->s_side, &sw);
    Q.zero_dev[ch] = zero - c->zr; Q.span_dev[ch] = span - c->sr;
    Q.last[ch] = clock_s;
    if (Q.n[ch] < 255u) Q.n[ch]++;
    uint8_t st = (uint8_t)((Q.status[ch] & OUT_BITS) | EMIS_Q3_ON);   /* a check now: not due */
    if (zw) st |= EMIS_Q3_ZERO_WARN;
    if (sw) st |= EMIS_Q3_SPAN_WARN;
    if (zo) st |= EMIS_Q3_ZERO_OUT;
    if (so) st |= EMIS_Q3_SPAN_OUT;
    Q.status[ch] = st;
    return 0;
}

void emis_qal3_reset(unsigned ch) {
    if (ch >= EMIS_CH_MAX) return;
    chart_t *c = &K[ch];
    c->z_run = c->s_run = c->z_side = c->s_side = 0;
    Q.n[ch] = 0u;
    Q.status[ch] &= (uint8_t)~(OUT_BITS | EMIS_Q3_ZERO_WARN | EMIS_Q3_SPAN_WARN);
}

void emis_qal3_tick(const emis_q3_set_t *s, uint32_t clock_s) {
    uint32_t due_s = (uint32_t)s->interval_d * 86400u;
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        sync(s, i);
        if (!(K[i].s > 0.0f)) { Q.status[i] = 0u; continue; }
        if (Q.last[i] == 0u || (clock_s != 0u && (clock_s < Q.last[i] || clock_s - Q.last[i] > due_s)))
            Q.status[i] |= EMIS_Q3_DUE;
        else if (clock_s != 0u)
            Q.status[i] &= (uint8_t)~EMIS_Q3_DUE;
    }
}

bool emis_qal3_alarm(void) {
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i)
        if (Q.status[i] & (OUT_BITS | EMIS_Q3_DUE)) return true;
    return false;
}

/* ---------------- retained ---------------- */

#define Q3_MAGIC 0x51334301u            /* "Q3C" + layout */
typedef struct {                        /* 32 bytes a channel */
    float    s, zr, sr, zero_dev, span_dev;
    uint32_t last;
    int8_t   z_run, s_run, z_side, s_side;
    uint8_t  status, n, pad[2];
} q3_keep_t;
typedef struct { uint32_t magic, spare; q3_keep_t k[EMIS_CH_MAX]; } q3_save_t;
_Static_assert(sizeof(q3_keep_t) == 32u, "q3_keep_t layout");
_Static_assert(sizeof(q3_save_t) <= EMIS_Q3_SAVE_SIZE, "EMIS_Q3_SAVE_SIZE too small");

void emis_qal3_save(void *buf) {
    q3_save_t *v = buf;
    v->magic = Q3_MAGIC; v->spare = 0u;
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        const chart_t *c = &K[i];
        v->k[i] = (q3_keep_t){ .s = c->s, .zr = c->zr, .sr = c->sr, .zero_dev = Q.zero_dev[i], .span_dev = Q.span_dev[i],
                               .last = Q.last[i], .z_run = c->z_run, .s_run = c->s_run, .z_side = c->z_side,
                               .s_side = c->s_side, .status = Q.status[i], .n = Q.n[i] };
    }
}

bool emis_qal3_load(const void *buf) {
    const q3_save_t *v = buf;
    if (v->magic != Q3_MAGIC) return false;
    for (unsigned i = 0; i < EMIS_CH_MAX; ++i) {
        const q3_keep_t *k = &v->k[i];
        if (!isfinite(k->s) || !isfinite(k->zr) || !isfinite(k->sr)) { start(i, NULL); continue; }
        K[i] = (chart_t){ .s = k->s, .zr = k->zr, .sr = k->sr, .z_run = k->z_run, .s_run = k->s_run,
                          .z_side = k->z_side, .s_side = k->s_side };
        Q.zero_dev[i] = k->zero_dev; Q.span_dev[i] = k->span_dev;
        Q.last[i] = k->last; Q.status[i] = k->status; Q.n[i] = k->n;
    }
    return true;
}
