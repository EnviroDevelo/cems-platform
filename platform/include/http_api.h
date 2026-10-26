/* http_api.h - the instrument's web interface (design spec "Remote access",
 * "Web interface live updates", "Permanent-connection behaviour").
 *
 * The page loads once (GET /), then JavaScript polls JSON and updates values in
 * place. Every request is independent (one request per connection), so
 * drop-outs, reboots and laptop sleep recover on the next poll.
 *
 *   GET  /                 the page (gzip)
 *   GET  /api/info         unit status: product, firmware, API version, state, serial, mode, alarms, store, scan, session
 *   GET  /api/params       the parameter table definitions (names, types, limits, levels; "q":true = carries quality)
 *   GET  /api/values[?ids=1,2,3]   current values {"t":ms,"v":{"id":value,...},"q":{"id":"stale",...}}
 *                          "q" lists only measurements whose quality is not valid ("id.index" for an array element)
 *   POST /api/login        level=operator|admin|factory&password=...      -> session cookie
 *   POST /api/logout
 *   POST /api/write        id=..&v=..[&o=..]  (repeat for several; all or nothing)
 *   POST /api/password     level=..&password=..   (needs a session at that level or higher)
 *   GET  /api/backup       parameter backup file (admin)
 *   POST /api/restore      body = backup file (admin; factory part needs factory)
 *   GET  /api/log/info     logger status and ring fill
 *   GET  /api/log/events[?n=50]          the newest events, newest first (JSON)
 *   GET  /api/log/events.csv[?from=&to=] the whole event log (s since 1970, inclusive)
 *   GET  /api/log/data.csv[?from=&to=]   the whole data log
 *   GET  /api/tls/cert     the unit's HTTPS certificate (DER), to trust it on the site's laptops
 *   GET  /api/alarms       alarms shown (active or unacknowledged), most urgent first, and counts
 *   GET  /api/alarms/config every alarm with its actions (email, Modbus, relays) and the relay set-up
 *   POST /api/alarms/ack   id=<alarm ID>|all  (any login; logged with the login and source)
 *   GET  /api/time         the clock (local standard time), UTC offset, SNTP status
 *   POST /api/time         t=<local seconds since 1970> sets it; sync=1 asks the time server now (admin)
 *   GET  /api/mail         alarm email status: queue, last result, the server's certificate fingerprint, the pin
 *   POST /api/mail/test    send a test email (admin)
 *   POST /api/mail/trust   trust=1 pins the certificate the mail server last presented, trust=0 unpins (admin)
 *   GET  /api/fw           firmware status: running and staged versions, trial, upload progress
 *   POST /api/fw/upload    body = image file (.cfw), streamed into the free flash bank (tech/factory level)
 *   POST /api/fw/activate  restart on the staged image (tech/factory level); it confirms itself after a
 *                          healthy trial, or the unit rolls back
 *
 * Every POST must carry the header "X-CEMS: 1" (a page from another site cannot
 * add it without the browser asking first), and writes need a session cookie.
 *
 * Value formats in /api/write: bool true/false/1/0; integers in decimal (an IPv4
 * address is also accepted for unsigned entries); floats in decimal; text as
 * UTF-8; bytes as hex; arrays as comma-separated elements with o = first element. */
#ifndef HTTP_API_H
#define HTTP_API_H
#include <stdint.h>
#include <stdbool.h>
#include "http_util.h"
#include "web_auth.h"
#include "scan_engine.h"
#include "plat_log.h"

/* /api/info "api": bumped on any change a client could break on (1 = before 26 Oct 2026) */
#define HTTP_API_VERSION 2u

typedef struct {
    char     method[8];
    char     path[64];
    char     query[256];
    char     sid[AUTH_TOKEN_LEN + 1];
    char     host[64];             /* Host header (for the https:// redirect) */
    bool     csrf;                 /* X-CEMS: 1 present */
    bool     has_length;
    bool     keep_alive;           /* HTTP/1.1 without "Connection: close" (or 1.0 with keep-alive) */
    uint32_t content_length;
    const uint8_t *body;
    uint32_t body_len;
} http_req_t;

typedef struct {
    const char *product, *fw_version;
    const scan_engine_t *scan;     /* for the statistics on /api/info (may be NULL) */
    uint8_t    *scratch;           /* backup/restore buffer (one request at a time) */
    uint32_t    scratch_size;
    void      (*wait)(void);       /* called while waiting for the store task (sleep ~10 ms) */
    const plat_log_chan_t *log_chans;   /* the data-log channels (CSV column names if the */
    unsigned    n_log_chans;            /* schema record has been overwritten); may be NULL */
} http_cfg_t;

void http_api_init(const http_cfg_t *cfg);

/* Parse the request line and headers. Returns the header length (up to and
 * including the blank line), 0 if more bytes are needed, -1 if malformed. */
int  http_parse_head(const char *buf, uint32_t len, http_req_t *r);

/* Handle a complete request; writes the whole response to o. */
void http_handle(const http_req_t *r, http_out_t *o, uint32_t now_ms);

/* Requests whose body is passed on in pieces rather than buffered (the firmware
 * upload). The server hands over what it already received in r->body/body_len and a
 * reader for the rest; rd returns bytes read, <= 0 on timeout or close. */
typedef int (*http_read_fn)(void *ctx, uint8_t *buf, uint32_t cap);
bool http_wants_stream(const http_req_t *r);
void http_handle_stream(const http_req_t *r, http_read_fn rd, void *ctx, http_out_t *o, uint32_t now_ms);

/* Port 80: send the browser to the same page over HTTPS. */
void http_redirect_https(const http_req_t *r, http_out_t *o);

/* Answer without a request (e.g. 413 body too large, 400 bad request). */
void http_simple(http_out_t *o, int status, const char *msg);

#endif
