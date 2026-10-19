/* tls.c - see tls.h */
#include <string.h>
#include <stdio.h>
#include "tls.h"
#include "plat_net.h"
#include "bsp.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/platform.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_ticket.h"
#include "mbedtls/sha256.h"
#include "mbedtls/entropy.h"

#define KEY_MAGIC 0x4B534C54u   /* "TLSK" */

static uint8_t pool[TLS_POOL_SIZE];
static mbedtls_entropy_context ent;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_pk_context key;
static mbedtls_x509_crt crt;
static mbedtls_ssl_config conf;
static mbedtls_ssl_context ssl;
static mbedtls_ssl_ticket_context ticket;
static uint8_t der[1024];
static size_t der_len;
static const uint8_t *der_p;
static char fp[96];
static uint32_t cert_ip, cert_serial, want_ip;
static bool ready, created, resumed, conf_done;
static int sock = -1;
static tls_stats_t st;

/* ---- glue ---- */
int mbedtls_hardware_poll(void *data, unsigned char *out, size_t len, size_t *olen) {
    (void)data;
    bsp_random(out, (uint32_t)len);
    *olen = len;
    return 0;
}
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)bsp_ms(); }

static mbedtls_time_t up_time(mbedtls_time_t *t) {
    mbedtls_time_t v = (mbedtls_time_t)(bsp_ms() / 1000u);
    if (t) *t = v;
    return v;
}

/* ---- key in FRAM: { magic, d[32], crc } at +0 and +64 ---- */
typedef struct { uint32_t magic; uint8_t d[32]; uint32_t crc; } key_rec_t;

static bool key_load(nvm_dev_t *f, uint32_t a, uint8_t d[32]) {
    for (unsigned c = 0; f && c < 2u; ++c) {
        key_rec_t r;
        if (f->read(f, a + c * 64u, &r, sizeof r) != 0) continue;
        if (r.magic != KEY_MAGIC || r.crc != plat_crc32(0, &r, (uint32_t)offsetof(key_rec_t, crc))) continue;
        memcpy(d, r.d, 32);
        memset(&r, 0, sizeof r);
        return true;
    }
    return false;
}
static void key_save(nvm_dev_t *f, uint32_t a, const uint8_t d[32]) {
    if (!f) return;
    key_rec_t r = { .magic = KEY_MAGIC };
    memcpy(r.d, d, 32);
    r.crc = plat_crc32(0, &r, (uint32_t)offsetof(key_rec_t, crc));
    f->write(f, a, &r, sizeof r);
    f->write(f, a + 64u, &r, sizeof r);
    memset(&r, 0, sizeof r);
}

/* ---- certificate ---- */
static int make_cert(void) {
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    char name[96];
    snprintf(name, sizeof name, "CN=CEMS Supervisor %lu,O=CEMS", (unsigned long)cert_serial);
    uint8_t serial[8] = { 0x01, (uint8_t)(cert_serial >> 24), (uint8_t)(cert_serial >> 16), (uint8_t)(cert_serial >> 8),
                          (uint8_t)cert_serial, (uint8_t)(want_ip >> 16), (uint8_t)(want_ip >> 8), (uint8_t)want_ip };
    uint8_t ipb[4] = { (uint8_t)(want_ip >> 24), (uint8_t)(want_ip >> 16), (uint8_t)(want_ip >> 8), (uint8_t)want_ip };
    mbedtls_x509_san_list san;
    memset(&san, 0, sizeof san);
    san.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    san.node.san.unstructured_name.p = ipb;
    san.node.san.unstructured_name.len = 4;
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &key);
    mbedtls_x509write_crt_set_issuer_key(&w, &key);
    int rc = mbedtls_x509write_crt_set_subject_name(&w, name);
    if (!rc) rc = mbedtls_x509write_crt_set_issuer_name(&w, name);
    if (!rc) rc = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial);
    if (!rc) rc = mbedtls_x509write_crt_set_validity(&w, "20250101000000", "20991231235959");
    if (!rc) rc = mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1);
    if (!rc) rc = mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (!rc && want_ip) rc = mbedtls_x509write_crt_set_subject_alternative_name(&w, &san);
    int n = rc ? rc : mbedtls_x509write_crt_der(&w, der, sizeof der, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_x509write_crt_free(&w);
    if (n <= 0) return n ? n : -1;
    der_len = (size_t)n;
    der_p = der + sizeof der - der_len;                 /* written at the end of the buffer */
    mbedtls_x509_crt_free(&crt);
    mbedtls_x509_crt_init(&crt);
    rc = mbedtls_x509_crt_parse_der(&crt, der_p, der_len);
    if (rc) return rc;
    uint8_t h[32];
    mbedtls_sha256(der_p, der_len, h, 0);
    static const char hx[] = "0123456789ABCDEF";
    for (int k = 0; k < 32; ++k) {
        fp[3 * k] = hx[h[k] >> 4]; fp[3 * k + 1] = hx[h[k] & 15]; fp[3 * k + 2] = k < 31 ? ':' : 0;
    }
    cert_ip = want_ip;
    if (conf_done) rc = mbedtls_ssl_conf_own_cert(&conf, &crt, &key);
    return rc;
}

