/* mail.h - alarm email (design spec "Alarms"; Gary 20 Oct: each alarm can email,
 * show on Modbus, drive an output, or do nothing, as the user sets it).
 *
 * An alarm whose actions include AR_EMAIL (plat_alarm.h) queues its raise (and its
 * clear, if mail_on_clear) here. The queue is collected for mail_gap seconds so a
 * burst of alarms is one email, then sent to the site's mail server (SMTP):
 *
 *   security  none      plain SMTP, normally port 25; no login allowed (it would go in clear)
 *             STARTTLS  port 587: plain, then TLS 1.2
 *             TLS       port 465: TLS 1.2 from the start
 *   trust     the server's certificate is pinned by its SHA-256 fingerprint (parameter
 *             74). "Send test email" on the web page shows the fingerprint the server
 *             presented; "Trust this server" stores it. With no pin the link is
 *             encrypted but the server is not checked.
 *   login     AUTH PLAIN (or LOGIN) with mail_user / mail_password (write-only).
 *
 * A failed send is retried after 1 and then 2 more minutes; after the third failure the batch
 * is dropped and alarm 10 NOTIFY_FAIL raised (it clears at the next success). Every
 * attempt's outcome is in the event log. Addressed by IP: sites seldom have DNS.
 *
 * With mail off (the default), nothing is queued and the instrument makes no outbound
 * connection at all (D17).
 *
 * Threading: mail_queue_alarm from the scan task (via plat_alarm_email); everything
 * else from the HTTP task, which runs mail_service between web requests: the TLS
 * code has one context and one memory pool, shared with the web server. */
#ifndef MAIL_H
#define MAIL_H
#include <stdint.h>
#include <stdbool.h>

#define MAIL_QUEUE 16u

typedef enum {
    MAIL_OK = 0, MAIL_E_OFF, MAIL_E_CONFIG, MAIL_E_CONNECT, MAIL_E_TLS, MAIL_E_PIN, MAIL_E_PROTOCOL,
    MAIL_E_AUTH, MAIL_E_REFUSED, MAIL_E_NOTLS_LOGIN, MAIL_E_BUSY
} mail_err_t;

typedef struct {
    bool       busy;            /* a batch is waiting or being sent */
    uint8_t    queued;          /* alarm events waiting */
    uint8_t    tries;           /* failed attempts of the current batch */
    uint32_t   sent, failed, dropped;   /* emails sent, attempts failed, events dropped */
    mail_err_t last;            /* last attempt's result */
    char       last_text[64];   /* ... in words (the server's reply on a refusal) */
    uint32_t   last_ms;         /* uptime of the last attempt (0 = none) */
    bool       have_fp;         /* the last TLS connection's server fingerprint */
    uint8_t    server_fp[32];
    bool       test_pending;
} mail_status_t;

void mail_init(const char *product);    /* the product name in the From line and the footer */
void mail_service(uint32_t now_ms, void (*alive)(void));   /* HTTP task, between requests */
int  mail_request_test(void);           /* send a test email at the next service; 0 ok, -1 mail off */
void mail_status(mail_status_t *s);
const char *mail_err_name(mail_err_t e);

/* for tests */
extern uint32_t mail_retry_ms;          /* 60 s; the second retry waits twice as long */
void mail_reset_for_test(void);

#endif
