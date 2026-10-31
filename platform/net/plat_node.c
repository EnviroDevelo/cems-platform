/* plat_node.c - see plat_node.h */
#include <string.h>
#include "plat_node.h"
#include "plat_net.h"
#include "plat_io.h"
#include "plat_diag.h"
#include "plat_port.h"
#include "bsp.h"

/* ---------------- the backup words: address, serial number, reset count, IP settings ---------------- */
enum { BK_ADDR = 0, BK_SERIAL = 1, BK_COUNT = 5, BK_IP = 6, BK_IPSUM = 10, BK_SERSUM = 11 };
#define ADDR_MAGIC 0xA0D50000u           /* BK_ADDR = magic | ~address << 8 | address */
#define CNT_MAGIC  0x52530000u           /* BK_COUNT = magic | resets since power-on */
#define IP_MAGIC   0x1B0E0000u           /* BK_IPSUM = magic | 16-bit sum of the four IP words */
#define SER_MAGIC  0x5E120000u           /* BK_SERSUM = magic | 16-bit sum of the serial words: else not set */

static uint32_t sum16(const node_ipcfg_t *c) {
    uint32_t w[4] = { c->ip, c->mask, c->gw, c->master_ip }, s = 0;
    for (unsigned k = 0; k < 4; ++k) s += (w[k] >> 16) + (w[k] & 0xFFFFu);
    return s & 0xFFFFu;
}

static void ip_load(node_ipcfg_t *c) {
    c->ip = bsp_backup_read(BK_IP); c->mask = bsp_backup_read(BK_IP + 1);
    c->gw = bsp_backup_read(BK_IP + 2); c->master_ip = bsp_backup_read(BK_IP + 3);
    if (bsp_backup_read(BK_IPSUM) != (IP_MAGIC | sum16(c))) memset(c, 0, sizeof *c);   /* never set, or power-off */
}

static int ip_store(void *ctx, const node_ipcfg_t *c) {
    (void)ctx;
    bool ok = bsp_backup_write(BK_IP, c->ip) && bsp_backup_write(BK_IP + 1, c->mask) &&
              bsp_backup_write(BK_IP + 2, c->gw) && bsp_backup_write(BK_IP + 3, c->master_ip) &&
              bsp_backup_write(BK_IPSUM, IP_MAGIC | sum16(c));
    node_ipcfg_t back;
    ip_load(&back);
    return ok && memcmp(&back, c, sizeof back) == 0 ? NODE_HW_OK : NODE_HW_READBACK;
}

static int addr_store(void *ctx, uint8_t a) {
    (void)ctx;
    return bsp_backup_write(BK_ADDR, ADDR_MAGIC | (uint32_t)(uint8_t)~a << 8 | a) ? NODE_HW_OK : NODE_HW_READBACK;
}

static uint32_t ser_sum(const uint32_t w[4]) {
    uint32_t s = 0;
    for (unsigned k = 0; k < 4; ++k) s += (w[k] >> 16) + (w[k] & 0xFFFFu);
    return SER_MAGIC | (s & 0xFFFFu);
}

static int serial_store(void *ctx, const uint8_t s[16]) {
    (void)ctx;
    bool ok = true;
    uint32_t w[4];
    for (unsigned k = 0; k < 4; ++k) {
        w[k] = (uint32_t)s[4 * k] | (uint32_t)s[4 * k + 1] << 8 | (uint32_t)s[4 * k + 2] << 16 | (uint32_t)s[4 * k + 3] << 24;
        ok = bsp_backup_write(BK_SERIAL + k, w[k]) && ok;
    }
    ok = bsp_backup_write(BK_SERSUM, ser_sum(w)) && ok;
    return ok ? NODE_HW_OK : NODE_HW_READBACK;
}

/* the serial number as saved; all zero (not set) unless its check word matches (whatever the
 * backup registers held before, e.g. another firmware's data) */
