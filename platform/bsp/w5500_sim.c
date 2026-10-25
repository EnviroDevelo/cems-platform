/* w5500_sim.c - see w5500_sim.h */
#include <string.h>
#include "w5500_sim.h"
#include "iob_net.h"

#define RD16(p) ((uint16_t)((p)[0] << 8 | (p)[1]))
#define RD32(p) ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (p)[3])

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static void chip_reset(w5500_sim_t *c) {
    c->resets++;
    memset(c->common, 0, sizeof c->common);
    memset(c->sreg, 0, sizeof c->sreg);
    memset(c->tx_rd, 0, sizeof c->tx_rd);
    memset(c->rx_wr, 0, sizeof c->rx_wr);
    put16(&c->common[W5500_RTR], 2000);
    c->common[W5500_RCR] = 8;
    for (unsigned s = 0; s < W5500_SOCKETS; ++s) { c->sreg[s][W5500_Sn_RXBUF] = 2; c->sreg[s][W5500_Sn_TXBUF] = 2; }
}

uint32_t w5500_sim_ip(const w5500_sim_t *c) { return RD32(&c->common[W5500_SIPR]); }

static uint16_t rx_rd(const w5500_sim_t *c, uint8_t s) { return RD16(&c->sreg[s][W5500_Sn_RX_RD]); }
static uint16_t tx_wr(const w5500_sim_t *c, uint8_t s) { return RD16(&c->sreg[s][W5500_Sn_TX_WR]); }

void w5500_sim_rx_push(w5500_sim_t *c, uint8_t s, const uint8_t *d, uint16_t n) {
    if ((uint16_t)(c->rx_wr[s] - rx_rd(c, s)) + n > 2048u) return;   /* no room: lost, as on the wire */
    for (uint16_t k = 0; k < n; ++k) c->rx[s][(uint16_t)(c->rx_wr[s] + k) & 0x7FFu] = d[k];
    c->rx_wr[s] = (uint16_t)(c->rx_wr[s] + n);
    c->sreg[s][W5500_Sn_IR] |= W5500_IR_RECV;
}

