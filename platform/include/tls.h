/* tls.h - HTTPS for the web server (decision D11): the unit's own key and
 * certificate, and TLS 1.2 server connections (TLS_CONNS of them open together).
 *
 * Key: an ECDSA P-256 key made by the unit at its first start from the hardware
 * random-number generator and kept in FRAM (META area, two copies, never in a
 * parameter backup file), so each unit has its own and it survives firmware updates.
 *
 * Certificate: self-signed, made from that key at every start. Signing is
 * deterministic (RFC 6979), so the certificate, and its fingerprint, stay the same
 * from start to start as long as the serial number and IP address do; it is remade
 * when the address changes. Subject "CEMS Supervisor <serial>", the unit's IP address
 * as subject-alternative name, valid 2025-2099. Browsers warn once ("not trusted")
 * until the certificate is trusted: either accept it, or import it
 * (GET /api/tls/cert) on the site's laptops. The fingerprint is on the touchscreen's
 * System screen and the web page, so a technician can check they reached the real
 * unit. No production step and no company certificate authority are needed (D11).
 *
 * Threading: tls_init before the tasks start; everything else from the HTTP task. */
#ifndef TLS_H
#define TLS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nvm.h"

#ifndef TLS_CONNS
#define TLS_CONNS 2u                   /* server connections open together (one per kept browser connection) */
#endif
#ifndef TLS_POOL_SIZE
#define TLS_POOL_SIZE (60u * 1024u)    /* each server connection holds ~21 KB of buffers (16 KB in / 4 KB out) from start:
                                          measured peak 28 KB for one, so ~49 KB for two; the mail client (32 KB RSA-2048,
                                          38 KB RSA-4096) runs with the server connections freed */
#endif

void tls_crypto_init(void);                 /* the mbedTLS memory pool (once; tls_init does it too) */
int  tls_init(nvm_dev_t *fram, uint32_t key_addr, uint32_t serial, uint32_t ip);   /* 0 = ok */
bool tls_ready(void);
bool tls_key_was_created(void);        /* this start made a new key (first start, or FRAM lost) */

/* current certificate (DER) and its SHA-256 fingerprint "AB:CD:..." (95 chars + NUL) */
const uint8_t *tls_cert(size_t *len);
const char    *tls_fingerprint(void);
void           tls_set_ip(uint32_t ip);    /* address changed: the next connection gets a new certificate */

/* Server connections: tls_use(k) picks connection k (0..TLS_CONNS-1) for the calls
 * below, which then act on it alone: handshake on an accepted socket, read/write,
 * close always. Connections are independent; only one is in use at a time (one task). */
void tls_use(unsigned k);
int  tls_accept(int fd);                    /* 0 = ok */
int  tls_read(uint8_t *buf, uint32_t cap);  /* bytes, <= 0 on close/timeout/error */
int  tls_write(const uint8_t *p, uint32_t n);   /* 0 = all sent */
void tls_close(void);
bool tls_resumed(void);                     /* the last handshake resumed a session (ticket) */
bool tls_pending(void);                     /* decrypted bytes are waiting (a request already received) */

/* client, for alarm email (same task as the server, between its connections): TLS 1.2
 * on a connected socket; peer_fp = SHA-256 of the server's certificate, which the
 * caller compares with the pinned one. Then tls_read/tls_write; tls_client_close
 * always (it gives the context back to the server). */
int  tls_client_open(int fd, uint8_t peer_fp[32]);   /* 0 = ok */
void tls_client_close(void);

typedef struct { uint32_t handshakes, resumed, failed, pool_peak; } tls_stats_t;
tls_stats_t tls_stats(void);

#endif
