/* test_iobtcp.c - the Ethernet transport of the I/O bus (iob_tcp.c) over real sockets on the
 * loopback: a simulated board (iob_node_sim) serves Modbus TCP and discovery in a child process. */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include "tst.h"
#include "iob_tcp.h"
#include "iob_node_sim.h"
#include "mb_rtu.h"
#include "plat_net.h"

#define LOOP 0x7F000001u

/* the board: one connection at a time, a new one replaces the old; 'garbage' answers rubbish once */
static void board(uint16_t tport, uint16_t dport, int ready) {
    iob_node_t n; iob_node_init(&n, 1);
    memcpy(n.serial, "CIO-TEST", 8);
    int one = 1;
    int l = socket(AF_INET, SOCK_STREAM, 0), u = socket(AF_INET, SOCK_DGRAM, 0), c = -1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(LOOP);
    a.sin_port = htons(tport);
    if (bind(l, (struct sockaddr *)&a, sizeof a) || listen(l, 2)) _exit(2);
    a.sin_port = htons(dport);
    if (bind(u, (struct sockaddr *)&a, sizeof a)) _exit(2);
    if (write(ready, "r", 1) != 1) _exit(2);
    uint8_t buf[600]; size_t have = 0;
    for (;;) {
        fd_set rd; FD_ZERO(&rd); FD_SET(l, &rd); FD_SET(u, &rd); if (c >= 0) FD_SET(c, &rd);
        int mx = l > u ? l : u; if (c > mx) mx = c;
        if (select(mx + 1, &rd, NULL, NULL, NULL) <= 0) continue;
        if (FD_ISSET(l, &rd)) { if (c >= 0) close(c); c = accept(l, NULL, NULL); have = 0; }
        if (FD_ISSET(u, &rd)) {
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t r = recvfrom(u, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            iob_disc_t m;
            if (r > 0 && iob_disc_decode(buf, (size_t)r, &m) && (m.op == IOB_DISC_FIND || iob_disc_set_accept(&m, n.uid, n.master_ip, LOOP))) {
                if (m.op == IOB_DISC_SET) { n.ip = m.ip; n.mask = m.mask; n.gw = m.gw; n.master_ip = m.master_ip; n.address = m.address; }
                iob_disc_t h; memset(&h, 0, sizeof h);
                h.op = IOB_DISC_HERE; h.seq = m.seq; memcpy(h.uid, n.uid, 12); memcpy(h.mac, n.mac, 6);
                memcpy(h.serial, n.serial, 8); h.ip = n.ip; h.mask = n.mask; h.master_ip = n.master_ip; h.address = n.address;
                uint8_t out[IOB_DISC_LEN]; iob_disc_encode(&h, out);
                sendto(u, out, sizeof out, 0, (struct sockaddr *)&from, fl);   /* back to the request's port */
            }
        }
        if (c >= 0 && FD_ISSET(c, &rd)) {
            ssize_t r = recv(c, buf + have, sizeof buf - have, 0);
            if (r <= 0) { close(c); c = -1; continue; }
            have += (size_t)r;
            int k;
            while ((k = iob_mbap_complete(buf, have)) > 0) {
                uint8_t req[IOB_MBAP_MAX], rep[IOB_MBAP_MAX], out[IOB_MBAP_MAX]; uint16_t tid;
                size_t rl = iob_rtu_from_mbap(buf, (size_t)k, &tid, req);
                memmove(buf, buf + k, have - (size_t)k); have -= (size_t)k;
                if (rl >= 4 && req[1] == 0x06 && req[2] == 0x00 && req[3] == 0x63) {   /* test hook: write 99 */
                    if (req[5] == 1) { send(c, "\x00\x01\x00\x07\x00\x00\x00", 7, 0); continue; }   /* not Modbus */
                    if (req[5] == 2) continue;                                               /* no answer */
                }
                uint16_t rn = rl ? iob_node_frame(&n, 0, req, (uint16_t)rl, rep, sizeof rep) : 0;
                size_t ol = rn ? iob_mbap_from_rtu(rep, rn, tid, out) : 0;
                if (ol) send(c, out, ol, 0);
            }
            if (k < 0) { close(c); c = -1; }
        }
    }
}

static pid_t child;
static void start(uint16_t tp, uint16_t dp) {
    int p[2]; CHECK_EQ(pipe(p), 0);
    child = fork();
    if (child == 0) { close(p[0]); board(tp, dp, p[1]); _exit(0); }
    char r; close(p[1]); CHECK_EQ(read(p[0], &r, 1), 1); close(p[0]);
}
static void stop(void) { kill(child, SIGKILL); waitpid(child, NULL, 0); }

static int ident(iob_tcp_t *t, uint8_t addr) {
    uint8_t f[MBR_FRAME_MAX], rx[MBR_FRAME_MAX]; uint16_t r[IOB_R_IDENT_N];
    size_t n = mbr_req_read(f, addr, IOB_R_MAGIC, IOB_R_IDENT_N);
    uint16_t got = t->port.xfer(&t->port, f, (uint16_t)n, rx, sizeof rx, 20);
    int rc = mbr_check_reply(f, n, rx, got, r, IOB_R_IDENT_N, NULL);
    return rc == MBR_OK && r[0] != IOB_MAGIC ? -1 : rc;
}
static int hook(iob_tcp_t *t, uint16_t v) {
    uint8_t f[MBR_FRAME_MAX], rx[MBR_FRAME_MAX];
    size_t n = mbr_req_write1(f, 1, 99, v);
    return t->port.xfer(&t->port, f, (uint16_t)n, rx, sizeof rx, 20);
}

static void t_tcp_link(void) {
    uint16_t tp = (uint16_t)(20000u + (unsigned)getpid() % 20000u), dp = (uint16_t)(tp + 1u);
    signal(SIGPIPE, SIG_IGN);
    start(tp, dp);
    iob_tcp_t t; iob_tcp_init(&t);
    t.tcp_port = tp; t.disc_port = dp; t.disc_to = LOOP;
    CHECK_EQ(ident(&t, 1), MBR_NO_REPLY);           /* no target yet */
    t.port.target(&t.port, LOOP);
    CHECK_EQ(ident(&t, 1), MBR_OK);
    CHECK_EQ(ident(&t, 1), MBR_OK);
    CHECK_EQ(t.connects, 1u);                       /* one connection, kept */
    CHECK_EQ(ident(&t, 2), MBR_NO_REPLY);           /* another unit id: the board does not answer */
    CHECK_EQ(hook(&t, 2), 0u);                      /* a lost reply keeps the connection */
    CHECK_EQ(ident(&t, 1), MBR_OK);
    CHECK_EQ(t.connects, 1u);
    CHECK_EQ(hook(&t, 1), 0u);                      /* not Modbus TCP: closed, opened again next time */
    CHECK_EQ(ident(&t, 1), MBR_OK);
    CHECK_EQ(t.connects, 2u);

    /* discovery */
    iob_disc_t m; memset(&m, 0, sizeof m); m.op = IOB_DISC_FIND; m.seq = 5;
    uint8_t b[IOB_DISC_LEN]; iob_disc_encode(&m, b);
    CHECK(t.disc.send(&t.disc, b, IOB_DISC_LEN));
    uint16_t got = t.disc.recv(&t.disc, b, sizeof b, 500);
    iob_disc_t h;
    CHECK_EQ(got, IOB_DISC_LEN);
    CHECK(iob_disc_decode(b, got, &h));
    CHECK_EQ(h.op, IOB_DISC_HERE); CHECK_EQ(h.seq, 5u);
    CHECK(!strcmp(h.serial, "CIO-TEST"));
    CHECK_EQ(t.disc.recv(&t.disc, b, sizeof b, 30), 0u);   /* nothing else */

    /* the board restarts: the next request reconnects */
    stop();
    CHECK_EQ(ident(&t, 1), MBR_NO_REPLY);
    CHECK_EQ(ident(&t, 1), MBR_NO_REPLY);           /* connection refused */
    start(tp, dp);
    CHECK_EQ(ident(&t, 1), MBR_OK);
    CHECK(t.failures >= 1u);
    stop();
    iob_tcp_close(&t);
}

void test_iobtcp_all(void) {
    RUN(t_tcp_link);
}
