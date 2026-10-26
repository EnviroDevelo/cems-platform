/* test_tls.c - the unit's HTTPS key and certificate: made once, kept in FRAM (not in
 * backups), the same certificate every start, a new one when the address changes.
 * The handshake itself is tested against real clients in tests/integration.
 * Requirements proved here: CIP-20.6. */
#include <string.h>
#include "tst.h"
#include "tls.h"
#include "param.h"
#include "param_store.h"
#include "bsp_host.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"

static uint8_t fram[PLAT_FRAM_SIZE];
static nvm_dev_t dev;
static nvm_ram_t ram;
#define KEY_ADDR (STORE_META_ADDR + 256u)

static void cert_copy(uint8_t *out, size_t *n) { const uint8_t *c = tls_cert(n); memcpy(out, c, *n); }

static void t_key_and_cert(void) {
    memset(fram, 0, sizeof fram);
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK_EQ(tls_init(&dev, KEY_ADDR, 1234, 0xC0A80132u), 0);
    CHECK(tls_ready());
    CHECK(tls_key_was_created());
    const char *fp = tls_fingerprint();
    CHECK_EQ(strlen(fp), 95);
    CHECK(fp[2] == ':' && fp[94] != ':');
    static uint8_t c1[1024], c2[1024];
    size_t n1, n2;
    cert_copy(c1, &n1);
    CHECK(n1 > 300 && n1 < 700);

    /* what a browser sees */
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    CHECK_EQ(mbedtls_x509_crt_parse_der(&crt, c1, n1), 0);
    char dn[128];
    CHECK(mbedtls_x509_dn_gets(dn, sizeof dn, &crt.subject) > 0);
    CHECK(!strcmp(dn, "CN=CEMS Supervisor 1234, O=CEMS"));
    CHECK_EQ(crt.valid_from.year, 2025); CHECK_EQ(crt.valid_to.year, 2099);
    CHECK(mbedtls_pk_get_type(&crt.pk) == MBEDTLS_PK_ECKEY);
    mbedtls_x509_subject_alternative_name san;
    CHECK_EQ(mbedtls_x509_parse_subject_alt_name(&crt.subject_alt_names.buf, &san), 0);
    CHECK_EQ(san.type, MBEDTLS_X509_SAN_IP_ADDRESS);
    CHECK_EQ(san.san.unstructured_name.len, 4);
    CHECK(!memcmp(san.san.unstructured_name.p, "\xC0\xA8\x01\x32", 4));
    mbedtls_x509_free_subject_alt_name(&san);
    uint32_t flags = 0;                       /* self-signed: verifies against itself */
    CHECK_EQ(mbedtls_x509_crt_verify(&crt, &crt, NULL, NULL, &flags, NULL, NULL), 0);
    mbedtls_x509_crt_free(&crt);

    /* the private key is not in a parameter backup file */
    static uint8_t key_rec[40];
    memcpy(key_rec, fram + KEY_ADDR, sizeof key_rec);
    CHECK(key_rec[0] == 'T');                                  /* "TLSK" magic, little-endian */
    CHECK(memcmp(fram + KEY_ADDR + 64, key_rec, sizeof key_rec) == 0);   /* two copies */
    CHECK(!(KEY_ADDR + 128 > PLAT_FRAM_SIZE));
    CHECK(KEY_ADDR >= STORE_META_ADDR);                         /* outside both parameter images */

    /* restart: same key, and the same certificate byte for byte (deterministic signing) */
    char fp1[96]; strcpy(fp1, fp);
    CHECK_EQ(tls_init(&dev, KEY_ADDR, 1234, 0xC0A80132u), 0);
    CHECK(!tls_key_was_created());
    cert_copy(c2, &n2);
    CHECK_EQ(n2, n1); CHECK(memcmp(c1, c2, n1) == 0);
    CHECK(!strcmp(tls_fingerprint(), fp1));

    /* another address: another certificate (made at the next connection), same key */
    CHECK_EQ(tls_init(&dev, KEY_ADDR, 1234, 0x0A000005u), 0);
    CHECK(!tls_key_was_created());
    CHECK(strcmp(tls_fingerprint(), fp1) != 0);

    /* one damaged copy of the key: the other one is used */
    fram[KEY_ADDR + 10] ^= 0xFF;
    CHECK_EQ(tls_init(&dev, KEY_ADDR, 1234, 0xC0A80132u), 0);
    CHECK(!tls_key_was_created());
    CHECK(!strcmp(tls_fingerprint(), fp1));

    /* FRAM lost: a new key, so a new fingerprint (and the event log says so) */
    memset(fram + KEY_ADDR, 0, 128);
    CHECK_EQ(tls_init(&dev, KEY_ADDR, 1234, 0xC0A80132u), 0);
    CHECK(tls_key_was_created());
    CHECK(strcmp(tls_fingerprint(), fp1) != 0);

    /* no FRAM at all (RAM stand-in): works, key not kept */
    nvm_dev_t vol; nvm_ram_t vr; static uint8_t vm[PLAT_FRAM_SIZE];
    nvm_ram_init(&vol, &vr, vm, sizeof vm, false);
    CHECK_EQ(tls_init(&vol, KEY_ADDR, 1, 0), 0);
    CHECK(tls_key_was_created());
    CHECK(vm[KEY_ADDR] == 0);
    tls_stats_t s = tls_stats();
    CHECK(s.pool_peak > 0 && s.pool_peak < TLS_POOL_SIZE);
    printf("  TLS pool peak after key/cert work: %u of %u bytes\n", s.pool_peak, (unsigned)TLS_POOL_SIZE);
}

void test_tls_all(void) {
    RUN(t_key_and_cert);
}
