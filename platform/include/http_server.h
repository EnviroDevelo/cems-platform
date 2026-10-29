/* http_server.h - the web server's socket loop. One connection at a time, each
 * request handled to completion, every read with a 2 s timeout.
 *
 * Waiting connections: up to HTTP_PENDING accepted sockets wait in a table, with no
 * TLS yet, until their first bytes arrive; only then does the handshake start. A
 * browser opens spare sockets that may never speak, and served at once each would
 * hold the server for the 2 s read timeout (several browsers then saw one value poll
 * every 5-8 s). A socket silent for HTTP_PENDING_MS is closed. With the kept
 * connections that is 5 sockets, the HTTP share of lwIP's MEMP_NUM_TCP_PCB.
 *
 * Keep-alive (HTTPS only): after a GET the connection stays open, so the page's
 * polls reuse it instead of making a TLS connection each time. Up to HTTP_KEPT stay
 * open together (each on its own TLS connection, tls_use), so two browsers each keep
 * theirs. A new connection that speaks takes a free place, or else the place of the
 * longest-idle kept one (the server is never held by one browser). A kept connection
 * is also closed after 5 s idle, after 200 requests, after any POST (a browser does
 * not retry a POST on a connection the server closed), and before the alarm email
 * borrows the TLS context. Requests are served one at a time, the kept connections
 * taking turns. */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H
#include <stdint.h>
#include <stdbool.h>

#define HTTP_PENDING    3u      /* accepted, waiting for their first bytes */
#define HTTP_PENDING_MS 2000u   /* closed if silent this long */
#define HTTP_KEPT       2u      /* kept-alive connections at once (<= TLS_CONNS) */

typedef struct {
    int      lfd;
    uint8_t *buf;          /* request buffer: headers + the largest upload (a backup file) */
    uint32_t cap;
    uint32_t served, bad_requests, too_large, timeouts;
    void   (*alive)(void);  /* called while receiving and sending (watchdog check-in); may be NULL */
    bool     tls;           /* HTTPS: every connection goes through tls.h */
    bool     redirect_only; /* plain HTTP that only sends browsers to https:// (port 80) */
    uint32_t tls_failed;
    int      kfd[HTTP_KEPT];     /* kept-alive connections, -1 = none; place k uses TLS connection k */
    uint32_t kept_ms[HTTP_KEPT]; /* each one's last activity */
    uint32_t kept_n[HTTP_KEPT];  /* requests on each */
    unsigned kturn;         /* the kept connection served first next time (taking turns) */
    uint32_t reused;        /* requests served on a kept connection (no new TLS connection) */
    int      pfd[HTTP_PENDING];  /* waiting connections, -1 = free */
    uint32_t pms[HTTP_PENDING];  /* when each was accepted */
    uint32_t silent;        /* waiting connections closed without a request */
} http_server_t;

int  http_open(http_server_t *s, uint16_t port, uint8_t *buf, uint32_t cap);
void http_poll(http_server_t *s, uint32_t timeout_ms);
void http_close(http_server_t *s);
void http_drop_kept(http_server_t *s);   /* close the kept-alive connections (e.g. before the mail client) */

#endif
