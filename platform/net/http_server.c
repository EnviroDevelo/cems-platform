/* http_server.c - see http_server.h */
#include <string.h>
#include "http_server.h"
#include "http_api.h"
#include "plat_net.h"
#include "bsp.h"

typedef struct { int fd; http_server_t *s; } conn_t;

static int recv_fd(void *ctx, uint8_t *buf, uint32_t cap) {
    conn_t *c = ctx;
    ssize_t got = recv(c->fd, buf, cap, 0);
    if (c->s->alive) c->s->alive();
    return got > 0 ? (int)got : -1;
}

static int send_fd(void *ctx, const void *p, size_t n) {
    conn_t *c = ctx;
    int fd = c->fd;
    if (c->s->alive) c->s->alive();
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

void http_close(http_server_t *s) { if (s->lfd >= 0) closesocket(s->lfd); s->lfd = -1; }

static void serve(http_server_t *s, int fd) {
    struct timeval rt = { .tv_sec = 2, .tv_usec = 0 }, wt = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wt, sizeof wt);
    http_out_t o;
    conn_t c = { fd, s };
    out_init(&o, send_fd, &c);
    http_req_t r;
    uint32_t n = 0;
    int hl = 0;
    while (hl == 0) {                                   /* headers */
        if (n >= s->cap) { s->too_large++; http_simple(&o, 413, "too large"); return; }
        ssize_t got = recv(fd, s->buf + n, s->cap - n, 0);
        if (s->alive) s->alive();
        if (got <= 0) { s->timeouts++; return; }
        n += (uint32_t)got;
        hl = http_parse_head((const char *)s->buf, n, &r);
    }
    if (hl < 0) { s->bad_requests++; http_simple(&o, 400, "bad request"); return; }
    if (!strcmp(r.method, "POST") && !r.has_length) { s->bad_requests++; http_simple(&o, 400, "length required"); return; }
    if (http_wants_stream(&r)) {                        /* firmware upload: not buffered */
        r.body = s->buf + hl;
        r.body_len = n - (uint32_t)hl > r.content_length ? r.content_length : n - (uint32_t)hl;
        http_handle_stream(&r, recv_fd, &c, &o, bsp_ms());
        s->served++;
        return;
    }
    if (r.content_length > s->cap - (uint32_t)hl) { s->too_large++; http_simple(&o, 413, "too large"); return; }
    while (n < (uint32_t)hl + r.content_length) {       /* body */
        ssize_t got = recv(fd, s->buf + n, (uint32_t)hl + r.content_length - n, 0);
        if (s->alive) s->alive();
        if (got <= 0) { s->timeouts++; return; }
        n += (uint32_t)got;
    }
    r.body = s->buf + hl;
    r.body_len = r.content_length;
    http_handle(&r, &o, bsp_ms());
    s->served++;
}

void http_poll(http_server_t *s, uint32_t timeout_ms) {
    if (s->lfd < 0) return;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(s->lfd, &rd);
    struct timeval tv = { .tv_sec = (long)(timeout_ms / 1000u), .tv_usec = (long)((timeout_ms % 1000u) * 1000u) };
    if (select(s->lfd + 1, &rd, NULL, NULL, &tv) <= 0) return;
    int fd = accept(s->lfd, NULL, NULL);
    if (fd < 0) return;
    serve(s, fd);
    shutdown(fd, SHUT_WR);
    closesocket(fd);
}
