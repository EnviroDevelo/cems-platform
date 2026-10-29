/* test_inst.c - third-party instruments (inst.h): decoding in every type and byte order, how
 * points are grouped into requests, values, scaling and quality (status bits, range, exceptions,
 * a silent instrument going stale then lost and coming back), instruments behind one gateway
 * sharing a connection, more endpoints than connections, the register explorer, a settings
 * change, and the whole path over the W5500 driver and the simulated chip beside the I/O boards. */
#include <math.h>
#include <string.h>
#include "tst.h"
#include "inst.h"
#include "inst_sim.h"
#include "inst_w5500.h"
#include "iob_w5500.h"
#include "w5500_sim.h"
#include "iob_node_sim.h"
#include "plat_state.h"
#include "param.h"

/* ---------------- a network of simulated instruments, without the chip ---------------- */
#define NSIM 6
static inst_sim_t sims[NSIM];
static unsigned   nsims;
static struct {
    uint8_t     st;              /* INST_L_* */
    bool        pending;         /* connecting: up (or refused) at the next look */
    inst_sim_t *to;
    uint8_t     rx[1024];
    uint16_t    n;
} fc[INST_CONN_MAX + 1];
static bool     port_ok = true;
static unsigned connects, sends;

static inst_sim_t *find_sim(uint32_t ip, uint16_t port) {
    for (unsigned i = 0; i < nsims; ++i) if (sims[i].ip == ip && sims[i].port == port) return &sims[i];
    return NULL;
}
static bool f_ready(inst_net_t *n) { (void)n; return port_ok; }
static bool f_connect(inst_net_t *n, unsigned c, uint32_t ip, uint16_t port) {
    (void)n;
    connects++;
    fc[c].to = find_sim(ip, port);
    fc[c].st = INST_L_CONNECTING; fc[c].pending = true; fc[c].n = 0;
    return true;
}
static uint8_t f_state(inst_net_t *n, unsigned c) {
    (void)n;
    if (fc[c].pending) { fc[c].pending = false; fc[c].st = fc[c].to && !fc[c].to->refuse ? INST_L_UP : INST_L_CLOSED; return INST_L_CONNECTING; }
    return fc[c].st;
}
static bool f_send(inst_net_t *n, unsigned c, const uint8_t *b, uint16_t len) {
    (void)n;
    if (fc[c].st != INST_L_UP) return false;
    sends++;
    uint8_t out[300];
    uint16_t r = inst_sim_serve(fc[c].to, b, len, out, sizeof out);
    if (r && fc[c].n + r <= sizeof fc[c].rx) { memcpy(fc[c].rx + fc[c].n, out, r); fc[c].n = (uint16_t)(fc[c].n + r); }
    return true;
}
static uint16_t f_recv(inst_net_t *n, unsigned c, uint8_t *b, uint16_t cap) {
    (void)n;
    uint16_t k = fc[c].n < cap ? fc[c].n : cap;
    memcpy(b, fc[c].rx, k);
    memmove(fc[c].rx, fc[c].rx + k, fc[c].n - k);
    fc[c].n = (uint16_t)(fc[c].n - k);
    return k;
}
static void f_close(inst_net_t *n, unsigned c) { (void)n; fc[c].st = INST_L_CLOSED; fc[c].pending = false; fc[c].n = 0; }
static inst_net_t fnet = { f_ready, f_connect, f_state, f_send, f_recv, f_close, NULL };

static uint32_t now = 1000;
static void run_ms(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 20) { inst_scan(now); inst_cycle(now); now += 20; }
    inst_scan(now);
}

#define IP(d) (0xC0A80100u | (d))
static void fresh(void) {
    inst_reset();
    memset(&inst_tags, 0, sizeof inst_tags);
    memset(&inst_pt, 0, sizeof inst_pt);
    for (unsigned p = 0; p < INST_POINTS; ++p) { inst_pt.gain[p] = 1.0f; inst_pt.fc[p] = INST_FC_HOLDING; }
    for (unsigned k = 0; k < INST_MAX; ++k) { inst_tags.unit[k] = 1; inst_tags.period_ms[k] = 1000; inst_tags.timeout_ms[k] = 200; inst_tags.max_regs[k] = 64; }
    memset(fc, 0, sizeof fc);
    nsims = 0; port_ok = true; connects = 0; sends = 0;
    inst_init(&fnet);
}
static inst_sim_t *add_sim(uint32_t ip, uint8_t unit) { inst_sim_init(&sims[nsims], ip, unit); return &sims[nsims++]; }
static void point(unsigned p, uint8_t inst, uint8_t f, uint16_t addr, uint8_t type, uint8_t order) {
    inst_pt.inst[p] = inst; inst_pt.fc[p] = f; inst_pt.addr[p] = addr; inst_pt.type[p] = type; inst_pt.order[p] = order;
}
static bool near(float a, float b) { return fabsf(a - b) < 1e-4f * (1.0f + fabsf(b)); }

