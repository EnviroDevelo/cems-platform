/* test_w5500.c - the W5500 driver against the simulated chip (registers, commands, buffers that
 * wrap, the UDP header, a refused connection, a peer that closes), and the I/O port transport's
 * upkeep of the chip (set-up, retries while it is missing, link state, an IP change, the MAC). */
#include <string.h>
#include "tst.h"
#include "w5500.h"
#include "w5500_sim.h"
#include "iob_w5500.h"
#include "iob_net.h"
#include "mb_rtu.h"
#include "bsp_host.h"
#include "param.h"

static w5500_sim_t chip;
static w5500_t w;
static const uint8_t MAC[6] = { 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };

/* a backend that keeps what was sent and answers each TCP send with the same bytes */
static uint8_t  seen[8192];
static size_t   seen_n;
static bool     accept_all = true;
static bool b_connect(w5500_sim_t *c, uint8_t s, uint32_t ip, uint16_t port) { (void)c; (void)s; (void)ip; (void)port; return accept_all; }
static void b_tcp(w5500_sim_t *c, uint8_t s, const uint8_t *d, uint16_t n) {
    if (seen_n + n <= sizeof seen) { memcpy(seen + seen_n, d, n); seen_n += n; }
    w5500_sim_rx_push(c, s, d, n);
}
static uint32_t u_ip; static uint16_t u_port, u_n;
static void b_udp(w5500_sim_t *c, uint8_t s, uint32_t ip, uint16_t port, const uint8_t *d, uint16_t n) {
    (void)c; (void)s; (void)d;
    u_ip = ip; u_port = port; u_n = n;
}

static void fresh(void) {
    w5500_sim_init(&chip);
    chip.tcp_connect = b_connect; chip.tcp_send = b_tcp; chip.udp_send = b_udp;
    memset(&w, 0, sizeof w);
    seen_n = 0; accept_all = true;
}

static void t_init(void) {
    fresh();
    chip.present = false;
    CHECK(!w5500_init(&w, &chip.bus, MAC, 0xC0A8013Cu, 0xFFFFFF00u, 0));
    CHECK(!w.ok);
    CHECK(!w5500_tcp_connect(&w, 0, 0xC0A8013Du, 502, 50000, 50));   /* nothing done without a chip */
    chip.present = true;
    CHECK(w5500_init(&w, &chip.bus, MAC, 0xC0A8013Cu, 0xFFFFFF00u, 0xC0A80101u));
    CHECK(w.ok);
    CHECK_EQ(chip.resets, 1u);                       /* the software reset reached it once: it was missing the first time */
    CHECK(memcmp(&chip.common[W5500_SHAR], MAC, 6) == 0);
    CHECK_EQ(w5500_sim_ip(&chip), 0xC0A8013Cu);
    CHECK_EQ(chip.common[W5500_SUBR + 3], 0x00); CHECK_EQ(chip.common[W5500_SUBR], 0xFF);
    CHECK_EQ(chip.common[W5500_GAR + 3], 0x01);
    CHECK_EQ(chip.common[W5500_RTR] << 8 | chip.common[W5500_RTR + 1], 200);   /* 20 ms ... */
    CHECK_EQ(chip.common[W5500_RCR], 2);                                        /* ... x 3 tries */
    CHECK(w5500_link_up(&w));
    chip.link = false;
    CHECK(!w5500_link_up(&w));
    CHECK(w5500_set_addr(&w, 0x0A000001u, 0xFF000000u, 0));
    CHECK_EQ(w5500_sim_ip(&chip), 0x0A000001u);
}