static void serial_load(uint8_t s[16]) {
    uint32_t w[4];
    for (unsigned k = 0; k < 4; ++k) w[k] = bsp_backup_read(BK_SERIAL + k);
    bool set = bsp_backup_read(BK_SERSUM) == ser_sum(w);
    for (unsigned i = 0; i < 16; ++i) s[i] = (uint8_t)(set ? w[i / 4u] >> (8u * (i % 4u)) : 0u);
}

/* the chip's unique ID as the node core's six words */
static void uid_words(uint16_t u[6]) {
    uint8_t uid[12];
    bsp_chip_uid(uid);
    for (unsigned i = 0; i < 6; ++i) u[i] = (uint16_t)(uid[2 * i] | uid[2 * i + 1] << 8);
}

void plat_node_mac(uint8_t mac[6]) {
    uint16_t u[6];
    uid_words(u);
    iob_mac_from_uid(u, mac);
}

/* ---------------- the board's pins (bsp_io_read / bsp_io_write) ---------------- */
static plat_io_out_t out;
static uint8_t  filter_ms = NODE_DEF_FILTER, stable, last_raw;
static uint32_t since[8];

static void put(unsigned first, unsigned n, uint8_t bits) {
    for (unsigned i = 0; i < n; ++i) plat_do_set(&out, first + i, (bits >> i) & 1u);
    bsp_io_write(&out);
}

static int a_config(void *ctx, uint8_t ol) { (void)ctx; (void)ol; return NODE_HW_OK; }
static int a_write(void *ctx, uint8_t o) { (void)ctx; put(0, 8, o); return NODE_HW_OK; }
static int a_diag(void *ctx, node_a_diag_t *d) { (void)ctx; *d = (node_a_diag_t){ .supply_ok = true }; return NODE_HW_OK; }
static int in_config(void *ctx, uint8_t f, uint8_t wb) { (void)ctx; (void)wb; filter_ms = f; return NODE_HW_OK; }

static int in_read(void *ctx, node_in_t *d) {          /* each input debounced over FILTER_MS, as the input chip would */
    (void)ctx;
    plat_io_in_t in;
    memset(&in, 0, sizeof in);
    bsp_io_read(&in);
    uint32_t now = bsp_ms();
    uint8_t raw = (uint8_t)in.di;
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t m = (uint8_t)(1u << i);
        if ((raw ^ last_raw) & m) since[i] = now;       /* it moved: start again */
        if (((raw ^ stable) & m) && (uint32_t)(now - since[i]) >= filter_ms) stable ^= m;
    }
    last_raw = raw;
    *d = (node_in_t){ .states = stable, .supply_ok = true };
    return NODE_HW_OK;
}

static void b_write(void *ctx, uint8_t o) { (void)ctx; put(8, 8, o); }
static bool b_false(void *ctx) { (void)ctx; return false; }
static bool b_true(void *ctx) { (void)ctx; return true; }
static void b_nop(void *ctx) { (void)ctx; }
static void relays(void *ctx, uint8_t r) { (void)ctx; put(16, 2, r); }

static const node_hw_t hw = {
    .a_config = a_config, .a_write = a_write, .a_diag = a_diag,
    .in_config = in_config, .in_read = in_read,
    .b_write = b_write, .b_fault_pin = b_false, .b_wdg_tripped = b_false, .b_wdg_service = b_nop,
    .b_supply_ok = b_true, .relays = relays, .addr_store = addr_store, .serial_store = serial_store,
};

__attribute__((weak)) const node_hw_t *bsp_node_chips(void) { return NULL; }
__attribute__((weak)) void bsp_node_pass(bool healthy, bool gate) { (void)healthy; (void)gate; }

static node_hw_t chips;                  /* the board's chips, with this file's address and serial store */
static const node_hw_t *in_use = &hw;

const node_hw_t *plat_node_hw(void) { return in_use; }

static void pass(plat_node_t *p, uint32_t now) {
    node_poll(&p->node, now);
    bsp_node_pass(node_healthy(&p->node), p->node.gate);
}

void plat_node_idle(plat_node_t *p, uint32_t now) { pass(p, now); }

