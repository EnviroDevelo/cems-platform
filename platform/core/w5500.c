/* w5500.c - see w5500.h */
#include <string.h>
#include "w5500.h"

#define CMD_SPINS 2000u             /* Sn_CR and MR clear within a few SPI reads; this bounds a dead chip */

static void frame(w5500_t *w, uint8_t block, uint16_t addr, bool wr) {
    uint8_t h[3] = { (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)((block << 3) | (wr ? 0x04u : 0u)) };
    w->bus->cs(w->bus, true);
    w->bus->xfer(w->bus, h, NULL, 3);
}

void w5500_read(w5500_t *w, uint8_t block, uint16_t addr, uint8_t *buf, uint16_t n) {
    frame(w, block, addr, false);
    w->bus->xfer(w->bus, NULL, buf, n);
    w->bus->cs(w->bus, false);
}

void w5500_write(w5500_t *w, uint8_t block, uint16_t addr, const uint8_t *buf, uint16_t n) {
    frame(w, block, addr, true);
    w->bus->xfer(w->bus, buf, NULL, n);
    w->bus->cs(w->bus, false);
}

static uint8_t rd8(w5500_t *w, uint8_t block, uint16_t addr) { uint8_t v; w5500_read(w, block, addr, &v, 1); return v; }
static void wr8(w5500_t *w, uint8_t block, uint16_t addr, uint8_t v) { w5500_write(w, block, addr, &v, 1); }
static void wr16(w5500_t *w, uint8_t block, uint16_t addr, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    w5500_write(w, block, addr, b, 2);
}
static void wr32(w5500_t *w, uint8_t block, uint16_t addr, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    w5500_write(w, block, addr, b, 4);
}
/* a 16-bit counter the chip may change between the two bytes: read until two reads agree (datasheet) */
static uint16_t rd16(w5500_t *w, uint8_t block, uint16_t addr) {
    uint16_t a = 0, b;
    for (unsigned k = 0; k < 4u; ++k) {
        uint8_t v[2];
        w5500_read(w, block, addr, v, 2);
        b = (uint16_t)(v[0] << 8 | v[1]);
        if (k && a == b) break;
        a = b;
    }
    return a;
}

static uint32_t now(w5500_t *w) { return w->bus->ms ? w->bus->ms() : 0u; }
/* A wait is over when its time has run out, or after about as many tries as it has ms (each try
 * sleeps a tick on the target), so a clock that stands still (host tests) cannot hang it. */
static bool late(w5500_t *w, uint32_t t0, uint32_t ms, unsigned spins) {
    return (w->bus->ms && (uint32_t)(now(w) - t0) >= ms) || spins > ms + CMD_SPINS;
}
static void nap(w5500_t *w) { if (w->bus->wait) w->bus->wait(w->bus, 1); }

static bool cmd(w5500_t *w, uint8_t s, uint8_t c) {
    wr8(w, W5500_SREG(s), W5500_Sn_CR, c);
    for (unsigned k = 0; k < CMD_SPINS; ++k) if (rd8(w, W5500_SREG(s), W5500_Sn_CR) == 0) return true;
    w->ok = false;                  /* the chip stopped taking commands: set it up again */
    return false;
}

static bool usable(w5500_t *w, uint8_t s) { return w->ok && s < W5500_SOCKETS; }

static void set_addr(w5500_t *w, uint32_t ip, uint32_t mask, uint32_t gw) {
    wr32(w, W5500_COMMON, W5500_GAR, gw);
    wr32(w, W5500_COMMON, W5500_SUBR, mask);
    wr32(w, W5500_COMMON, W5500_SIPR, ip);
    w->ip = ip; w->mask = mask; w->gw = gw;
}

