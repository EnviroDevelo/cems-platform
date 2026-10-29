/* iob_w5500.c - see iob_w5500.h */
#include <string.h>
#include "iob_w5500.h"
#include "plat_selftest.h"
#include "iob_net.h"
#include "plat_config.h"
#include "plat_port.h"
#include "nvm.h"            /* plat_crc32 */

#define LPORT_FIRST 49152u          /* each new connection from its own local port, so a board never
                                       mistakes it for the one it had before */
#define DISC_LPORT  50224u          /* the boards answer to the port a request came from */
#define RETRY_MS    1000u           /* a chip that does not answer is tried again this often */

void iob_w5500_mac(const uint8_t uid[12], uint8_t mac[6]) {
    uint32_t a = plat_crc32(0, uid, 12);            /* as plat_net_mac() for the main port */
    uint32_t b = plat_crc32(a, "CEMS", 4);
    mac[0] = 0x02;
    mac[1] = (uint8_t)(a >> 24); mac[2] = (uint8_t)(a >> 16); mac[3] = (uint8_t)(a >> 8);
    mac[4] = (uint8_t)a ^ (uint8_t)(b >> 24); mac[5] = (uint8_t)((uint8_t)b ^ 0x02u);
}

static uint32_t ms(iob_w5500_t *t) { return t->bus->ms ? t->bus->ms() : 0u; }

static void forget_all(iob_w5500_t *t) {
    for (unsigned k = 0; k < IOB_BOARDS; ++k) t->conn[k].open = false;
    t->udp = false;
}

/* the chip set up with the current settings; false while it does not answer */
static bool ready(iob_w5500_t *t) {
    uint32_t ip, mask;
    PLAT_CRIT_ENTER(); ip = iob_port_ip; mask = IOB_PORT_MASK(iob_port_prefix); PLAT_CRIT_EXIT();
    if (!t->chip.ok) {
        uint32_t now = ms(t);
        if (t->tried && (uint32_t)(now - t->init_at) < RETRY_MS) { iob_port_status = 0; return false; }
        t->tried = true;
        t->init_at = now;
        forget_all(t);
        bool up = w5500_init(&t->chip, t->bus, t->mac, ip, mask, 0);
        plat_selftest_report(PLAT_STEST_IO_PORT, up);      /* CIP-11.1 (the last try before the list closes counts) */
        if (!up) { iob_port_status = 0; return false; }
    } else if (ip != t->chip.ip || mask != t->chip.mask) {   /* a new address: every connection again */
        for (uint8_t s = 0; s < W5500_SOCKETS; ++s) w5500_close(&t->chip, s);
        forget_all(t);
        (void)w5500_set_addr(&t->chip, ip, mask, 0);
    }
    bool link = w5500_link_up(&t->chip);
    iob_port_status = (uint8_t)((t->chip.ok ? IOBP_FOUND : 0u) | (link ? IOBP_LINK : 0u));
    return t->chip.ok;
}

bool iob_w5500_ready(iob_w5500_t *t) { return t->bus && ready(t); }

static void drop(iob_w5500_t *t, unsigned k) {
    if (t->conn[k].open) w5500_close(&t->chip, (uint8_t)k);
    t->conn[k].open = false;
    t->conn[k].ip = 0;
}

/* the socket for ip: the one open, else a free one, else the least recently used one (-1: none) */
static int conn_for(iob_w5500_t *t, uint32_t ip) {
    unsigned pick = 0;
    for (unsigned k = 0; k < IOB_BOARDS; ++k) {
        if (t->conn[k].open && t->conn[k].ip == ip) {
            if (w5500_tcp_ok(&t->chip, (uint8_t)k)) { t->conn[k].used = ++t->use; return (int)k; }
            drop(t, k);             /* the board closed it (restarted): open it again below */
        }
        if (t->conn[pick].open && (!t->conn[k].open || t->conn[k].used < t->conn[pick].used)) pick = k;
    }
    drop(t, pick);
    t->lport = (uint16_t)(t->lport < LPORT_FIRST || t->lport == 0xFFFFu ? LPORT_FIRST : t->lport + 1u);
    if (!w5500_tcp_connect(&t->chip, (uint8_t)pick, ip, t->tcp_port, t->lport, PLAT_IOB_TCP_CONNECT_MS)) {
        t->failures++;
        return -1;
    }
    t->connects++;
    t->conn[pick].open = true; t->conn[pick].ip = ip; t->conn[pick].used = ++t->use;
    return (int)pick;
}