/* ---------------- tests ---------------- */
static void t_decode(void) {
    uint16_t w[2];
    uint32_t raw;
    for (uint8_t o = 0; o < INST_O_COUNT; ++o) {                 /* every order round-trips through the simulator's writer */
        inst_sim_put_f32(w, 0, -123.456f, o);
        CHECK(near(inst_decode(INST_T_F32, o, 0, w, &raw), -123.456f));
        inst_sim_put_u32(w, 0, 0x89ABCDEFu, o);
        CHECK(inst_decode(INST_T_U32, o, 0, w, &raw) == (float)0x89ABCDEFu);
        CHECK_EQ(raw, 0x89ABCDEFu);
        CHECK(inst_decode(INST_T_I32, o, 0, w, &raw) == (float)(int32_t)0x89ABCDEFu);
    }
    /* the wire bytes 12 34 56 78 read four ways */
    w[0] = 0x1234; w[1] = 0x5678;
    inst_decode(INST_T_U32, INST_O_ABCD, 0, w, &raw); CHECK_EQ(raw, 0x12345678u);
    inst_decode(INST_T_U32, INST_O_CDAB, 0, w, &raw); CHECK_EQ(raw, 0x56781234u);
    inst_decode(INST_T_U32, INST_O_BADC, 0, w, &raw); CHECK_EQ(raw, 0x34127856u);
    inst_decode(INST_T_U32, INST_O_DCBA, 0, w, &raw); CHECK_EQ(raw, 0x78563412u);
    /* 16-bit */
    w[0] = 0xFF38;
    CHECK(inst_decode(INST_T_I16, INST_O_ABCD, 0, w, NULL) == -200.0f);
    CHECK(inst_decode(INST_T_U16, INST_O_ABCD, 0, w, NULL) == 65336.0f);
    CHECK(inst_decode(INST_T_U16, INST_O_BADC, 0, w, &raw) == (float)0x38FF); CHECK_EQ(raw, 0x38FFu);
    w[0] = 0x0104;
    CHECK(inst_decode(INST_T_BIT, INST_O_ABCD, 2, w, NULL) == 1.0f);
    CHECK(inst_decode(INST_T_BIT, INST_O_ABCD, 8, w, NULL) == 1.0f);
    CHECK(inst_decode(INST_T_BIT, INST_O_ABCD, 3, w, NULL) == 0.0f);
    CHECK_EQ(inst_type_regs(INST_T_F32), 2); CHECK_EQ(inst_type_regs(INST_T_I16), 1);
    CHECK(!strcmp(inst_order_name(INST_O_CDAB), "CDAB"));
    CHECK(!strcmp(inst_type_name(INST_T_F32), "f32"));
}

