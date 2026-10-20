/* mail.c - see mail.h */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "mail.h"
#include "plat_net.h"
#include "plat_alarm.h"
#include "plat_log.h"
#include "plat_port.h"
#include "plat_cmdq.h"
#include "param.h"
#include "plat_clock.h"
#include "tls.h"
#include "ul_system.h"
#include "mbedtls/base64.h"

#define TRIES        3u
#define CONNECT_MS   5000u
#define IO_TIMEOUT_S 10

typedef struct { uint16_t id; uint8_t what; uint32_t t_unix, up_s; } item_t;

static item_t   mq[MAIL_QUEUE];
static volatile uint32_t q_head, q_tail;      /* head: scan task; tail: HTTP task */
static uint32_t batch_t, next_try;
static mail_status_t M;
uint32_t mail_retry_ms = 60000u;
static const char *product = "CEMS Supervisor";
/* the unit's address for the link in the email (the network layer provides it; absent in some builds) */
extern bool ui_net_status(uint32_t *ip, bool *link_up, uint8_t mac[6]) __attribute__((weak));

/* ---------------- queue (scan task) ---------------- */
void plat_alarm_email(const alarm_def_t *d, uint8_t what, uint32_t t_unix, uint32_t up_s) {
    plat_val_t v;
    if (!param_read(PARAM_ID_MAIL_ENABLE, &v) || !v.b) return;
    if (what == ALARM_TR_CLEAR && (!param_read(PARAM_ID_MAIL_ON_CLEAR, &v) || !v.b)) return;
    if (what != ALARM_TR_RAISE && what != ALARM_TR_CLEAR) return;
    PLAT_CRIT_ENTER();
    if (q_head - q_tail < MAIL_QUEUE) {
        mq[q_head % MAIL_QUEUE] = (item_t){ .id = d->id, .what = what, .t_unix = t_unix, .up_s = up_s };
        q_head++;
    } else {
        M.dropped++;
    }
    PLAT_CRIT_EXIT();
}

void mail_reset_for_test(void) {
    PLAT_CRIT_ENTER();
    q_head = q_tail = 0;
    PLAT_CRIT_EXIT();
    batch_t = next_try = 0;
    memset(&M, 0, sizeof M);
}

void mail_init(const char *prod) { if (prod && *prod) product = prod; }

const char *mail_err_name(mail_err_t e) {
    static const char *const n[] = { "sent", "mail is off", "settings incomplete", "no connection to the mail server",
        "TLS failed", "server certificate does not match", "server protocol error", "login refused",
        "server refused", "login needs TLS", "busy" };
    return (unsigned)e < sizeof n / sizeof n[0] ? n[e] : "?";
}

int mail_request_test(void) {
    plat_val_t v;
    if (!param_read(PARAM_ID_MAIL_ENABLE, &v) || !v.b) return -1;
    M.test_pending = true;
    return 0;
}

void mail_status(mail_status_t *s) {
    *s = M;
    s->queued = (uint8_t)(q_head - q_tail);
    s->busy = s->queued || M.test_pending;
}

/* ---------------- connection ---------------- */
static int  fd = -1;
static bool tls_on;
static char rbuf[512];
static size_t rlen;
static void (*alive_cb)(void);
static void (*release_cb)(void);

static void alive(void) { if (alive_cb) alive_cb(); }

static int connect_to(uint32_t ip, uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    int fl = plat_fcntl(s, F_GETFL, 0);
    plat_fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, (struct sockaddr *)&a, sizeof a);
    if (rc != 0 && errno != EINPROGRESS) { closesocket(s); return -1; }
    if (rc != 0) {
        fd_set w; FD_ZERO(&w); FD_SET(s, &w);
        struct timeval tv = { CONNECT_MS / 1000u, 0 };
        int err = 0; socklen_t el = sizeof err;
        if (select(s + 1, NULL, &w, NULL, &tv) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
            closesocket(s); return -1;
        }
    }
    plat_fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval to = { IO_TIMEOUT_S, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
    return s;
}

static bool wr(const char *p, size_t n) {
    if (tls_on) return tls_write((const uint8_t *)p, (uint32_t)n) == 0;
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}
static bool wrs(const char *s) { return wr(s, strlen(s)); }

