/* http_server.h - the web server's socket loop. One request per connection
 * (Connection: close), handled to completion, so one slow client cannot hold
 * the server for long: every read has a 2 s timeout. The browser opens a new
 * connection for each poll; the listen backlog queues the others meanwhile. */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H
#include <stdint.h>

typedef struct {
    int      lfd;
    uint8_t *buf;          /* request buffer: headers + the largest upload (a backup file) */
    uint32_t cap;
    uint32_t served, bad_requests, too_large, timeouts;
    void   (*alive)(void);  /* called while receiving and sending (watchdog check-in); may be NULL */
} http_server_t;

int  http_open(http_server_t *s, uint16_t port, uint8_t *buf, uint32_t cap);
void http_poll(http_server_t *s, uint32_t timeout_ms);
void http_close(http_server_t *s);

#endif