void w5500_sim_udp_push(w5500_sim_t *c, uint8_t s, uint32_t ip, uint16_t port, const uint8_t *d, uint16_t n) {
    if ((uint16_t)(c->rx_wr[s] - rx_rd(c, s)) + 8u + n > 2048u) return;
    uint8_t h[8] = { (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip,
                     (uint8_t)(port >> 8), (uint8_t)port, (uint8_t)(n >> 8), (uint8_t)n };
    w5500_sim_rx_push(c, s, h, 8);
    w5500_sim_rx_push(c, s, d, n);
}

void w5500_sim_peer_close(w5500_sim_t *c, uint8_t s) {
    if (c->sreg[s][W5500_Sn_SR] == W5500_SOCK_ESTABLISHED) {
        c->sreg[s][W5500_Sn_SR] = W5500_SOCK_CLOSE_WAIT;
        c->sreg[s][W5500_Sn_IR] |= W5500_IR_DISCON;
    }
}

static void command(w5500_sim_t *c, uint8_t s, uint8_t cr) {
    uint8_t *r = c->sreg[s];
    c->commands++;
    switch (cr) {
    case W5500_CR_OPEN:
        if ((r[W5500_Sn_MR] & 0x0Fu) == W5500_MR_TCP) r[W5500_Sn_SR] = W5500_SOCK_INIT;
        else if ((r[W5500_Sn_MR] & 0x0Fu) == W5500_MR_UDP) r[W5500_Sn_SR] = W5500_SOCK_UDP;
        c->tx_rd[s] = tx_wr(c, s);
        put16(&r[W5500_Sn_RX_RD], c->rx_wr[s]);
        break;
    case W5500_CR_CONNECT:
        if (r[W5500_Sn_SR] != W5500_SOCK_INIT) break;
        c->connects++;
        bool ok = !c->refuse_connects && c->tcp_connect && c->tcp_connect(c, s, RD32(&r[W5500_Sn_DIPR]), RD16(&r[W5500_Sn_DPORT]));
        if (c->refuse_connects) c->refuse_connects--;
        r[W5500_Sn_SR] = ok ? W5500_SOCK_ESTABLISHED : W5500_SOCK_CLOSED;
        r[W5500_Sn_IR] |= ok ? W5500_IR_CON : W5500_IR_TIMEOUT;
        break;
    case W5500_CR_SEND: {
        uint16_t n = (uint16_t)(tx_wr(c, s) - c->tx_rd[s]);
        uint8_t d[2048];
        for (uint16_t k = 0; k < n && k < sizeof d; ++k) d[k] = c->tx[s][(uint16_t)(c->tx_rd[s] + k) & 0x7FFu];
        c->tx_rd[s] = tx_wr(c, s);
        if (r[W5500_Sn_SR] == W5500_SOCK_ESTABLISHED) {
            r[W5500_Sn_IR] |= W5500_IR_SENDOK;
            if (c->tcp_send) c->tcp_send(c, s, d, n);
        } else if (r[W5500_Sn_SR] == W5500_SOCK_UDP) {
            r[W5500_Sn_IR] |= W5500_IR_SENDOK;
            if (c->udp_send) c->udp_send(c, s, RD32(&r[W5500_Sn_DIPR]), RD16(&r[W5500_Sn_DPORT]), d, n);
        } else {
            r[W5500_Sn_IR] |= W5500_IR_TIMEOUT;
        }
        break; }
    case W5500_CR_DISCON: case W5500_CR_CLOSE:
        r[W5500_Sn_SR] = W5500_SOCK_CLOSED;
        break;
    default:                        /* RECV: Sn_RX_RD already moved */
        break;
    }
}

static uint8_t read_byte(w5500_sim_t *c, uint8_t block, uint16_t a) {
    if (block == W5500_COMMON) {
        if (a == W5500_VERSIONR) return 0x04;
        if (a == W5500_PHYCFGR) return (uint8_t)(0xB8u | (c->link ? 1u : 0u));
        return a < sizeof c->common ? c->common[a] : 0;
    }
    uint8_t s = (uint8_t)((block - 1u) / 4u), kind = (uint8_t)((block - 1u) % 4u);
    if (s >= W5500_SOCKETS) return 0;
    if (kind == 1) return c->tx[s][a & 0x7FFu];
    if (kind == 2) return c->rx[s][a & 0x7FFu];
    if (kind != 0 || a >= 0x30u) return 0;
    uint16_t v;
    switch (a) {
    case W5500_Sn_CR: return 0;     /* commands complete at once */
    case W5500_Sn_TX_FSR: case W5500_Sn_TX_FSR + 1:
        v = (uint16_t)(2048u - (uint16_t)(tx_wr(c, s) - c->tx_rd[s]));
        return a == W5500_Sn_TX_FSR ? (uint8_t)(v >> 8) : (uint8_t)v;
    case 0x22: case 0x23:           /* Sn_TX_RD */
        return a == 0x22 ? (uint8_t)(c->tx_rd[s] >> 8) : (uint8_t)c->tx_rd[s];
    case W5500_Sn_RX_RSR: case W5500_Sn_RX_RSR + 1:
        if (a == W5500_Sn_RX_RSR && c->poll) c->poll(c, s);
        v = (uint16_t)(c->rx_wr[s] - rx_rd(c, s));
        return a == W5500_Sn_RX_RSR ? (uint8_t)(v >> 8) : (uint8_t)v;
    case 0x2A: case 0x2B:           /* Sn_RX_WR */
        return a == 0x2A ? (uint8_t)(c->rx_wr[s] >> 8) : (uint8_t)c->rx_wr[s];
    default:
        return c->sreg[s][a];
    }
}

static void write_byte(w5500_sim_t *c, uint8_t block, uint16_t a, uint8_t v) {
    if (block == W5500_COMMON) {
        if (a == W5500_MR && (v & 0x80u)) { chip_reset(c); return; }
        if (a < sizeof c->common && a != W5500_VERSIONR) c->common[a] = v;
        return;
    }
    uint8_t s = (uint8_t)((block - 1u) / 4u), kind = (uint8_t)((block - 1u) % 4u);
    if (s >= W5500_SOCKETS) return;
    if (kind == 1) { c->tx[s][a & 0x7FFu] = v; return; }
    if (kind == 2) { c->rx[s][a & 0x7FFu] = v; return; }
    if (kind != 0 || a >= 0x30u) return;
    if (a == W5500_Sn_CR) { command(c, s, v); return; }
    if (a == W5500_Sn_IR) { c->sreg[s][a] &= (uint8_t)~v; return; }   /* write 1 to clear */
    if (a == W5500_Sn_SR || a == W5500_Sn_TX_FSR || a == W5500_Sn_TX_FSR + 1 || a == W5500_Sn_RX_RSR || a == W5500_Sn_RX_RSR + 1) return;
    c->sreg[s][a] = v;
}

static void sim_cs(w5500_bus_t *b, bool active) {
    w5500_sim_t *c = (w5500_sim_t *)b->ctx;
    c->sel = active;
    c->pos = 0;
}

static void sim_xfer(w5500_bus_t *b, const uint8_t *tx, uint8_t *rx, uint16_t n) {
    w5500_sim_t *c = (w5500_sim_t *)b->ctx;
    for (uint16_t k = 0; k < n; ++k) {
        uint8_t in = tx ? tx[k] : 0, out = 0xFF;
        c->spi_bytes++;
        if (c->sel && c->present) {
            if (c->pos < 3) {
                c->hdr[c->pos] = in;
                if (c->pos == 1) c->addr = (uint16_t)(c->hdr[0] << 8 | c->hdr[1]);
                out = (uint8_t)(c->pos);   /* the chip clocks out 0x00 0x01 0x02 during the header */
            } else {
                uint8_t block = (uint8_t)(c->hdr[2] >> 3);
                if (c->hdr[2] & 0x04u) { write_byte(c, block, c->addr, in); out = 0; }
                else out = read_byte(c, block, c->addr);
                c->addr++;
            }
            c->pos++;
        }
        if (rx) rx[k] = out;
    }
}

static uint32_t sim_ms(void) { return 0; }

void w5500_sim_init(w5500_sim_t *c) {
    memset(c, 0, sizeof *c);
    c->present = true;
    c->link = true;
    c->bus.cs = sim_cs;
    c->bus.xfer = sim_xfer;
    c->bus.ctx = c;
    c->bus.ms = NULL;               /* no clock: the driver bounds its waits by tries (see w5500.c) */
    (void)sim_ms;
    chip_reset(c);
    c->resets = 0;
}

/* ---------------- backend: the simulated I/O network ---------------- */
static void lk(w5500_sim_t *c, bool on) { if (c->lock) c->lock(on); }

static bool net_connect(w5500_sim_t *c, uint8_t s, uint32_t ip, uint16_t port) {
    (void)s;
    iob_simnet_t *net = (iob_simnet_t *)c->ctx;
    bool ok = false;
    lk(c, true);
    for (unsigned i = 0; i < net->n && port == IOB_TCP_PORT; ++i) {
        const iob_node_t *nd = net->nodes[i];
        if (nd->powered && nd->ip && nd->ip == ip && (!nd->master_ip || nd->master_ip == w5500_sim_ip(c))) ok = true;
    }
    lk(c, false);
    return ok;
}

static void net_tcp(w5500_sim_t *c, uint8_t s, const uint8_t *d, uint16_t n) {
    iob_simnet_t *net = (iob_simnet_t *)c->ctx;
    int len;
    while (n && (len = iob_mbap_complete(d, n)) > 0) {
        uint8_t rtu[IOB_MBAP_MAX], rep[IOB_MBAP_MAX], back[IOB_MBAP_MAX];
        uint16_t tid;
        size_t rl = iob_rtu_from_mbap(d, (size_t)len, &tid, rtu);
        d += len; n = (uint16_t)(n - len);
        if (!rl) continue;
        lk(c, true);
        net->master_ip = w5500_sim_ip(c);
        net->port.target(&net->port, RD32(&c->sreg[s][W5500_Sn_DIPR]));
        uint16_t r = net->port.xfer(&net->port, rtu, (uint16_t)rl, rep, sizeof rep, 0);
        lk(c, false);
        size_t bl = r ? iob_mbap_from_rtu(rep, r, tid, back) : 0;
        if (bl) w5500_sim_rx_push(c, s, back, (uint16_t)bl);
    }
}

static void net_udp(w5500_sim_t *c, uint8_t s, uint32_t ip, uint16_t port, const uint8_t *d, uint16_t n) {
    iob_simnet_t *net = (iob_simnet_t *)c->ctx;
    if (ip != 0xFFFFFFFFu || port != IOB_DISC_PORT) return;
    uint8_t b[IOB_DISC_LEN];
    lk(c, true);
    net->master_ip = w5500_sim_ip(c);
    (void)net->disc.send(&net->disc, d, n);
    uint16_t got;
    while ((got = net->disc.recv(&net->disc, b, sizeof b, 0)) > 0) w5500_sim_udp_push(c, s, 0, IOB_DISC_PORT, b, got);
    lk(c, false);
}

void w5500_sim_simnet(w5500_sim_t *c, iob_simnet_t *net) {
    c->ctx = net;
    c->tcp_connect = net_connect;
    c->tcp_send = net_tcp;
    c->udp_send = net_udp;
}