static void t_tcp(void) {
    fresh();
    CHECK(w5500_init(&w, &chip.bus, MAC, 0xC0A8013Cu, 0xFFFFFF00u, 0));
    accept_all = false;
    CHECK(!w5500_tcp_connect(&w, 1, 0xC0A8013Du, 502, 50001, 50));
    CHECK_EQ(chip.sreg[1][W5500_Sn_SR], W5500_SOCK_CLOSED);
    accept_all = true;
    CHECK(w5500_tcp_connect(&w, 1, 0xC0A8013Du, 502, 50002, 50));
    CHECK(w5500_tcp_ok(&w, 1));
    CHECK_EQ(chip.sreg[1][W5500_Sn_DIPR + 3], 0x3D);
    CHECK_EQ(chip.sreg[1][W5500_Sn_DPORT] << 8 | chip.sreg[1][W5500_Sn_DPORT + 1], 502);
    CHECK_EQ(chip.sreg[1][W5500_Sn_PORT] << 8 | chip.sreg[1][W5500_Sn_PORT + 1], 50002);
    CHECK_EQ(chip.sreg[1][W5500_Sn_MR], W5500_MR_TCP | W5500_MR_ND);
    /* 5 x 700 bytes: both 2 KB rings wrap, and every byte comes back in order */
    static uint8_t out[700], in[800];
    bool same = true;
    for (unsigned r = 0; r < 5; ++r) {
        for (unsigned k = 0; k < sizeof out; ++k) out[k] = (uint8_t)(r * 31u + k);
        CHECK(w5500_send(&w, 1, out, sizeof out, 20));
        CHECK_EQ(w5500_rx_avail(&w, 1), sizeof out);
        uint16_t got = w5500_recv(&w, 1, in, 300);         /* in two parts */
        got = (uint16_t)(got + w5500_recv(&w, 1, in + got, sizeof in));
        same &= got == sizeof out && memcmp(in, out, sizeof out) == 0;
        same &= memcmp(seen + r * sizeof out, out, sizeof out) == 0;
    }
    CHECK(same);
    CHECK_EQ(seen_n, 5u * sizeof out);
    CHECK_EQ(w5500_rx_avail(&w, 1), 0u);
    CHECK(!w5500_send(&w, 1, out, 0, 20));                  /* nothing to send */
    w5500_sim_peer_close(&chip, 1);                         /* the board went away */
    CHECK(!w5500_tcp_ok(&w, 1));
    CHECK(!w5500_send(&w, 1, out, 10, 20));
    w5500_close(&w, 1);
    CHECK_EQ(chip.sreg[1][W5500_Sn_SR], W5500_SOCK_CLOSED);
}

static void t_udp(void) {
    fresh();
    CHECK(w5500_init(&w, &chip.bus, MAC, 0xC0A8013Cu, 0xFFFFFF00u, 0));
    CHECK(w5500_udp_open(&w, 3, 50224));
    CHECK_EQ(chip.sreg[3][W5500_Sn_SR], W5500_SOCK_UDP);
    uint8_t m[IOB_DISC_LEN] = { 'C', 'E', 'M', 'S' };
    CHECK(w5500_udp_send(&w, 3, 0xFFFFFFFFu, IOB_DISC_PORT, m, sizeof m, 20));
    CHECK_EQ(u_ip, 0xFFFFFFFFu); CHECK_EQ(u_port, IOB_DISC_PORT); CHECK_EQ(u_n, IOB_DISC_LEN);
    uint8_t a[5] = { 1, 2, 3, 4, 5 }, b[3] = { 9, 8, 7 }, r[8];
    w5500_sim_udp_push(&chip, 3, 0xC0A8013Du, 50223, a, sizeof a);
    w5500_sim_udp_push(&chip, 3, 0xC0A8013Eu, 50223, b, sizeof b);
    uint32_t ip = 0; uint16_t port = 0;
    CHECK_EQ(w5500_udp_recv(&w, 3, r, 2, &ip, &port), 2u);  /* cut to fit; the rest of it is dropped */
    CHECK_EQ(ip, 0xC0A8013Du); CHECK_EQ(port, 50223); CHECK(r[0] == 1 && r[1] == 2);
    CHECK_EQ(w5500_udp_recv(&w, 3, r, sizeof r, &ip, NULL), 3u);
    CHECK_EQ(ip, 0xC0A8013Eu); CHECK(r[0] == 9 && r[2] == 7);
    CHECK_EQ(w5500_udp_recv(&w, 3, r, sizeof r, NULL, NULL), 0u);
    for (unsigned k = 0; k < 40; ++k) w5500_sim_udp_push(&chip, 3, 1, 2, m, sizeof m);   /* wraps the ring */
    unsigned got = 0;
    while (w5500_udp_recv(&w, 3, r, sizeof r, NULL, NULL)) ++got;
    CHECK_EQ(got, 2048u / (8u + IOB_DISC_LEN));            /* as many as fit; the rest lost, as on the wire */
}