static void t_values_and_grouping(void) {
    fresh();
    inst_sim_t *a = add_sim(IP(70), 1);
    inst_sim_put_f32(a->holding, 0, 20.9f, INST_O_CDAB);         /* O2 % */
    inst_sim_put_f32(a->holding, 2, 15.5f, INST_O_CDAB);         /* CO */
    a->holding[10] = (uint16_t)(int16_t)-35;                      /* a temperature x10 */
    a->input[0] = 1234;
    a->coils[0] = 0x05;
    inst_tags.enable[0] = true; inst_tags.ip[0] = IP(70);
    point(0, 1, INST_FC_HOLDING, 0, INST_T_F32, INST_O_CDAB);
    point(1, 1, INST_FC_HOLDING, 2, INST_T_F32, INST_O_CDAB);
    point(2, 1, INST_FC_HOLDING, 10, INST_T_I16, INST_O_ABCD); inst_pt.gain[2] = 0.1f; inst_pt.offset[2] = 1.0f;
    point(3, 1, INST_FC_INPUT, 0, INST_T_U16, INST_O_ABCD);
    point(4, 1, INST_FC_COILS, 2, INST_T_U16, INST_O_ABCD);
    point(5, 1, INST_FC_COILS, 1, INST_T_U16, INST_O_ABCD);
    run_ms(300);
    CHECK_EQ(inst_tags.state[0], INST_S_OK);
    CHECK(near(inst_pt.value[0], 20.9f)); CHECK(near(inst_pt.value[1], 15.5f));
    CHECK(near(inst_pt.value[2], -2.5f));                         /* -35 x 0.1 + 1 */
    CHECK(inst_pt.value[3] == 1234.0f);
    CHECK(inst_pt.value[4] == 1.0f); CHECK(inst_pt.value[5] == 0.0f);
    for (unsigned p = 0; p < 6; ++p) CHECK_EQ(inst_pt.q[p], PLAT_Q_VALID);
    CHECK_EQ(inst_pt.q[6], PLAT_Q_UNAVAILABLE); CHECK(isnan(inst_pt.value[6]));   /* not used */
    CHECK_EQ(a->requests, 4u);   /* holding 0-3, holding 10, input 0, coils 1-2: no hole allowed between 3 and 10 */
    float v; uint8_t q;
    CHECK(inst_point(0, &v, &q)); CHECK(near(v, 20.9f)); CHECK_EQ(q, PLAT_Q_VALID);
    CHECK(!inst_point(INST_POINTS, &v, &q));
    /* a gap of 6 joins holding 0-3 and 10 into one read (a settings change starts again) */
    inst_tags.gap[0] = 6;
    run_ms(300);
    uint32_t before = a->requests, g0 = inst_tags.good[0];
    run_ms(5000);
    CHECK(inst_tags.good[0] - g0 >= 4u);
    CHECK_EQ(a->requests - before, 3u * (inst_tags.good[0] - g0));
    CHECK(near(inst_pt.value[2], -2.5f));
    /* max_regs 3: the floats can no longer share a read */
    inst_tags.max_regs[0] = 3; inst_tags.gap[0] = 0;
    run_ms(300);
    before = a->requests; g0 = inst_tags.good[0];
    run_ms(5000);
    CHECK_EQ(a->requests - before, 5u * (inst_tags.good[0] - g0));
    CHECK(near(inst_pt.value[1], 15.5f));
    /* max_regs 1: a float does not fit at all */
    inst_tags.max_regs[0] = 1;
    run_ms(100);
    CHECK_EQ(inst_tags.state[0], INST_S_BAD_SETTINGS);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_UNAVAILABLE);
    /* every second */
    inst_tags.max_regs[0] = 64;
    run_ms(100);
    before = a->requests;
    run_ms(5000);
    CHECK(a->requests - before >= 4u * 4u && a->requests - before <= 4u * 6u);
    CHECK(inst_tags.good[0] >= 5u);
}

