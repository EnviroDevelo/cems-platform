/* mbtcp.c - see mbtcp.h */
#include <string.h>
#include "mbtcp.h"
#include "modbus.h"
#include "param.h"
#include "plat_net.h"
#include "bsp.h"

int mbtcp_feed(mbtcp_rx_t *rx, const uint8_t *in, uint16_t len, uint16_t *used, uint8_t *out, uint16_t cap) {
    int produced = 0;
    *used = 0;
    for (;;) {
        /* stop before starting a frame whose answer might not fit; the caller sends and calls again */
        if (rx->n == 0 && (uint32_t)produced + MBTCP_FRAME_MAX > cap) return produced;
        uint16_t need;
        if (rx->n < 7) need = (uint16_t)(7u - rx->n);
        else need = (uint16_t)(6u + ((rx->buf[4] << 8) | rx->buf[5]) - rx->n);
        uint16_t take = (uint16_t)(len - *used) < need ? (uint16_t)(len - *used) : need;
        memcpy(rx->buf + rx->n, in + *used, take);
        rx->n = (uint16_t)(rx->n + take);
        *used = (uint16_t)(*used + take);
        if (rx->n < 7) return produced;                                  /* need more input */
        uint16_t proto = (uint16_t)((rx->buf[2] << 8) | rx->buf[3]);
        uint16_t flen  = (uint16_t)((rx->buf[4] << 8) | rx->buf[5]);     /* unit id + PDU */
        if (proto != 0 || flen < 2 || flen > MBTCP_FRAME_MAX - 6u) return -1;
        if (rx->n < 6u + flen) { if (*used == len) return produced; continue; }
        uint8_t *o = out + produced;                                     /* one complete request */
        uint16_t plen = mb_pdu(rx->buf + 7, (uint16_t)(flen - 1u), o + 7, (uint16_t)(MBTCP_FRAME_MAX - 7u));
        if (plen) {
            memcpy(o, rx->buf, 4);                                       /* transaction + protocol id */
            o[4] = (uint8_t)((plen + 1u) >> 8); o[5] = (uint8_t)(plen + 1u);
            o[6] = rx->buf[6];                                           /* unit id echoed */
            produced += 7 + plen;
        }
        rx->n = 0;
        if (*used == len) return produced;
    }
}

int mbtcp_open(mbtcp_server_t *s, uint16_t port) {
    memset(s, 0, sizeof *s);
    for (int k = 0; k < MBTCP_MAX_CLIENTS; ++k) s->c[k].fd = -1;
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lfd < 0) return -1;
    int one = 1;
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(s->lfd, 2) != 0) {
        closesocket(s->lfd); s->lfd = -1; return -1;
    }
    return 0;
}

static void drop(mbtcp_server_t *s, mbtcp_client_t *c) {
    if (c->fd >= 0) { closesocket(c->fd); s->closed++; }
    c->fd = -1; c->rx.n = 0;
}

int mbtcp_clients(const mbtcp_server_t *s) {
    int n = 0;
    for (int k = 0; k < MBTCP_MAX_CLIENTS; ++k) if (s->c[k].fd >= 0) n++;
    return n;
}

void mbtcp_close_all(mbtcp_server_t *s) {
    for (int k = 0; k < MBTCP_MAX_CLIENTS; ++k) drop(s, &s->c[k]);
    if (s->lfd >= 0) closesocket(s->lfd);
    s->lfd = -1;
}

static void accept_one(mbtcp_server_t *s, uint32_t now) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int fd = accept(s->lfd, (struct sockaddr *)&a, &al);
    if (fd < 0) return;
    uint32_t ip = ntohl(a.sin_addr.s_addr);
    plat_val_t allow;
    if (param_read(PARAM_ID_MODBUS_ALLOW_IP, &allow) && allow.u != 0 && allow.u != ip) {
        closesocket(fd); s->refused_ip++; return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };            /* never block the task for long */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    mbtcp_client_t *slot = NULL, *oldest = NULL;
    for (int k = 0; k < MBTCP_MAX_CLIENTS; ++k) {
        mbtcp_client_t *c = &s->c[k];
        if (c->fd < 0) { if (!slot) slot = c; continue; }
        if (!oldest || (uint32_t)(now - c->last_ms) > (uint32_t)(now - oldest->last_ms)) oldest = c;
    }
    if (!slot) { drop(s, oldest); s->evicted++; slot = oldest; }
    slot->fd = fd; slot->ip = ip; slot->last_ms = now; slot->rx.n = 0;
    s->accepted++;
}

static bool send_all(int fd, const uint8_t *p, int n) {
    while (n > 0) {
        ssize_t w = send(fd, p, (size_t)n, 0);
        if (w <= 0) return false;
        p += w; n -= (int)w;
    }
    return true;
}

void mbtcp_poll(mbtcp_server_t *s, uint32_t timeout_ms) {
    if (s->lfd < 0) return;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s->lfd, &rd);
    int maxfd = s->lfd;
    for (int k = 0; k < MBTCP_MAX_CLIENTS; ++k)
        if (s->c[k].fd >= 0) { FD_SET(s->c[k].fd, &rd); if (s->c[k].fd > maxfd) maxfd = s->c[k].fd; }
    struct timeval tv = { .tv_sec = (long)(timeout_ms / 1000u), .tv_usec = (long)((timeout_ms % 1000u) * 1000u) };
    int r = select(maxfd + 1, &rd, NULL, NULL, &tv);
    if (r <= 0) return;
    uint32_t now = bsp_ms();
    static uint8_t in[512], out[4 * MBTCP_FRAME_MAX];
    for (int k = 0; k < MBTCP_MAX_CLIENTS; ++k) {
        mbtcp_client_t *c = &s->c[k];
        if (c->fd < 0 || !FD_ISSET(c->fd, &rd)) continue;
        ssize_t n = recv(c->fd, in, sizeof in, 0);
        if (n <= 0) { drop(s, c); continue; }
        bool ok = true;
        for (uint16_t off = 0; ok && off < (uint16_t)n;) {
            uint16_t used;
            int o = mbtcp_feed(&c->rx, in + off, (uint16_t)(n - off), &used, out, sizeof out);
            if (o < 0) { s->protocol_errors++; ok = false; break; }
            if (o > 0) {
                s->frames++;
                c->last_ms = now;
                param_comms_activity(PLAT_SRC_MODBUS, now);
                ok = send_all(c->fd, out, o);
            }
            off = (uint16_t)(off + used);
        }
        if (!ok) drop(s, c);
    }
    if (FD_ISSET(s->lfd, &rd)) accept_one(s, now);
}
