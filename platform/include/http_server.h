/* http_server.h - the web server's socket loop. One request per connection
 * (Connection: close), handled to completion, so one slow client cannot hold
 * the server for long: every read has a 2 s timeout. The browser opens a new
 * connection for each poll; the listen backlog queues the others meanwhile. */
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
} http_server_t;

int  http_open(http_server_t *s, uint16_t port, uint8_t *buf, uint32_t cap);
void http_poll(http_server_t *s, uint32_t timeout_ms);
void http_close(http_server_t *s);

#endif