static void t_quality(void) {
    fresh();
    inst_sim_t *a = add_sim(IP(70), 1);
    inst_sim_put_f32(a->holding, 0, 8.0f, INST_O_ABCD);
    inst_tags.enable[0] = true; inst_tags.ip[0] = IP(70);
    point(0, 1, INST_FC_HOLDING, 0, INST_T_F32, INST_O_ABCD); inst_pt.lo[0] = 0.0f; inst_pt.hi[0] = 25.0f;
    point(1, 1, INST_FC_HOLDING, 10, INST_T_U16, INST_O_ABCD);           /* the analyser's status word */
    point(2, 1, INST_FC_HOLDING, 600, INST_T_U16, INST_O_ABCD);          /* not in its map: exception 02 */
    inst_tags.status_pt[0] = 2;
    inst_tags.fault_mask[0] = 0x0001; inst_tags.maint_mask[0] = 0x0002; inst_tags.cal_mask[0] = 0x0004;
    run_ms(100);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_VALID);
    CHECK_EQ(inst_pt.q[1], PLAT_Q_VALID);                                  /* the status point itself */
    CHECK_EQ(inst_pt.q[2], PLAT_Q_FAULT);
    CHECK_EQ(inst_tags.state[0], INST_S_OK);                               /* it answers: an exception is not lost comms */
    CHECK_EQ(inst_tags.last_err[0], INST_E_EXCEPTION); CHECK_EQ(inst_tags.last_exc[0], 2);
    a->holding[10] = 0x0002; run_ms(1000); CHECK_EQ(inst_pt.q[0], PLAT_Q_MAINT);
    a->holding[10] = 0x0006; run_ms(1000); CHECK_EQ(inst_pt.q[0], PLAT_Q_CAL);      /* calibration before maintenance */
    a->holding[10] = 0x0007; run_ms(1000); CHECK_EQ(inst_pt.q[0], PLAT_Q_FAULT);    /* fault first */
    a->holding[10] = 0x0008; run_ms(1000); CHECK_EQ(inst_pt.q[0], PLAT_Q_VALID);    /* bits nobody asked about */
    inst_sim_put_f32(a->holding, 0, 30.0f, INST_O_ABCD); run_ms(1000); CHECK_EQ(inst_pt.q[0], PLAT_Q_RANGE);
    uint16_t nan[2]; float f = NAN; uint32_t u; memcpy(&u, &f, 4);
    nan[0] = (uint16_t)(u >> 16); nan[1] = (uint16_t)u;
    a->holding[0] = nan[0]; a->holding[1] = nan[1]; run_ms(1000); CHECK_EQ(inst_pt.q[0], PLAT_Q_RANGE);
    inst_sim_put_f32(a->holding, 0, 9.0f, INST_O_ABCD); run_ms(1000); CHECK_EQ(inst_pt.q[0], PLAT_Q_VALID);
    /* silent: stale with the last value kept, then lost and unavailable, retried every 5 s */
    a->silent = true;
    run_ms(1500);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_STALE); CHECK(near(inst_pt.value[0], 9.0f));
    CHECK_EQ(inst_tags.state[0], INST_S_FAILING); CHECK_EQ(inst_tags.last_err[0], INST_E_TIMEOUT);
    run_ms(2500);
    CHECK_EQ(inst_tags.state[0], INST_S_LOST);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_UNAVAILABLE);
    uint32_t r0 = a->requests;
    run_ms(4000);
    CHECK(a->requests - r0 <= 1u);                                   /* not every second while lost */
    a->silent = false;
    run_ms(6000);
    CHECK_EQ(inst_tags.state[0], INST_S_OK);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_VALID);
    /* disabled */
    inst_tags.enable[0] = false;
    run_ms(100);
    CHECK_EQ(inst_tags.state[0], INST_S_OFF);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_UNAVAILABLE); CHECK(isnan(inst_pt.value[0]));
    /* a status point that is not this instrument's: a settings fault */
    inst_tags.enable[0] = true; inst_pt.inst[1] = 0;
    run_ms(100);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_FAULT);
}

static void t_connections(void) {
    fresh();
    /* a gateway with two analysers behind it (units 1 and 2): one connection for both */
    inst_sim_t *g = add_sim(IP(80), 0);
    g->holding[0] = 111;
    inst_tags.enable[0] = true; inst_tags.ip[0] = IP(80); inst_tags.unit[0] = 1;
    inst_tags.enable[1] = true; inst_tags.ip[1] = IP(80); inst_tags.unit[1] = 2;
    point(0, 1, INST_FC_HOLDING, 0, INST_T_U16, 0);
    point(1, 2, INST_FC_HOLDING, 0, INST_T_U16, 0);
    run_ms(2000);
    CHECK_EQ(connects, 1u);
    CHECK_EQ(inst_tags.state[0], INST_S_OK); CHECK_EQ(inst_tags.state[1], INST_S_OK);
    CHECK(inst_pt.value[1] == 111.0f);
    /* the gateway answers 0B for a unit that is not there: lost comms for that one only */
    g->unit = 1;
    run_ms(4000);
    CHECK_EQ(inst_tags.state[0], INST_S_OK);
    CHECK_EQ(inst_tags.state[1], INST_S_LOST);
    CHECK_EQ(inst_tags.last_exc[1], 0x0B);
    CHECK_EQ(inst_pt.q[1], PLAT_Q_UNAVAILABLE);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_VALID);

    /* five analysers at five addresses, three connections: every one is still read */
    fresh();
    for (unsigned k = 0; k < 5; ++k) {
        inst_sim_t *s = add_sim(IP(90 + k), 1);
        s->holding[0] = (uint16_t)(k + 1);
        inst_tags.enable[k] = true; inst_tags.ip[k] = IP(90 + k);
        point(k, (uint8_t)(k + 1), INST_FC_HOLDING, 0, INST_T_U16, 0);
    }
    run_ms(3000);
    for (unsigned k = 0; k < 5; ++k) { CHECK_EQ(inst_tags.state[k], INST_S_OK); CHECK(inst_pt.value[k] == (float)(k + 1)); }
    CHECK(inst_tags.good[4] >= 2u);

    /* refused, and nothing at that address */
    fresh();
    inst_sim_t *r = add_sim(IP(70), 1);
    r->refuse = true;
    inst_tags.enable[0] = true; inst_tags.ip[0] = IP(70);
    inst_tags.enable[1] = true; inst_tags.ip[1] = IP(71);
    point(0, 1, INST_FC_HOLDING, 0, INST_T_U16, 0);
    point(1, 2, INST_FC_HOLDING, 0, INST_T_U16, 0);
    run_ms(200);
    CHECK_EQ(inst_tags.last_err[0], INST_E_CONNECT); CHECK_EQ(inst_tags.last_err[1], INST_E_CONNECT);
    CHECK_EQ(inst_tags.state[0], INST_S_FAILING);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_UNAVAILABLE);                     /* never read */
    /* no IP: bad settings */
    inst_tags.ip[1] = 0;
    run_ms(100);
    CHECK_EQ(inst_tags.state[1], INST_S_BAD_SETTINGS);
    /* the I/O port not working */
    port_ok = false;
    r->refuse = false;
    run_ms(4000);
    CHECK_EQ(inst_tags.last_err[0], INST_E_NO_PORT);
    CHECK_EQ(inst_tags.state[0], INST_S_LOST);
    port_ok = true;
    run_ms(6000);
    CHECK_EQ(inst_tags.state[0], INST_S_OK);
    /* no network given at all */
    inst_init(NULL);
    run_ms(4000);
    CHECK_EQ(inst_tags.last_err[0], INST_E_NO_PORT);
    inst_init(&fnet);
}