/* the transport keeps the chip: set-up, missing chip, link, IP change */
static void t_upkeep(void) {
    bsp_host_reset();
    bsp_host.ms = 5000;
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    param_set_defaults(PARAM_ALL);
    fresh();
    chip.bus.ms = bsp_ms;
    iob_w5500_t t;
    iob_w5500_init(&t, &chip.bus, MAC);
    uint8_t tx[MBR_FRAME_MAX], rx[64];
    size_t n = mbr_req_read(tx, 1, 0, 2);
    chip.present = false;
    t.port.target(&t.port, 0xC0A8013Du);
    CHECK_EQ(t.port.xfer(&t.port, tx, (uint16_t)n, rx, sizeof rx, 15), 0u);
    CHECK_EQ(iob_port_status, 0u);
    chip.present = true;
    bsp_host_advance_ms(500);
    CHECK_EQ(t.port.xfer(&t.port, tx, (uint16_t)n, rx, sizeof rx, 15), 0u);   /* not tried again yet */
    CHECK(!t.chip.ok);
    bsp_host_advance_ms(600);
    (void)t.port.xfer(&t.port, tx, (uint16_t)n, rx, sizeof rx, 15);          /* a second later: found */
    CHECK(t.chip.ok);
    CHECK_EQ(iob_port_status, IOBP_FOUND | IOBP_LINK);
    CHECK_EQ(w5500_sim_ip(&chip), 0xC0A8013Cu);              /* iob_port_ip's default */
    CHECK_EQ(t.connects, 1u);
    chip.link = false;
    (void)t.port.xfer(&t.port, tx, (uint16_t)n, rx, sizeof rx, 15);
    CHECK_EQ(iob_port_status, IOBP_FOUND);
    chip.link = true;
    iob_port_ip = 0x0A0A0001u; iob_port_prefix = 16;        /* a new address: the chip takes it, connections again */
    (void)t.port.xfer(&t.port, tx, (uint16_t)n, rx, sizeof rx, 15);
    CHECK_EQ(w5500_sim_ip(&chip), 0x0A0A0001u);
    CHECK_EQ(chip.common[W5500_SUBR + 2], 0x00); CHECK_EQ(chip.common[W5500_SUBR + 1], 0xFF);
    CHECK_EQ(t.connects, 2u);
    CHECK_EQ(chip.resets, 1u);                               /* no reset for that */
    param_set_defaults(PARAM_ALL);
    /* the MAC: locally administered, unicast, and never the main port's */
    uint8_t uid[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }, m1[6], m2[6];
    iob_w5500_mac(uid, m1);
    uid[11] = 13;
    iob_w5500_mac(uid, m2);
    CHECK_EQ(m1[0], 0x02);
    CHECK(memcmp(m1, m2, 6) != 0);
    uint32_t a = plat_crc32(0, (const uint8_t[]){ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }, 12), b = plat_crc32(a, "CEMS", 4);
    CHECK_EQ(m1[5], (uint8_t)((uint8_t)b ^ 0x02u));         /* plat_net_mac()'s last byte, bit 1 flipped */
    CHECK_EQ(m1[1], (uint8_t)(a >> 24));
}

void test_w5500_all(void) { RUN(t_init); RUN(t_tcp); RUN(t_udp); RUN(t_upkeep); }