/* ---------------- start-up ---------------- */
static node_reset_t reset_cause(void) {
    switch (plat_diag.last_reset_cause) {
    case BSP_RESET_IWDG: case BSP_RESET_WWDG: return NODE_RST_IWDG;
    case BSP_RESET_SOFTWARE: return NODE_RST_SOFTWARE;
    case BSP_RESET_PIN: return NODE_RST_PIN;
    case BSP_RESET_POWER_ON: case BSP_RESET_BROWNOUT: return NODE_RST_POWER_ON;
    default: return NODE_RST_OTHER;
    }
}

static plat_node_t *the_node;            /* for plat_node_ipcfg */

void plat_node_init(plat_node_t *p, uint32_t now) {
    memset(p, 0, sizeof *p);
    p->lfd = p->cfd = p->ufd = -1;
    p->tcp_port = IOB_TCP_PORT;
    p->disc_port = IOB_DISC_PORT;
    p->disc_to = 0xFFFFFFFFu;
    memset(&out, 0, sizeof out);
    bsp_io_write(&out);
    const node_hw_t *board = bsp_node_chips();
    if (board) {
        chips = *board;
        chips.addr_store = addr_store;
        chips.serial_store = serial_store;
        in_use = &chips;
    } else
        in_use = &hw;

    node_boot_t b;
    memset(&b, 0, sizeof b);
    b.reset_cause = reset_cause();
    uint32_t c = bsp_backup_read(BK_COUNT);
    b.resets = (b.reset_cause == NODE_RST_POWER_ON || (c & 0xFFFF0000u) != CNT_MAGIC) ? 0u : (uint16_t)(c + 1u);
    (void)bsp_backup_write(BK_COUNT, CNT_MAGIC | b.resets);
    uint32_t a = bsp_backup_read(BK_ADDR);
    bool set = (a & 0xFFFF0000u) == ADDR_MAGIC && (uint8_t)(a >> 8) == (uint8_t)~a;
    b.address = set ? (uint8_t)a : (uint8_t)PLAT_NODE_ADDRESS;
    b.nv_ok = true;
    serial_load(b.serial);
    uint8_t rev;
    b.hw_rev = bsp_hw_straps(&rev) ? rev : (uint8_t)PLAT_NODE_HW_REV;
    uid_words(b.uid);
    node_init(&p->node, in_use, &b, now);

    node_ipcfg_t ipc;
    ip_load(&ipc);
    uint8_t mac[6];
    plat_node_mac(mac);
    node_net_init(&p->net, &p->node, &ipc, mac, ip_store, NULL);
#ifdef PLAT_NODE_DEFAULT_IP
    if (p->net.cfg.ip == 0) {            /* nothing saved: the bench board's IP, any supervisor */
        p->net.cfg.ip = PLAT_NODE_DEFAULT_IP;
        p->net.cfg.mask = PLAT_NODE_DEFAULT_MASK;
    }
#endif
    the_node = p;
}

bool plat_node_ipcfg(node_ipcfg_t *c) {
    if (!the_node) return false;
    PLAT_CRIT_ENTER();
    *c = the_node->net.cfg;
    PLAT_CRIT_EXIT();
    return true;
}

/* ---------------- sockets ---------------- */
static void nonblock(int s) { int fl = plat_fcntl(s, F_GETFL, 0); plat_fcntl(s, F_SETFL, fl | O_NONBLOCK); }

static int open_sock(int type, uint16_t port) {
    int s = socket(AF_INET, type, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (type == SOCK_DGRAM) setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || (type == SOCK_STREAM && listen(s, 2) != 0)) {
        closesocket(s);
        return -1;
    }
    nonblock(s);
    return s;
}

static void drop_conn(plat_node_t *p) {
    if (p->cfd >= 0) { closesocket(p->cfd); p->dropped++; }
    p->cfd = -1;
}

void plat_node_close(plat_node_t *p) {
    drop_conn(p);
    if (p->lfd >= 0) closesocket(p->lfd);
    if (p->ufd >= 0) closesocket(p->ufd);
    p->lfd = p->ufd = -1;
}