static void t_explorer(void) {
    fresh();
    inst_sim_t *a = add_sim(IP(70), 1);
    inst_sim_put_f32(a->holding, 100, 3.25f, INST_O_CDAB);
    a->discrete[0] = 0xA5;
    inst_ex_view_t v;
    inst_ex_view(&v); CHECK_EQ(v.result, INST_EX_IDLE);
    CHECK(!inst_ex_start(IP(70), 502, 1, 6, 0, 1));                   /* reads only */
    CHECK(!inst_ex_start(IP(70), 502, 1, 3, 0, 126));
    CHECK(!inst_ex_start(IP(70), 502, 1, 3, 65530, 10));
    CHECK(!inst_ex_start(0, 502, 1, 3, 0, 1));
    CHECK(inst_ex_start(IP(70), 502, 1, 3, 100, 2));
    CHECK(!inst_ex_start(IP(70), 502, 1, 3, 100, 2));                 /* one at a time */
    inst_ex_view(&v); CHECK_EQ(v.result, INST_EX_BUSY);
    run_ms(100);
    inst_ex_view(&v);
    CHECK_EQ(v.result, INST_EX_OK); CHECK_EQ(v.nbytes, 4); CHECK_EQ(v.seq, 1u);
    uint16_t w[2] = { (uint16_t)(v.data[0] << 8 | v.data[1]), (uint16_t)(v.data[2] << 8 | v.data[3]) };
    CHECK(near(inst_decode(INST_T_F32, INST_O_CDAB, 0, w, NULL), 3.25f));
    CHECK(inst_ex_start(IP(70), 502, 1, 2, 0, 8));
    run_ms(100); inst_ex_view(&v);
    CHECK_EQ(v.result, INST_EX_OK); CHECK_EQ(v.nbytes, 1); CHECK_EQ(v.data[0], 0xA5);
    CHECK(inst_ex_start(IP(70), 502, 1, 4, 510, 5));                  /* past its map */
    run_ms(100); inst_ex_view(&v);
    CHECK_EQ(v.result, INST_EX_EXCEPTION); CHECK_EQ(v.exception, 2);
    CHECK(inst_ex_start(IP(99), 502, 1, 3, 0, 1));                    /* nothing there */
    run_ms(100); inst_ex_view(&v);
    CHECK_EQ(v.result, INST_EX_CONNECT);
    a->silent = true;
    CHECK(inst_ex_start(IP(70), 502, 1, 3, 0, 1));
    run_ms(2200); inst_ex_view(&v);
    CHECK_EQ(v.result, INST_EX_TIMEOUT);
    port_ok = false;
    CHECK(inst_ex_start(IP(70), 502, 1, 3, 0, 1));
    run_ms(40); inst_ex_view(&v);
    CHECK_EQ(v.result, INST_EX_NO_PORT);
    CHECK(!strcmp(inst_ex_result_name(INST_EX_TIMEOUT), "no reply"));
}

