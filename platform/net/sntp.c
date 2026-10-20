/* sntp.c - SNTP v4 client (RFC 4330) for the site's time server; see plat_clock.h.
 * One query per interval (60 s after a failure), one UDP packet each way, 1 s wait. */
#include <stdio.h>
#include <string.h>
#include "plat_clock.h"
#include "plat_net.h"
#include "plat_cmdq.h"
#include "param.h"
#include "bsp.h"

#define WAIT_MS      1000u
#define RETRY_MS     60000u
#define NTP_UNIX     2208988800u     /* 1900 -> 1970 */
#define STEP_S       2               /* only step the clock when it is this far out */

static sntp_status_t S;
uint16_t sntp_port = 123;                    /* tests use another */
static uint32_t next_ms;
static bool now_req, started;

void sntp_reset_for_test(void) { memset(&S, 0, sizeof S); next_ms = 0; now_req = false; started = false; }
void sntp_request_now(void) { now_req = true; }
void sntp_status(sntp_status_t *s) { *s = S; }

static void fail(const char *why) { S.fails++; snprintf(S.error, sizeof S.error, "%s", why); }

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* one query; true = answered (and the clock stepped if needed) */
static bool query(uint32_t ip) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { fail("no socket"); return false; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(sntp_port); a.sin_addr.s_addr = htonl(ip);
    uint8_t p[48] = { 0x23 };                        /* LI 0, version 4, mode 3 (client) */
    uint32_t nonce = bsp_ms() ^ 0x5A5AC3E1u;         /* our "transmit time": the server must echo it */
    for (int k = 0; k < 4; ++k) { p[40 + k] = (uint8_t)(nonce >> (24 - 8 * k)); p[44 + k] = (uint8_t)(k * 37u + 11u); }
    uint8_t sent[8];
    memcpy(sent, p + 40, 8);
    uint32_t t1 = bsp_ms();
    if (sendto(s, p, sizeof p, 0, (struct sockaddr *)&a, sizeof a) != (ssize_t)sizeof p) {
        closesocket(s); fail("could not send"); return false;
    }
    for (;;) {
        uint32_t el = bsp_ms() - t1;
        if (el >= WAIT_MS) { closesocket(s); fail("no answer from the time server"); return false; }
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        struct timeval tv = { 0, (long)(WAIT_MS - el) * 1000L };
        if (select(s + 1, &rd, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(s, p, sizeof p, 0, (struct sockaddr *)&from, &fl);
        if (n < 48 || from.sin_addr.s_addr != a.sin_addr.s_addr || from.sin_port != a.sin_port) continue;
        if (memcmp(p + 24, sent, 8) != 0) continue;             /* not the answer to our question */
        break;
    }
    uint32_t rtt = bsp_ms() - t1;
    closesocket(s);
    uint8_t mode = p[0] & 7u, li = p[0] >> 6;
    if (mode != 4 && mode != 5) { fail("bad answer (mode)"); return false; }
    if (p[1] == 0 || p[1] > 15) { fail("server not synchronised"); return false; }
    if (li == 3) { fail("server clock not synchronised"); return false; }
    uint32_t tx = get32(p + 40), frac = get32(p + 44);
    if (tx < NTP_UNIX + PLAT_CLOCK_MIN_UNIX) { fail("server time implausible"); return false; }
    /* server time now = its transmit time + half the round trip; rounded to the second */
    uint64_t ms = (uint64_t)(tx - NTP_UNIX) * 1000u + (uint64_t)(((uint64_t)frac * 1000u) >> 32) + rtt / 2u;
    uint32_t utc = (uint32_t)((ms + 500u) / 1000u);
    uint32_t local = (uint32_t)((int64_t)utc + plat_clock_utc_offset_s());
    uint32_t cur;
    bool set = plat_clock_now(&cur);
    int64_t diff = (int64_t)local - (int64_t)cur;
    S.stratum = p[1];
    S.last_ok_ms = bsp_ms() ? bsp_ms() : 1u;
    S.last_diff_s = set ? (int32_t)diff : 0;
    S.error[0] = 0;
    if (!set || diff >= STEP_S || diff <= -STEP_S) {
        if (plat_clock_request(local, PLAT_SRC_INTERNAL, 0, "sntp") == 0) S.steps++;
    }
    return true;
}

void sntp_service(uint32_t now) {
    plat_val_t v;
    uint32_t ip = param_read(PARAM_ID_SNTP_SERVER, &v) ? v.u : 0;
    if (!ip) { started = false; return; }
    if (!started) { started = true; next_ms = now + 3000u; }   /* a few seconds after start-up or enabling */
    if (!now_req && (int32_t)(now - next_ms) < 0) return;
    now_req = false;
    S.tries++;
    bool ok = query(ip);
    uint32_t iv = param_read(PARAM_ID_SNTP_INTERVAL, &v) ? v.u : 60u;
    next_ms = bsp_ms() + (ok ? iv * 60000u : RETRY_MS);
}
