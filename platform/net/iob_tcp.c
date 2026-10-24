/* iob_tcp.c - see iob_tcp.h */
#include <string.h>
#include "iob_tcp.h"
#include "plat_config.h"
#include "plat_net.h"

static void tv_ms(struct timeval *tv, uint32_t ms) { tv->tv_sec = (long)(ms / 1000u); tv->tv_usec = (long)(ms % 1000u) * 1000; }

static int tcp_connect(uint32_t ip, uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    int fl = plat_fcntl(s, F_GETFL, 0);
    plat_fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, (struct sockaddr *)&a, sizeof a);
    if (rc != 0 && errno != EINPROGRESS) { closesocket(s); return -1; }
    if (rc != 0) {
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv; tv_ms(&tv, PLAT_IOB_TCP_CONNECT_MS);
        int err = 0; socklen_t el = sizeof err;
        if (select(s + 1, NULL, &w, NULL, &tv) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
            closesocket(s); return -1;
        }
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;                                       /* stays non-blocking: every wait is a select */
}

static void drop(iob_tcp_t *t, unsigned k) {
    if (t->conn[k].fd >= 0) closesocket(t->conn[k].fd);
    t->conn[k].fd = -1;
    t->conn[k].ip = 0;
}

/* the connection to ip: the one open, else a free slot, else the least recently used one */
static int conn_for(iob_tcp_t *t, uint32_t ip) {
    unsigned pick = 0;
    for (unsigned k = 0; k < IOB_BOARDS; ++k) {
        if (t->conn[k].fd >= 0 && t->conn[k].ip == ip) { t->conn[k].used = ++t->use; return t->conn[k].fd; }
        if (t->conn[pick].fd >= 0 && (t->conn[k].fd < 0 || t->conn[k].used < t->conn[pick].used)) pick = k;
    }
    drop(t, pick);
    int fd = tcp_connect(ip, t->tcp_port);
    if (fd < 0) { t->failures++; return -1; }
    t->connects++;
    t->conn[pick].fd = fd; t->conn[pick].ip = ip; t->conn[pick].used = ++t->use;
    return fd;
}

static bool send_all(int fd, const uint8_t *b, size_t n) {
    while (n) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
        struct timeval tv; tv_ms(&tv, PLAT_IOB_TCP_REPLY_MS);
        if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) return false;
        ssize_t s = send(fd, b, n, 0);
        if (s <= 0) return false;
        b += s; n -= (size_t)s;
    }
    return true;
}

static uint16_t xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t timeout) {
    iob_tcp_t *t = (iob_tcp_t *)p->ctx;
    uint8_t buf[2 * IOB_MBAP_MAX];
    if (t->target == 0) return 0;
    uint16_t tid = ++t->tid;
    size_t m = iob_mbap_from_rtu(tx, n, tid, buf);
    if (!m) return 0;
    unsigned k;
    int fd = conn_for(t, t->target);
    if (fd < 0) return 0;
    for (k = 0; k < IOB_BOARDS && t->conn[k].fd != fd; ++k) { }
    for (;;) {                                      /* a late reply to an earlier request: drop it */
        ssize_t r = recv(fd, buf + m, sizeof buf - m, 0);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { drop(t, k); return 0; }
        if (r < 0) break;
    }
    if (!send_all(fd, buf, m)) { drop(t, k); return 0; }
    if (timeout < PLAT_IOB_TCP_REPLY_MS) timeout = PLAT_IOB_TCP_REPLY_MS;
    size_t have = 0;
    for (;;) {
        fd_set rd; FD_ZERO(&rd); FD_SET(fd, &rd);
        struct timeval tv; tv_ms(&tv, timeout);     /* each wait gets the whole time: replies are small */
        if (select(fd + 1, &rd, NULL, NULL, &tv) <= 0) return 0;          /* no reply: keep the connection */
        ssize_t r = recv(fd, buf + have, sizeof buf - have, 0);
        if (r <= 0) { drop(t, k); return 0; }
        have += (size_t)r;
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
        if (c < 0) { drop(t, k); return 0; }        /* not Modbus TCP */
    }
}

static void target(iob_port_t *p, uint32_t ip) { ((iob_tcp_t *)p->ctx)->target = ip; }

static int udp_open(iob_tcp_t *t) {
    if (t->udp >= 0) return t->udp;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);          /* any port: the boards answer to the port a request came from */
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return -1; }
    int fl = plat_fcntl(s, F_GETFL, 0);
    plat_fcntl(s, F_SETFL, fl | O_NONBLOCK);
    t->udp = s;
    return s;
}

static bool dsend(iob_dgram_t *d, const uint8_t *b, uint16_t n) {
    iob_tcp_t *t = (iob_tcp_t *)d->ctx;
    int s = udp_open(t);
    if (s < 0) return false;
    uint8_t junk[IOB_DISC_LEN];
    while (recv(s, junk, sizeof junk, 0) > 0) { }  /* answers to an earlier request */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(t->disc_port);
    a.sin_addr.s_addr = htonl(t->disc_to);
    return sendto(s, b, n, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)n;
}

static uint16_t drecv(iob_dgram_t *d, uint8_t *b, uint16_t cap, uint32_t timeout) {
    iob_tcp_t *t = (iob_tcp_t *)d->ctx;
    if (t->udp < 0) return 0;
    fd_set rd; FD_ZERO(&rd); FD_SET(t->udp, &rd);
    struct timeval tv; tv_ms(&tv, timeout);
    if (select(t->udp + 1, &rd, NULL, NULL, &tv) <= 0) return 0;
    ssize_t r = recv(t->udp, b, cap, 0);
    return r > 0 ? (uint16_t)r : 0;
}

void iob_tcp_init(iob_tcp_t *t) {
    memset(t, 0, sizeof *t);
    t->port.xfer = xfer;
    t->port.target = target;
    t->port.ctx = t;
    t->disc.send = dsend;
    t->disc.recv = drecv;
    t->disc.ctx = t;
    t->tcp_port = IOB_TCP_PORT;
    t->disc_port = IOB_DISC_PORT;
    t->disc_to = 0xFFFFFFFFu;
    t->udp = -1;
    for (unsigned k = 0; k < IOB_BOARDS; ++k) t->conn[k].fd = -1;
}

void iob_tcp_close(iob_tcp_t *t) {
    for (unsigned k = 0; k < IOB_BOARDS; ++k) drop(t, k);
    if (t->udp >= 0) closesocket(t->udp);
    t->udp = -1;
}