bool w5500_init(w5500_t *w, w5500_bus_t *bus, const uint8_t mac[6], uint32_t ip, uint32_t mask, uint32_t gw) {
    w->bus = bus;
    w->ok = false;
    memcpy(w->mac, mac, 6);
    w->resets++;
    if (bus->reset) {               /* RSTn low for at least 500 us, then 1 ms for the PLL */
        bus->reset(bus, true);
        if (bus->wait) bus->wait(bus, 1);
        bus->reset(bus, false);
        if (bus->wait) bus->wait(bus, 2);
    }
    wr8(w, W5500_COMMON, W5500_MR, 0x80);   /* software reset */
    unsigned k;
    for (k = 0; k < CMD_SPINS && (rd8(w, W5500_COMMON, W5500_MR) & 0x80u); ++k) { }
    if (k == CMD_SPINS || rd8(w, W5500_COMMON, W5500_VERSIONR) != 0x04u) return false;
    w5500_write(w, W5500_COMMON, W5500_SHAR, mac, 6);
    set_addr(w, ip, mask, gw);
    /* ARP and TCP retries: 20 ms x 3 tries. The boards sit on one short segment; an absent board
     * must fail fast, since the bus task serves three boards every 20 ms. */
    wr16(w, W5500_COMMON, W5500_RTR, 200);
    wr8(w, W5500_COMMON, W5500_RCR, 2);
    for (uint8_t s = 0; s < W5500_SOCKETS; ++s) {   /* 2 KB each way per socket (the reset values, made sure) */
        wr8(w, W5500_SREG(s), W5500_Sn_RXBUF, 2);
        wr8(w, W5500_SREG(s), W5500_Sn_TXBUF, 2);
    }
    w->ok = true;
    return true;
}

bool w5500_set_addr(w5500_t *w, uint32_t ip, uint32_t mask, uint32_t gw) {
    if (!w->ok) return false;
    set_addr(w, ip, mask, gw);
    return true;
}

bool w5500_link_up(w5500_t *w) { return w->ok && (rd8(w, W5500_COMMON, W5500_PHYCFGR) & 0x01u); }

void w5500_close(w5500_t *w, uint8_t s) {
    if (!usable(w, s)) return;
    (void)cmd(w, s, W5500_CR_CLOSE);
    wr8(w, W5500_SREG(s), W5500_Sn_IR, 0xFF);
}

static bool open_as(w5500_t *w, uint8_t s, uint8_t mode, uint16_t local_port, uint8_t want) {
    w5500_close(w, s);
    wr8(w, W5500_SREG(s), W5500_Sn_MR, mode);
    wr16(w, W5500_SREG(s), W5500_Sn_PORT, local_port);
    return cmd(w, s, W5500_CR_OPEN) && rd8(w, W5500_SREG(s), W5500_Sn_SR) == want;
}

bool w5500_tcp_connect(w5500_t *w, uint8_t s, uint32_t ip, uint16_t port, uint16_t local_port, uint32_t timeout_ms) {
    if (!usable(w, s) || !open_as(w, s, W5500_MR_TCP | W5500_MR_ND, local_port, W5500_SOCK_INIT)) return false;
    wr32(w, W5500_SREG(s), W5500_Sn_DIPR, ip);
    wr16(w, W5500_SREG(s), W5500_Sn_DPORT, port);
    if (!cmd(w, s, W5500_CR_CONNECT)) return false;
    uint32_t t0 = now(w);
    for (unsigned spins = 0;; ++spins) {
        uint8_t sr = rd8(w, W5500_SREG(s), W5500_Sn_SR);
        if (sr == W5500_SOCK_ESTABLISHED) { wr8(w, W5500_SREG(s), W5500_Sn_IR, W5500_IR_CON); return true; }
        if (sr == W5500_SOCK_CLOSED || late(w, t0, timeout_ms, spins)) break;
        nap(w);
    }
    w5500_close(w, s);
    return false;
}

bool w5500_tcp_ok(w5500_t *w, uint8_t s) {
    return usable(w, s) && rd8(w, W5500_SREG(s), W5500_Sn_SR) == W5500_SOCK_ESTABLISHED;
}

