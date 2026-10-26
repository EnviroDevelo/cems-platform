/* http_server.c - see http_server.h */
#include <string.h>
#include "http_server.h"
#include "http_api.h"
#include "plat_net.h"
#include "bsp.h"
#include "tls.h"

typedef struct { int fd; http_server_t *s; } conn_t;

static int recv_fd(void *ctx, uint8_t *buf, uint32_t cap) {
    conn_t *c = ctx;
    int got = c->s->tls ? tls_read(buf, cap) : (int)recv(c->fd, buf, cap, 0);
    if (c->s->alive) c->s->alive();
    return got > 0 ? got : -1;
}

static int send_fd(void *ctx, const void *p, size_t n) {
    conn_t *c = ctx;
    int fd = c->fd;
    if (c->s->alive) c->s->alive();
    if (c->s->tls) return tls_write(p, (uint32_t)n);
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}

int http_open(http_server_t *s, uint16_t port, uint8_t *buf, uint32_t cap) {
    memset(s, 0, sizeof *s);
    s->buf = buf; s->cap = cap;
    s->kfd = -1;
    for (unsigned i = 0; i < HTTP_PENDING; i++) s->pfd[i] = -1;
    s->lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->lfd < 0) return -1;
    int one = 1;
    setsockopt(s->lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(s->lfd, 4) != 0) {
        closesocket(s->lfd); s->lfd = -1; return -1;
    }
    return 0;
}

void http_close(http_server_t *s) {
    http_drop_kept(s);
    for (unsigned i = 0; i < HTTP_PENDING; i++) if (s->pfd[i] >= 0) { closesocket(s->pfd[i]); s->pfd[i] = -1; }
    if (s->lfd >= 0) closesocket(s->lfd);
    s->lfd = -1;
}

#define KEEP_IDLE_MS 5000u
#define KEEP_MAX     200u

/* one request on fd (TLS already up if HTTPS); true = the connection may stay open */
static bool serve(http_server_t *s, int fd) {
    http_out_t o;
    conn_t c = { fd, s };
    out_init(&o, send_fd, &c);
    http_req_t r;
    uint32_t n = 0;
    int hl = 0;
    while (hl == 0) {                                   /* headers */
        if (n >= s->cap) { s->too_large++; http_simple(&o, 413, "too large"); return false; }
        int got = recv_fd(&c, s->buf + n, s->cap - n);
        if (got <= 0) { s->timeouts++; return false; }
        n += (uint32_t)got;
        hl = http_parse_head((const char *)s->buf, n, &r);
    }
    if (hl < 0) { s->bad_requests++; http_simple(&o, 400, "bad request"); return false; }
    if (s->redirect_only) { http_redirect_https(&r, &o); s->served++; return false; }
    if (!strcmp(r.method, "POST") && !r.has_length) { s->bad_requests++; http_simple(&o, 400, "length required"); return false; }
    if (http_wants_stream(&r)) {                        /* firmware upload: not buffered */
        r.body = s->buf + hl;
        r.body_len = n - (uint32_t)hl > r.content_length ? r.content_length : n - (uint32_t)hl;
        http_handle_stream(&r, recv_fd, &c, &o, bsp_ms());
        s->served++;
        return false;
    }
    if (r.content_length > s->cap - (uint32_t)hl) { s->too_large++; http_simple(&o, 413, "too large"); return false; }
    while (n < (uint32_t)hl + r.content_length) {       /* body */
        int got = recv_fd(&c, s->buf + n, (uint32_t)hl + r.content_length - n);
        if (got <= 0) { s->timeouts++; return false; }
        n += (uint32_t)got;
    }
    r.body = s->buf + hl;
    r.body_len = r.content_length;
    o.keep = s->tls && r.keep_alive && !strcmp(r.method, "GET") && n == (uint32_t)hl + r.content_length;
    http_handle(&r, &o, bsp_ms());
    s->served++;
    return o.keep && !o.err;
}

