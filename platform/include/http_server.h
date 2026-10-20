/* http_server.h - the web server's socket loop. One connection at a time, each
 * request handled to completion, every read with a 2 s timeout.
 *
 * Keep-alive (HTTPS only): after a GET the connection stays open, so the page's
 * polls reuse it instead of making a TLS connection each time. It is closed as soon
 * as another connection is waiting (the server is never held by one browser), after
 * 5 s idle, after 200 requests, after any POST (a browser does not retry a POST on a
 * connection the server closed), and before the alarm email borrows the TLS context. */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int      lfd;
    uint8_t *buf;          /* request buffer: headers + the largest upload (a backup file) */
    uint32_t cap;
    uint32_t served, bad_requests, too_large, timeouts;
    void   (*alive)(void);  /* called while receiving and sending (watchdog check-in); may be NULL */
    bool     tls;           /* HTTPS: every connection goes through tls.h (one at a time) */
    bool     redirect_only; /* plain HTTP that only sends browsers to https:// (port 80) */
    uint32_t tls_failed;
    int      kfd;           /* the kept-alive connection, -1 = none */
    uint32_t kept_ms;       /* its last activity */
    uint32_t kept_n;        /* requests on it */
    uint32_t reused;        /* requests served on a kept connection (no new TLS connection) */
} http_server_t;

int  http_open(http_server_t *s, uint16_t port, uint8_t *buf, uint32_t cap);
void http_poll(http_server_t *s, uint32_t timeout_ms);
void http_close(http_server_t *s);
void http_drop_kept(http_server_t *s);   /* close the kept-alive connection (e.g. before the mail client) */

#endif