/* put n bytes in the socket's TX buffer and send them: wait for room, then for SEND_OK */
static bool put_and_send(w5500_t *w, uint8_t s, const uint8_t *buf, uint16_t n, uint32_t timeout_ms) {
    uint32_t t0 = now(w);
    for (unsigned spins = 0; rd16(w, W5500_SREG(s), W5500_Sn_TX_FSR) < n; ++spins) {
        if (late(w, t0, timeout_ms, spins)) return false;
        nap(w);
    }
    uint16_t wp = rd16(w, W5500_SREG(s), W5500_Sn_TX_WR);
    w5500_write(w, W5500_STX(s), wp, buf, n);       /* the chip wraps the offset inside the buffer */
    wr16(w, W5500_SREG(s), W5500_Sn_TX_WR, (uint16_t)(wp + n));
    if (!cmd(w, s, W5500_CR_SEND)) return false;
    for (unsigned spins = 0;; ++spins) {
        uint8_t ir = rd8(w, W5500_SREG(s), W5500_Sn_IR);
        if (ir & W5500_IR_SENDOK) { wr8(w, W5500_SREG(s), W5500_Sn_IR, W5500_IR_SENDOK); return true; }
        if ((ir & W5500_IR_TIMEOUT) || rd8(w, W5500_SREG(s), W5500_Sn_SR) == W5500_SOCK_CLOSED) {
            wr8(w, W5500_SREG(s), W5500_Sn_IR, W5500_IR_TIMEOUT);
            return false;
        }
        if (late(w, t0, timeout_ms, spins)) return false;
        nap(w);
    }
}

bool w5500_send(w5500_t *w, uint8_t s, const uint8_t *buf, uint16_t n, uint32_t timeout_ms) {
    if (!usable(w, s) || n == 0 || n > 2048u || rd8(w, W5500_SREG(s), W5500_Sn_SR) != W5500_SOCK_ESTABLISHED) return false;
    return put_and_send(w, s, buf, n, timeout_ms);
}

uint16_t w5500_rx_avail(w5500_t *w, uint8_t s) { return usable(w, s) ? rd16(w, W5500_SREG(s), W5500_Sn_RX_RSR) : 0u; }

static void take(w5500_t *w, uint8_t s, uint16_t rp, uint8_t *buf, uint16_t n) {
    if (buf && n) w5500_read(w, W5500_SRX(s), rp, buf, n);
}
static void consumed(w5500_t *w, uint8_t s, uint16_t rp) {
    wr16(w, W5500_SREG(s), W5500_Sn_RX_RD, rp);
    (void)cmd(w, s, W5500_CR_RECV);
}

uint16_t w5500_recv(w5500_t *w, uint8_t s, uint8_t *buf, uint16_t cap) {
    uint16_t n = w5500_rx_avail(w, s);
    if (n > cap) n = cap;
    if (!n) return 0;
    uint16_t rp = rd16(w, W5500_SREG(s), W5500_Sn_RX_RD);
    take(w, s, rp, buf, n);
    consumed(w, s, (uint16_t)(rp + n));
    return n;
}

bool w5500_udp_open(w5500_t *w, uint8_t s, uint16_t local_port) {
    return usable(w, s) && open_as(w, s, W5500_MR_UDP, local_port, W5500_SOCK_UDP);
}

bool w5500_udp_send(w5500_t *w, uint8_t s, uint32_t ip, uint16_t port, const uint8_t *buf, uint16_t n, uint32_t timeout_ms) {
    if (!usable(w, s) || n == 0 || n > 1472u || rd8(w, W5500_SREG(s), W5500_Sn_SR) != W5500_SOCK_UDP) return false;
    wr32(w, W5500_SREG(s), W5500_Sn_DIPR, ip);
    wr16(w, W5500_SREG(s), W5500_Sn_DPORT, port);
    return put_and_send(w, s, buf, n, timeout_ms);
}

uint16_t w5500_udp_recv(w5500_t *w, uint8_t s, uint8_t *buf, uint16_t cap, uint32_t *from_ip, uint16_t *from_port) {
    uint16_t avail = w5500_rx_avail(w, s);
    if (avail < 8u) return 0;
    uint16_t rp = rd16(w, W5500_SREG(s), W5500_Sn_RX_RD);
    uint8_t h[8];                   /* each datagram: sender IP, port, length */
    take(w, s, rp, h, 8);
    uint16_t len = (uint16_t)(h[6] << 8 | h[7]);
    if ((uint32_t)len + 8u > avail) {                /* not consistent: drop everything there */
        consumed(w, s, (uint16_t)(rp + avail));
        return 0;
    }
    uint16_t n = len < cap ? len : cap;
    take(w, s, (uint16_t)(rp + 8u), buf, n);
    consumed(w, s, (uint16_t)(rp + 8u + len));
    if (from_ip) *from_ip = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
    if (from_port) *from_port = (uint16_t)(h[4] << 8 | h[5]);
    return n;
}