static void take_conn(plat_node_t *p, uint32_t now) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int s = accept(p->lfd, (struct sockaddr *)&a, &al);
    if (s < 0) return;
    (void)now;
    if (!node_net_accept(&p->net, ntohl(a.sin_addr.s_addr))) { closesocket(s); return; }   /* not its supervisor */
    drop_conn(p);                        /* one at a time: the new one wins (the supervisor restarted) */
    nonblock(s);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    p->cfd = s;
    p->accepted++;
}

static bool send_all(int fd, const uint8_t *b, size_t n) {
    for (unsigned tries = 0; n && tries < 50u; ) {
        ssize_t s = send(fd, b, n, 0);
        if (s > 0) { b += s; n -= (size_t)s; continue; }
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
            struct timeval tv = { 0, 2000 };
            (void)select(fd + 1, NULL, &w, NULL, &tv);
            tries++;
            continue;
        }
        return false;
    }
    return n == 0;
}

static void serve_conn(plat_node_t *p, uint32_t now) {
    uint8_t in[IOB_MBAP_MAX], tx[2u * IOB_MBAP_MAX];
    ssize_t r = recv(p->cfd, in, sizeof in, 0);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (r <= 0) { drop_conn(p); return; }           /* closed by the supervisor, or an error */
    int o = node_net_tcp(&p->net, now, in, (uint16_t)r, tx, sizeof tx);
    if (o < 0 || (o > 0 && !send_all(p->cfd, tx, (size_t)o))) drop_conn(p);
}

static void serve_disc(plat_node_t *p, uint32_t now) {
    uint8_t d[IOB_DISC_LEN + 1], rep[IOB_DISC_LEN];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t r = recvfrom(p->ufd, d, sizeof d, 0, (struct sockaddr *)&from, &fl);
    if (r <= 0) return;
    bool changed = false;
    PLAT_CRIT_ENTER();                               /* the net task reads the settings */
    uint16_t rl = node_net_disc(&p->net, now, d, (uint16_t)r, ntohl(from.sin_addr.s_addr), rep, &changed);
    PLAT_CRIT_EXIT();
    if (rl) {
        struct sockaddr_in to = from;                /* to the port it came from, at the broadcast address */
        to.sin_addr.s_addr = htonl(p->disc_to);
        (void)sendto(p->ufd, rep, rl, 0, (struct sockaddr *)&to, sizeof to);
    }
    if (changed) drop_conn(p);                       /* the board changes over to its new IP (the net task applies it) */
}

void plat_node_poll(plat_node_t *p, uint32_t now, unsigned wait_ms) {
    if (p->lfd < 0) p->lfd = open_sock(SOCK_STREAM, p->tcp_port);
    if (p->ufd < 0) p->ufd = open_sock(SOCK_DGRAM, p->disc_port);
    if (!node_net_serving(&p->net)) drop_conn(p);    /* a new board: discovery only */
    fd_set rd;
    FD_ZERO(&rd);
    int top = -1;
    if (p->lfd >= 0) { FD_SET(p->lfd, &rd); top = p->lfd; }
    if (p->ufd >= 0) { FD_SET(p->ufd, &rd); if (p->ufd > top) top = p->ufd; }
    if (p->cfd >= 0) { FD_SET(p->cfd, &rd); if (p->cfd > top) top = p->cfd; }
    struct timeval tv = { 0, (long)wait_ms * 1000 };
    if (top >= 0 && select(top + 1, &rd, NULL, NULL, &tv) > 0) {
        now = bsp_ms();
        if (p->lfd >= 0 && FD_ISSET(p->lfd, &rd)) take_conn(p, now);
        else if (p->cfd >= 0 && FD_ISSET(p->cfd, &rd)) serve_conn(p, now);
        if (p->ufd >= 0 && FD_ISSET(p->ufd, &rd)) serve_disc(p, now);
    }
    PLAT_CRIT_ENTER();
    bool cleared = node_net_poll(&p->net);           /* the address cleared over USB: a new board again */
    PLAT_CRIT_EXIT();
    if (cleared) drop_conn(p);
    pass(p, bsp_ms());
}