static void finish(http_server_t *s, int fd) {
    if (s->tls) tls_close();
    shutdown(fd, SHUT_WR);
    closesocket(fd);
}

void http_drop_kept(http_server_t *s) {
    if (s->kfd < 0) return;
    finish(s, s->kfd);
    s->kfd = -1;
}

static void set_timeouts(int fd) {
    struct timeval rt = { .tv_sec = 2, .tv_usec = 0 }, wt = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wt, sizeof wt);
}

/* a waiting connection that has spoken: handshake, one request, then keep it or close it */
static void start(http_server_t *s, int fd) {
    uint8_t b;
    if (recv(fd, &b, 1, MSG_PEEK | MSG_DONTWAIT) == 0) { s->silent++; closesocket(fd); return; }   /* closed unused */
    http_drop_kept(s);                                  /* someone else is waiting: they come first */
    set_timeouts(fd);
    if (s->tls && tls_accept(fd) != 0) { s->tls_failed++; finish(s, fd); return; }
    if (serve(s, fd)) { s->kfd = fd; s->kept_ms = bsp_ms(); s->kept_n = 1; }
    else finish(s, fd);
}

void http_poll(http_server_t *s, uint32_t timeout_ms) {
    if (s->lfd < 0) return;
    uint32_t now = bsp_ms();
    if (s->kfd >= 0 && (now - s->kept_ms > KEEP_IDLE_MS || s->kept_n >= KEEP_MAX)) http_drop_kept(s);
    fd_set rd;
    FD_ZERO(&rd);
    int mx = -1, freeslot = -1;
    for (unsigned i = 0; i < HTTP_PENDING; i++) {
        if (s->pfd[i] >= 0 && now - s->pms[i] > HTTP_PENDING_MS) { s->silent++; closesocket(s->pfd[i]); s->pfd[i] = -1; }
        if (s->pfd[i] < 0) { if (freeslot < 0) freeslot = (int)i; continue; }
        FD_SET(s->pfd[i], &rd); if (s->pfd[i] > mx) mx = s->pfd[i];
    }
    if (freeslot >= 0) { FD_SET(s->lfd, &rd); if (s->lfd > mx) mx = s->lfd; }   /* table full: new ones wait in the backlog */
    if (s->kfd >= 0) { FD_SET(s->kfd, &rd); if (s->kfd > mx) mx = s->kfd; }
    struct timeval tv = { .tv_sec = (long)(timeout_ms / 1000u), .tv_usec = (long)((timeout_ms % 1000u) * 1000u) };
    bool pending = s->kfd >= 0 && s->tls && tls_pending();
    if (pending) tv.tv_sec = tv.tv_usec = 0;
    int nr = select(mx + 1, &rd, NULL, NULL, &tv);
    if (nr < 0) return;
    if (freeslot >= 0 && FD_ISSET(s->lfd, &rd)) {
        int fd = accept(s->lfd, NULL, NULL);
        if (fd >= 0) { s->pfd[freeslot] = fd; s->pms[freeslot] = bsp_ms(); }   /* served once it speaks */
    }
    unsigned first = 0;                                 /* the longest waiting that has spoken */
    int pick = -1;
    for (unsigned i = 0; i < HTTP_PENDING; i++)
        if (s->pfd[i] >= 0 && FD_ISSET(s->pfd[i], &rd) && (pick < 0 || (int32_t)(s->pms[i] - s->pms[first]) < 0)) { pick = (int)i; first = i; }
    if (pick >= 0) {
        int fd = s->pfd[pick];
        s->pfd[pick] = -1;
        start(s, fd);
        return;
    }
    if (s->kfd >= 0 && (pending || FD_ISSET(s->kfd, &rd))) {
        int fd = s->kfd;
        s->reused++;
        if (serve(s, fd)) { s->kept_ms = bsp_ms(); s->kept_n++; }
        else { s->kfd = -1; finish(s, fd); }
    }
}
