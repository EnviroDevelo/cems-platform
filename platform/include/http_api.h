/* http_api.h - the instrument's web interface (design spec "Remote access",
 * "Web interface live updates", "Permanent-connection behaviour").
 *
 * The page loads once (GET /), then JavaScript polls JSON and updates values in
 * place. Every request is independent (one request per connection), so
 * drop-outs, reboots and laptop sleep recover on the next poll.
 *
 *   GET  /                 the page (gzip)
 *   GET  /api/info         unit status: product, firmware, serial, mode, alarms, store, scan, session
 *   GET  /api/params       the parameter table definitions (names, types, limits, levels)
 *   GET  /api/values[?ids=1,2,3]   current values {"t":ms,"v":{"id":value,...}}
 *   POST /api/login        level=operator|admin|factory&password=...      -> session cookie
 *   POST /api/logout
 *   POST /api/write        id=..&v=..[&o=..]  (repeat for several; all or nothing)
 *   POST /api/password     level=..&password=..   (needs a session at that level or higher)
 *   GET  /api/backup       parameter backup file (admin)
 *   POST /api/restore      body = backup file (admin; factory part needs factory)
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

typedef struct {
    char     method[8];
    char     path[64];
    char     query[256];
    char     sid[AUTH_TOKEN_LEN + 1];
    bool     csrf;                 /* X-CEMS: 1 present */
    bool     has_length;
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
} http_cfg_t;

void http_api_init(const http_cfg_t *cfg);

/* Parse the request line and headers. Returns the header length (up to and
 * including the blank line), 0 if more bytes are needed, -1 if malformed. */
int  http_parse_head(const char *buf, uint32_t len, http_req_t *r);

/* Handle a complete request; writes the whole response to o. */
void http_handle(const http_req_t *r, http_out_t *o, uint32_t now_ms);

/* Answer without a request (e.g. 413 body too large, 400 bad request). */
void http_simple(http_out_t *o, int status, const char *msg);

#endif