static uint16_t xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t timeout) {
    iob_w5500_t *t = (iob_w5500_t *)p->ctx;
    uint8_t buf[2 * IOB_MBAP_MAX];
    if (t->target == 0 || !ready(t)) return 0;
    uint16_t tid = ++t->tid;
    size_t m = iob_mbap_from_rtu(tx, n, tid, buf);
    if (!m) return 0;
    int k = conn_for(t, t->target);
    if (k < 0) return 0;
    uint8_t s = (uint8_t)k;
    while (w5500_recv(&t->chip, s, buf + m, (uint16_t)(sizeof buf - m)) > 0) { }   /* a late reply: drop it */
    if (!w5500_send(&t->chip, s, buf, (uint16_t)m, PLAT_IOB_TCP_REPLY_MS)) { drop(t, (unsigned)k); return 0; }
    if (timeout < PLAT_IOB_TCP_REPLY_MS) timeout = PLAT_IOB_TCP_REPLY_MS;
    size_t have = 0;
    uint32_t t0 = ms(t);
    for (unsigned spins = 0;; ++spins) {
        uint16_t r = w5500_recv(&t->chip, s, buf + have, (uint16_t)(sizeof buf - have));
        if (r) {
            have += r;
            int c;
            while ((c = iob_mbap_complete(buf, have)) > 0) {
                uint8_t out[IOB_MBAP_MAX];
                uint16_t got_tid;
                size_t ol = iob_rtu_from_mbap(buf, (size_t)c, &got_tid, out);
                memmove(buf, buf + c, have - (size_t)c);
                have -= (size_t)c;
                if (got_tid != tid) continue;           /* an old one */
                if (!ol || ol > cap) return 0;
                memcpy(rx, out, ol);
                return (uint16_t)ol;
            }
            if (c < 0 || have >= sizeof buf) { drop(t, (unsigned)k); return 0; }   /* not Modbus TCP */
            continue;
        }
        if (!w5500_tcp_ok(&t->chip, s)) { drop(t, (unsigned)k); return 0; }
        if ((t->bus->ms && (uint32_t)(ms(t) - t0) >= timeout) || spins > timeout + 2000u) return 0;   /* no reply: keep the connection */
        if (t->bus->wait) t->bus->wait(t->bus, 1);
    }
}

static void target(iob_port_t *p, uint32_t ip) { ((iob_w5500_t *)p->ctx)->target = ip; }

static bool dsend(iob_dgram_t *d, const uint8_t *b, uint16_t n) {
    iob_w5500_t *t = (iob_w5500_t *)d->ctx;
    if (!ready(t)) return false;
    if (!t->udp && !(t->udp = w5500_udp_open(&t->chip, IOB_W5500_DISC_SOCK, DISC_LPORT))) return false;
    uint8_t junk[IOB_DISC_LEN];
    while (w5500_udp_recv(&t->chip, IOB_W5500_DISC_SOCK, junk, sizeof junk, NULL, NULL) > 0) { }   /* earlier answers */
    if (w5500_udp_send(&t->chip, IOB_W5500_DISC_SOCK, t->disc_to, t->disc_port, b, n, PLAT_IOB_TCP_REPLY_MS)) return true;
    t->udp = false;
    return false;
}

static uint16_t drecv(iob_dgram_t *d, uint8_t *b, uint16_t cap, uint32_t timeout) {
    iob_w5500_t *t = (iob_w5500_t *)d->ctx;
    if (!t->udp || !t->chip.ok) return 0;
    uint32_t t0 = ms(t);
    for (unsigned spins = 0;; ++spins) {
        uint16_t r = w5500_udp_recv(&t->chip, IOB_W5500_DISC_SOCK, b, cap, NULL, NULL);
        if (r) return r;
        if ((t->bus->ms && (uint32_t)(ms(t) - t0) >= timeout) || spins > timeout + 2000u) return 0;
        if (t->bus->wait) t->bus->wait(t->bus, 1);
    }
}

void iob_w5500_init(iob_w5500_t *t, w5500_bus_t *bus, const uint8_t mac[6]) {
    memset(t, 0, sizeof *t);
    t->bus = bus;
    memcpy(t->mac, mac, 6);
    t->port.xfer = xfer;
    t->port.target = target;
    t->port.ctx = t;
    t->disc.send = dsend;
    t->disc.recv = drecv;
    t->disc.ctx = t;
    t->tcp_port = IOB_TCP_PORT;
    t->disc_port = IOB_DISC_PORT;
    t->disc_to = 0xFFFFFFFFu;
}