/* one reply (possibly multi-line "250-..."), code returned; the whole text in all */
static int reply(char *all, size_t cap) {
    size_t an = 0;
    if (all && cap) all[0] = 0;
    for (;;) {
        char *nl = memchr(rbuf, '\n', rlen);
        if (!nl) {
            if (rlen >= sizeof rbuf - 1) return -1;
            int r = tls_on ? tls_read((uint8_t *)rbuf + rlen, (uint32_t)(sizeof rbuf - 1 - rlen))
                           : (int)recv(fd, rbuf + rlen, sizeof rbuf - 1 - rlen, 0);
            if (r <= 0) return -1;
            rlen += (size_t)r;
            alive();
            continue;
        }
        size_t ll = (size_t)(nl - rbuf) + 1;
        if (all) {
            size_t c = ll < cap - an - 1 ? ll : cap - an - 1;
            memcpy(all + an, rbuf, c); an += c; all[an] = 0;
        }
        bool last = ll >= 4 && rbuf[3] == ' ';
        int code = (rbuf[0] - '0') * 100 + (rbuf[1] - '0') * 10 + (rbuf[2] - '0');
        bool digits = ll >= 4 && rbuf[0] >= '0' && rbuf[0] <= '9' && rbuf[1] >= '0' && rbuf[1] <= '9' && rbuf[2] >= '0' && rbuf[2] <= '9';
        memmove(rbuf, rbuf + ll, rlen - ll);
        rlen -= ll;
        if (!digits) return -1;
        if (last || ll < 4) return code;
    }
}

static mail_err_t fail_text(mail_err_t e, const char *srv) {
    M.last = e;
    if (srv && *srv) {
        size_t k = 0;
        for (; srv[k] && srv[k] != '\r' && srv[k] != '\n' && k + 1 < sizeof M.last_text; ++k) M.last_text[k] = srv[k];
        M.last_text[k] = 0;
    } else {
        snprintf(M.last_text, sizeof M.last_text, "%s", mail_err_name(e));
    }
    return e;
}

/* command, then expect a reply class (2 = 2xx, 3 = 3xx) */
static int cmd(const char *c, int want, char *all, size_t cap) {
    if (c && !wrs(c)) return -1;
    int code = reply(all, cap);
    return code / 100 == want ? code : -(code < 0 ? 1 : code);
}

static bool contains_word(const char *hay, const char *w) {
    size_t n = strlen(w);
    for (const char *p = hay; (p = strstr(p, w)) != NULL; p += n) {
        bool l = p == hay || p[-1] == ' ' || p[-1] == '-' || p[-1] == '\n' || p[-1] == '=';
        bool r = p[n] == 0 || p[n] == ' ' || p[n] == '\r' || p[n] == '\n';
        if (l && r) return true;
    }
    return false;
}

/* ---------------- message ---------------- */
static void fmt_unix(char *b, size_t cap, uint32_t t, uint32_t up) {
    if (!t) { snprintf(b, cap, "(uptime %lu min)", (unsigned long)(up / 60u)); return; }
    unsigned y, mo, d, h, mi, s;
    plat_civil_from_unix(t, &y, &mo, &d, &h, &mi, &s);
    snprintf(b, cap, "%04u-%02u-%02u %02u:%02u:%02u", y, mo, d, h, mi, s);
}

static uint32_t now_unix(void) {
    if (RTC_Date.Year < 2000 || !RTC_Date.Month) return 0;
    return plat_unix_from_civil(RTC_Date.Year, RTC_Date.Month, RTC_Date.Day_In_Month, RTC_Time.Hours, RTC_Time.Minutes, RTC_Time.Seconds);
}

/* a body line; a leading '.' is doubled (SMTP transparency) */
static bool line(const char *s) {
    if (s[0] == '.' && !wr(".", 1)) return false;
    return wrs(s) && wr("\r\n", 2);
}

static void text_of(char *site, size_t cap) {
    if (param_read_blob(PARAM_ID_SITE_NAME, 0, site, (uint16_t)(cap - 1)) != PERR_OK) site[0] = 0;
    site[cap - 1] = 0;
}