static int send_cb(void *ctx, const unsigned char *p, size_t n) {
    (void)ctx;
    ssize_t w = send(sock, p, n, 0);
    return w > 0 ? (int)w : -0x004E /* MBEDTLS_ERR_NET_SEND_FAILED */;
}
static int recv_cb(void *ctx, unsigned char *p, size_t n) {
    (void)ctx;
    ssize_t r = recv(sock, p, n, 0);
    if (r > 0) return (int)r;
    return r == 0 ? MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY : MBEDTLS_ERR_SSL_TIMEOUT;
}

/* ticket parse that notes a resumed session (no public-key work this time) */
static int ticket_parse(void *p, mbedtls_ssl_session *s, unsigned char *buf, size_t len) {
    int rc = mbedtls_ssl_ticket_parse(p, s, buf, len);
    if (rc == 0) resumed = true;
    return rc;
}

int tls_init(nvm_dev_t *fram, uint32_t key_addr, uint32_t serial, uint32_t ip) {
    ready = created = conf_done = false;
    mbedtls_memory_buffer_alloc_init(pool, sizeof pool);
    mbedtls_platform_set_time(up_time);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509_crt_init(&crt);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_ticket_init(&ticket);
    int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"cems-tls", 8);
    if (rc) return rc;
    rc = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc) return rc;
    uint8_t d[32];
    if (fram && fram->persistent && key_load(fram, key_addr, d)) {
        rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), d, 32);
        if (!rc) rc = mbedtls_ecp_keypair_calc_public(mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg);
    } else {
        rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_ctr_drbg_random, &drbg);
        if (!rc) rc = mbedtls_ecp_write_key_ext(mbedtls_pk_ec(key), &(size_t){ 0 }, d, 32);
        if (!rc) { key_save(fram && fram->persistent ? fram : NULL, key_addr, d); created = true; }
    }
    memset(d, 0, sizeof d);
    if (rc) return rc;
    cert_serial = serial;
    want_ip = ip;
    if ((rc = make_cert()) != 0) return rc;
    rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) return rc;
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    rc = mbedtls_ssl_ticket_setup(&ticket, mbedtls_ctr_drbg_random, &drbg, MBEDTLS_CIPHER_AES_128_GCM, 24u * 3600u);
    if (rc) return rc;
    mbedtls_ssl_conf_session_tickets_cb(&conf, mbedtls_ssl_ticket_write, ticket_parse, &ticket);
    conf_done = true;
    if ((rc = mbedtls_ssl_conf_own_cert(&conf, &crt, &key)) != 0) return rc;
    if ((rc = mbedtls_ssl_setup(&ssl, &conf)) != 0) return rc;
    mbedtls_ssl_set_bio(&ssl, NULL, send_cb, recv_cb, NULL);
    ready = true;
    return 0;
}

bool tls_ready(void) { return ready; }
bool tls_key_was_created(void) { return created; }
const uint8_t *tls_cert(size_t *len) { *len = ready ? der_len : 0; return der_p; }
const char *tls_fingerprint(void) { return ready ? fp : ""; }
void tls_set_ip(uint32_t ip) { want_ip = ip; }

int tls_accept(int fd) {
    if (!ready) return -1;
    if (want_ip != cert_ip && make_cert() != 0) return -1;
    sock = fd;
    resumed = false;
    mbedtls_ssl_session_reset(&ssl);
    int rc;
    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) { st.failed++; return rc; }
    }
    st.handshakes++;
    if (resumed) st.resumed++;
    return 0;
}

int tls_read(uint8_t *buf, uint32_t cap) {
    int r;
    do r = mbedtls_ssl_read(&ssl, buf, cap); while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    return r;
}

int tls_write(const uint8_t *p, uint32_t n) {
    while (n) {
        int w = mbedtls_ssl_write(&ssl, p, n);
        if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (w <= 0) return -1;
        p += w; n -= (uint32_t)w;
    }
    return 0;
}

void tls_close(void) {
    if (sock >= 0) (void)mbedtls_ssl_close_notify(&ssl);
    sock = -1;
}

bool tls_resumed(void) { return resumed; }

tls_stats_t tls_stats(void) {
    size_t peak = 0, blocks = 0;
    mbedtls_memory_buffer_alloc_max_get(&peak, &blocks);
    st.pool_peak = (uint32_t)peak;
    return st;
}