/* the whole path: poller, inst_w5500, the W5500 driver and the simulated chip, sharing it with
 * the I/O port's own upkeep */
static w5500_sim_t chip;
static iob_simnet_t net;
static iob_w5500_t io;
static inst_w5500_t iw;
static uint32_t sim_ms;
static uint32_t clk(void) { return sim_ms; }
static void t_w5500(void) {
    fresh();
    iob_simnet_init(&net, 0);
    w5500_sim_init(&chip);
    w5500_sim_simnet(&chip, &net);
    chip.bus.ms = clk;
    static const uint8_t mac[6] = { 0x02, 1, 2, 3, 4, 5 };
    iob_w5500_init(&io, &chip.bus, mac);
    inst_w5500_init(&iw, &io);
    inst_init(&iw.net);
    inst_sim_t *a = add_sim(IP(70), 1);
    inst_sim_put_f32(a->holding, 0, 20.9f, INST_O_DCBA);
    net.inst[net.n_inst++] = a;
    inst_tags.enable[0] = true; inst_tags.ip[0] = IP(70);
    point(0, 1, INST_FC_HOLDING, 0, INST_T_F32, INST_O_DCBA);
    for (unsigned i = 0; i < 100; ++i) { sim_ms = now; inst_scan(now); inst_cycle(now); now += 20; }
    inst_scan(now);
    CHECK_EQ(inst_tags.state[0], INST_S_OK);
    CHECK(near(inst_pt.value[0], 20.9f));
    CHECK(a->requests >= 2u);
    CHECK_EQ(chip.sreg[INST_W5500_SOCK0][W5500_Sn_SR], W5500_SOCK_ESTABLISHED);    /* socket 4 */
    CHECK_EQ(chip.sreg[0][W5500_Sn_SR], W5500_SOCK_CLOSED);                        /* the boards' sockets untouched */
    /* the explorer on socket 7 */
    CHECK(inst_ex_start(IP(70), 502, 1, 3, 0, 2));
    for (unsigned i = 0; i < 10; ++i) { sim_ms = now; inst_cycle(now); now += 20; }
    inst_ex_view_t v; inst_ex_view(&v);
    CHECK_EQ(v.result, INST_EX_OK); CHECK_EQ(v.nbytes, 4);
    /* the analyser goes away: no reply, then lost */
    net.n_inst = 0;
    w5500_sim_peer_close(&chip, INST_W5500_SOCK0);
    for (unsigned i = 0; i < 300; ++i) { sim_ms = now; inst_scan(now); inst_cycle(now); now += 20; }
    inst_scan(now);
    CHECK_EQ(inst_tags.state[0], INST_S_LOST);
    CHECK_EQ(inst_pt.q[0], PLAT_Q_UNAVAILABLE);
    net.n_inst = 1;
    for (unsigned i = 0; i < 400; ++i) { sim_ms = now; inst_scan(now); inst_cycle(now); now += 20; }
    inst_scan(now);
    CHECK_EQ(inst_tags.state[0], INST_S_OK);
    inst_init(&fnet);
}

/* the settings are parameters with sensible defaults */
static void t_params(void) {
    const param_def_t *d = param_find(PARAM_ID_INST_PORT);
    CHECK(d != NULL);
    if (d) { CHECK(d->dflt != NULL); CHECK_EQ(((const uint16_t *)d->dflt)[7], 502); }
    d = param_find(PARAM_ID_PT_GAIN);
    CHECK(d != NULL);
    if (d) CHECK(((const float *)d->dflt)[31] == 1.0f);
    CHECK(param_find(PARAM_ID_PT_NAME_FIRST + 31) != NULL);
    CHECK(param_find(PARAM_ID_PT_UNIT_FIRST + 31) != NULL);
    CHECK(param_find(PARAM_ID_INST_NAME_FIRST + 7) != NULL);
    d = param_find(PARAM_ID_PT_VALUE);
    CHECK(d && (d->flags & PF_RO));
}

void test_inst_all(void) {
    RUN(t_decode);
    RUN(t_values_and_grouping);
    RUN(t_quality);
    RUN(t_connections);
    RUN(t_explorer);
    RUN(t_w5500);
    RUN(t_params);
    inst_reset();
    inst_init(NULL);
}