static bool message(const item_t *it, unsigned n, bool test, const char *from, const char *to) {
    char site[33], b[200], t[32];
    plat_val_t v;
    text_of(site, sizeof site);
    uint32_t serial = param_read(PARAM_ID_SERIAL, &v) ? v.u : 0;
    char who[64];
    if (site[0]) snprintf(who, sizeof who, "%s", site);
    else snprintf(who, sizeof who, "S/N %lu", (unsigned long)serial);

    snprintf(b, sizeof b, "From: %s <%s>", product, from); if (!line(b)) return false;
    if (!wrs("To: ") || !wrs(to) || !wr("\r\n", 2)) return false;
    uint32_t nu = now_unix();
    if (nu) {
        static const char *const dn[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
        static const char *const mn[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        unsigned y, mo, d, h, mi, s;
        plat_civil_from_unix(nu, &y, &mo, &d, &h, &mi, &s);
        int32_t off = plat_clock_utc_offset_s() / 60;
        unsigned ao = (unsigned)(off < 0 ? -off : off);
        snprintf(b, sizeof b, "Date: %s, %u %s %u %02u:%02u:%02u %c%02u%02u", dn[(nu / 86400u) % 7u], d, mn[(mo - 1u) % 12u], y,
                 h, mi, s, off < 0 ? '-' : '+', ao / 60u, ao % 60u);
        if (!line(b)) return false;
    }
    if (test) {
        snprintf(b, sizeof b, "Subject: [%s] Test email", who);
    } else {
        alarm_view_t a;
        const char *first = alarm_get(it[0].id, &a) ? a.text : "alarm";
        snprintf(b, sizeof b, "Subject: [%s] %s: %.80s%s", who, it[0].what == ALARM_TR_RAISE ? "ALARM" : "Cleared",
                 first, n > 1 ? " (and more)" : "");
    }
    if (!line(b)) return false;
    snprintf(b, sizeof b, "Message-ID: <%lu.%lu.%lu@cems>", (unsigned long)serial, (unsigned long)nu, (unsigned long)(M.sent + M.failed));
    if (!line(b) || !line("MIME-Version: 1.0") || !line("Content-Type: text/plain; charset=utf-8") ||
        !line("Content-Transfer-Encoding: 8bit") || !line("Auto-Submitted: auto-generated") || !line("")) return false;

    if (test) {
        if (!line("This is a test email from the instrument. Alarm email is working.")) return false;
    } else {
        for (unsigned k = 0; k < n; ++k) {
            alarm_view_t a;
            bool known = alarm_get(it[k].id, &a);
            fmt_unix(t, sizeof t, it[k].t_unix, it[k].up_s);
            snprintf(b, sizeof b, "%s  %s  %.70s  (%s, %s %u)", t, it[k].what == ALARM_TR_RAISE ? "RAISED " : "CLEARED",
                     known ? a.text : "?", known ? alarm_sev_name(a.severity) : "?", known ? a.name : "alarm", it[k].id);
            if (!line(b)) return false;
        }
        if (M.dropped) { snprintf(b, sizeof b, "(%lu earlier alarm events were not emailed: too many at once)", (unsigned long)M.dropped); if (!line(b)) return false; }
        alarm_summary_t s;
        alarm_summary(&s);
        snprintf(b, sizeof b, "Now: %u alarm%s active, %u waiting for an acknowledge.", s.active, s.active == 1 ? "" : "s", s.unacked);
        if (!line("") || !line(b)) return false;
        uint64_t pos = 0;
        alarm_view_t a;
        for (unsigned k = 0; k < 20 && alarm_next(&pos, &a); ++k) {
            fmt_unix(t, sizeof t, a.t_raise, a.up_raise);
            snprintf(b, sizeof b, "  %.70s - %s since %s", a.text,
                     a.active ? (a.unacked ? "active, not acknowledged" : "active") : "cleared, not acknowledged", t);
            if (!line(b)) return false;
        }
    }
    uint32_t ip = 0; bool up = false; uint8_t mac[6];
    if (ui_net_status) (void)ui_net_status(&ip, &up, mac);
    snprintf(b, sizeof b, "%s, serial %lu", product, (unsigned long)serial);
    if (!line("") || !line("--") || !line(b)) return false;
    if (ip) {
        snprintf(b, sizeof b, "https://%u.%u.%u.%u/", (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255u),
                 (unsigned)((ip >> 8) & 255u), (unsigned)(ip & 255u));
        if (!line(b)) return false;
    }
    return line("Sent automatically by the instrument; replies are not read.");
}

/* ---------------- one SMTP session ---------------- */
static bool all_zero(const uint8_t *p, size_t n) { uint8_t x = 0; for (size_t k = 0; k < n; ++k) x |= p[k]; return !x; }

static mail_err_t start_tls(const uint8_t pin[32]) {
    int rc = tls_client_open(fd, M.server_fp);
    if (rc != 0) {
        char t[40];
        snprintf(t, sizeof t, "TLS failed (-0x%04X)", (unsigned)(rc < 0 ? -rc : rc));
        return fail_text(MAIL_E_TLS, t);
    }
    tls_on = true;
    M.have_fp = true;
    if (!all_zero(pin, 32) && memcmp(pin, M.server_fp, 32) != 0) return MAIL_E_PIN;
    return MAIL_OK;
}

static mail_err_t session(const item_t *it, unsigned n, bool test) {
    plat_val_t v;
    uint32_t ip = param_read(PARAM_ID_MAIL_SERVER, &v) ? v.u : 0;
    uint16_t port = param_read(PARAM_ID_MAIL_PORT, &v) ? (uint16_t)v.u : 25;
    uint8_t sec = param_read(PARAM_ID_MAIL_SECURITY, &v) ? (uint8_t)v.u : MAIL_SEC_STARTTLS;
    uint8_t pin[32];
    static char user[49], pass[49], from[65], to[161], ehlo[512], b[300];
    if (param_read_blob(PARAM_ID_MAIL_PIN, 0, pin, 32) != PERR_OK) memset(pin, 0, sizeof pin);
    memset(user, 0, sizeof user); memset(pass, 0, sizeof pass); memset(from, 0, sizeof from); memset(to, 0, sizeof to);
    (void)param_read_blob(PARAM_ID_MAIL_USER, 0, user, 48);
    (void)param_read_secret(PARAM_ID_MAIL_PASS, pass, 48);
    (void)param_read_blob(PARAM_ID_MAIL_FROM, 0, from, 64);
    (void)param_read_blob(PARAM_ID_MAIL_TO, 0, to, 160);
    if (!ip || !from[0] || !to[0]) return MAIL_E_CONFIG;
    if (user[0] && sec == MAIL_SEC_NONE) return MAIL_E_NOTLS_LOGIN;

    M.have_fp = false;
    rlen = 0;
    if ((fd = connect_to(ip, port)) < 0) return MAIL_E_CONNECT;
    alive();
    mail_err_t e;
    if (sec == MAIL_SEC_TLS && (e = start_tls(pin)) != MAIL_OK) return e;
    if (cmd(NULL, 2, b, sizeof b) < 0) return fail_text(MAIL_E_PROTOCOL, b);
    if (cmd("EHLO cems-supervisor\r\n", 2, ehlo, sizeof ehlo) < 0) return fail_text(MAIL_E_PROTOCOL, ehlo);
    if (sec == MAIL_SEC_STARTTLS) {
        if (!contains_word(ehlo, "STARTTLS")) return fail_text(MAIL_E_TLS, "the server does not offer STARTTLS");
        if (cmd("STARTTLS\r\n", 2, b, sizeof b) < 0) return fail_text(MAIL_E_TLS, b);
        rlen = 0;
        if ((e = start_tls(pin)) != MAIL_OK) return e;
        if (cmd("EHLO cems-supervisor\r\n", 2, ehlo, sizeof ehlo) < 0) return fail_text(MAIL_E_PROTOCOL, ehlo);
    }
    alive();
    if (user[0]) {
        size_t ul = strlen(user), pl = strlen(pass), ol = 0;
        static uint8_t raw[100];
        static char enc[140];
        if (contains_word(ehlo, "PLAIN")) {
            raw[0] = 0; memcpy(raw + 1, user, ul); raw[1 + ul] = 0; memcpy(raw + 2 + ul, pass, pl);
            if (mbedtls_base64_encode((uint8_t *)enc, sizeof enc, &ol, raw, 2 + ul + pl) != 0) return MAIL_E_CONFIG;
            snprintf(b, sizeof b, "AUTH PLAIN %s\r\n", enc);
            int c = cmd(b, 2, b, sizeof b);
            memset(raw, 0, sizeof raw); memset(enc, 0, sizeof enc);
            if (c < 0) return fail_text(MAIL_E_AUTH, b);
        } else if (contains_word(ehlo, "LOGIN")) {
            if (cmd("AUTH LOGIN\r\n", 3, b, sizeof b) < 0) return fail_text(MAIL_E_AUTH, b);
            mbedtls_base64_encode((uint8_t *)enc, sizeof enc - 2, &ol, (const uint8_t *)user, ul);
            strcpy(enc + ol, "\r\n");
            if (cmd(enc, 3, b, sizeof b) < 0) return fail_text(MAIL_E_AUTH, b);
            mbedtls_base64_encode((uint8_t *)enc, sizeof enc - 2, &ol, (const uint8_t *)pass, pl);
            strcpy(enc + ol, "\r\n");
            int c = cmd(enc, 2, b, sizeof b);
            memset(enc, 0, sizeof enc);
            if (c < 0) return fail_text(MAIL_E_AUTH, b);
        } else {
            return fail_text(MAIL_E_AUTH, "the server offers no login method this unit supports");
        }
    }
    memset(pass, 0, sizeof pass);
    snprintf(b, sizeof b, "MAIL FROM:<%s>\r\n", from);
    if (cmd(b, 2, b, sizeof b) < 0) return fail_text(MAIL_E_REFUSED, b);
    unsigned rcpt = 0;
    for (char *p = to; *p && rcpt < 4;) {                  /* "a@x, b@y; c@z" */
        while (*p == ' ' || *p == ',' || *p == ';') p++;
        char *q = p;
        while (*q && *q != ',' && *q != ';' && *q != ' ') q++;
        if (q == p) break;
        snprintf(b, sizeof b, "RCPT TO:<%.*s>\r\n", (int)(q - p), p);
        if (cmd(b, 2, b, sizeof b) < 0) return fail_text(MAIL_E_REFUSED, b);
        rcpt++;
        p = q;
    }
    if (!rcpt) return MAIL_E_CONFIG;
    if (cmd("DATA\r\n", 3, b, sizeof b) < 0) return fail_text(MAIL_E_REFUSED, b);
    alive();
    if (!message(it, n, test, from, to)) return MAIL_E_PROTOCOL;
    if (cmd(".\r\n", 2, b, sizeof b) < 0) return fail_text(MAIL_E_REFUSED, b);
    (void)cmd("QUIT\r\n", 2, NULL, 0);
    return MAIL_OK;
}

static mail_err_t send_now(const item_t *it, unsigned n, bool test) {
    if (release_cb) release_cb();
    M.last_text[0] = 0;
    M.last = MAIL_OK;
    mail_err_t e = session(it, n, test);
    if (tls_on) tls_client_close();
    tls_on = false;
    if (fd >= 0) closesocket(fd);
    fd = -1;
    if (e != MAIL_OK && M.last != e) fail_text(e, NULL);
    if (e == MAIL_OK) { M.last = MAIL_OK; snprintf(M.last_text, sizeof M.last_text, "sent"); }
    return e;
}

static void log_result(unsigned n, mail_err_t e) {
    char t[23];
    snprintf(t, sizeof t, "%s", e == MAIL_OK ? "sent" : mail_err_name(e));
    plat_log_event(LOG_EV_MAIL, PLAT_SRC_INTERNAL, 0, n, NAN, e == MAIL_OK ? 1.0 : 0.0, t);
}

void mail_service(uint32_t now, void (*alive_fn)(void), void (*release_fn)(void)) {
    release_cb = release_fn;
    plat_val_t v;
    alive_cb = alive_fn;
    if (!param_read(PARAM_ID_MAIL_ENABLE, &v) || !v.b) {          /* off: forget everything */
        if (q_head != q_tail || M.test_pending) {
            PLAT_CRIT_ENTER(); q_tail = q_head; PLAT_CRIT_EXIT();
            M.test_pending = false; batch_t = 0; M.tries = 0;
        }
        alarm_set(ALARM_ID_NOTIFY_FAIL, false);
        return;
    }
    if (M.test_pending) {
        M.test_pending = false;
        M.last_ms = now ? now : 1;
        mail_err_t e = send_now(NULL, 0, true);
        if (e == MAIL_OK) { M.sent++; alarm_set(ALARM_ID_NOTIFY_FAIL, false); } else M.failed++;
        log_result(0, e);
        return;
    }
    uint32_t avail = q_head - q_tail;
    if (!avail) { batch_t = 0; return; }
    if (!batch_t) { batch_t = now ? now : 1; return; }
    uint32_t gap = param_read(PARAM_ID_MAIL_GAP_S, &v) ? v.u * 1000u : 10000u;
    if (now - batch_t < gap || (next_try && (int32_t)(now - next_try) < 0)) return;
    item_t it[MAIL_QUEUE];
    unsigned n = avail > MAIL_QUEUE ? MAIL_QUEUE : (unsigned)avail;
    for (unsigned k = 0; k < n; ++k) it[k] = mq[(q_tail + k) % MAIL_QUEUE];
    M.last_ms = now ? now : 1;
    mail_err_t e = send_now(it, n, false);
    log_result(n, e);
    if (e == MAIL_OK) {
        M.sent++;
        M.tries = 0; next_try = 0; batch_t = 0; M.dropped = 0;
        PLAT_CRIT_ENTER(); q_tail += n; PLAT_CRIT_EXIT();
        alarm_set(ALARM_ID_NOTIFY_FAIL, false);
        return;
    }
    M.failed++;
    if (++M.tries >= TRIES) {                                      /* give up on this batch */
        PLAT_CRIT_ENTER(); q_tail += n; PLAT_CRIT_EXIT();
        M.dropped += n;
        M.tries = 0; next_try = 0; batch_t = 0;
        alarm_set(ALARM_ID_NOTIFY_FAIL, true);
    } else {
        next_try = now + mail_retry_ms * M.tries;
        if (!next_try) next_try = 1;
    }
}
