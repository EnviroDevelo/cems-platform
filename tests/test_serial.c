/* test_serial.c - serial number changes need a developer token (plat_serial.h, D25). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tst.h"
#include "plat_serial.h"
#include "plat_sha256.h"
#include "http_util.h"
#include "http_api.h"
#include "web_auth.h"
#include "param.h"
#include "param_store.h"
#include "bsp_host.h"
#include "tls.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"

static uint8_t fram[128 * 1024]; static nvm_ram_t ram; static nvm_dev_t dev;
static plat_cmdq_t q; static plat_seqlock_t lk;
static uint32_t tnow = 50000;
static mbedtls_ecdsa_context sk[2];
static mbedtls_ctr_drbg_context rng;
static uint8_t pub[2][65];

static void scan_once(void) {
    plat_cmd_t c; while (plat_cmdq_pop(&q, &c)) (void)param_apply(&c);
    param_blob_drain(); param_store_scan_service(); param_publish();
}
static void store_tick(void) { tnow += 10; param_store_poll(tnow); scan_once(); param_store_poll(tnow); }
static void settle(void) { for (int k = 0; k < 3; ++k) store_tick(); tnow += 2000; store_tick(); store_tick(); }

static void boot(void) {
    nvm_ram_init(&dev, &ram, fram, sizeof fram, true);
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    CHECK(param_store_init(&dev, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    plat_cmdq_init(&q); lk.seq = 0;
    param_bind(&q, &lk);
    auth_init();
    param_publish();
}

static void sign(int which, uint32_t new_serial, uint8_t sig[64]) {
    char msg[SERIAL_MSG_MAX];
    size_t n = plat_serial_message(new_serial, msg, sizeof msg);
    uint8_t h[32]; sha256_t s; sha256_init(&s); sha256_update(&s, msg, n); sha256_final(&s, h);
    mbedtls_mpi r, ss; mbedtls_mpi_init(&r); mbedtls_mpi_init(&ss);
    CHECK_EQ(mbedtls_ecdsa_sign(&sk[which].MBEDTLS_PRIVATE(grp), &r, &ss, &sk[which].MBEDTLS_PRIVATE(d), h, 32,
                                mbedtls_ctr_drbg_random, &rng), 0);
    mbedtls_mpi_write_binary(&r, sig, 32); mbedtls_mpi_write_binary(&ss, sig + 32, 32);
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&ss);
}

static void keys(void) {
    static mbedtls_entropy_context ent;
    tls_crypto_init();
    mbedtls_entropy_init(&ent); mbedtls_ctr_drbg_init(&rng);
    CHECK_EQ(mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &ent, NULL, 0), 0);
    for (int k = 0; k < 2; ++k) {
        mbedtls_ecdsa_init(&sk[k]);
        CHECK_EQ(mbedtls_ecdsa_genkey(&sk[k], MBEDTLS_ECP_DP_SECP256R1, mbedtls_ctr_drbg_random, &rng), 0);
        size_t n = 0;
        CHECK_EQ(mbedtls_ecp_point_write_binary(&sk[k].MBEDTLS_PRIVATE(grp), &sk[k].MBEDTLS_PRIVATE(Q),
                                                MBEDTLS_ECP_PF_UNCOMPRESSED, &n, pub[k], 65), 0);
    }
}

static void t_ordinary_writes_refused(void) {
    memset(fram, 0, sizeof fram);
    boot();
    plat_val_t v = { .u = 1234 };
    const uint8_t srcs[] = { PLAT_SRC_LOCAL, PLAT_SRC_WEB, PLAT_SRC_MODBUS };
    for (size_t k = 0; k < 3; ++k)
        for (uint8_t l = PLAT_LVL_VIEW; l <= PLAT_LVL_FACTORY; ++l) {
            CHECK_EQ(param_check(PARAM_ID_SERIAL, PLAT_VAL_U32, v, srcs[k], l), PERR_READONLY);
            plat_cmd_t c = { .id = PARAM_ID_SERIAL, .src = srcs[k], .level = l, .kind = PLAT_VAL_U32, .v = v };
            CHECK_EQ(param_apply(&c), PERR_READONLY);
        }
    CHECK_EQ(plat_serial(), 0u);
    CHECK(param_find(PARAM_ID_SERIAL)->flags & PF_DEV);
}

static void t_message(void) {
    char m[SERIAL_MSG_MAX], uid[25];
    plat_serial_uid_hex(uid);
    CHECK(!strcmp(uid, "00112233445566778899aabb"));        /* the host's stand-in chip ID */
    CHECK(plat_serial_message(240117, m, sizeof m) > 0);
    CHECK(!strcmp(m, "CEMS-SERIAL v1 uid=00112233445566778899aabb old=0 new=240117"));
    CHECK_EQ(plat_serial_message(1, m, 20), 0u);              /* no room: nothing */
}

static void t_assign(void) {
    memset(fram, 0, sizeof fram);
    boot();
    keys();
    uint8_t sig[64];
    sign(0, 240117, sig);
    /* this firmware has no developer key: nothing can change it, whatever the token */
    CHECK(!plat_serial_key_present());
    CHECK_EQ(plat_serial_assign(240117, sig, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_E_NO_KEY);
    plat_serial_key_override_for_test(pub[0]);               /* a production build */
    CHECK(plat_serial_key_present());
    CHECK_EQ(plat_serial_assign(240117, sig, PLAT_SRC_WEB, PLAT_LVL_ADMIN), SER_E_LEVEL);
    uint8_t other[64]; sign(1, 240117, other);
    CHECK_EQ(plat_serial_assign(240117, other, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_E_SIG);   /* another key */
    CHECK_EQ(plat_serial_assign(240118, sig, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_E_SIG);     /* another serial */
    uint8_t bad[64]; memcpy(bad, sig, 64); bad[10] ^= 1;
    CHECK_EQ(plat_serial_assign(240117, bad, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_E_SIG);
    CHECK_EQ(plat_serial_assign(0, sig, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_E_SAME);
    CHECK_EQ(plat_serial_assign(240117, sig, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_OK);
    settle();
    CHECK_EQ(plat_serial(), 240117u);
    /* the same token again: "old" no longer matches */
    CHECK_EQ(plat_serial_assign(240117, sig, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_E_SAME);
    /* on another chip the token for this one is useless */
    setenv("CEMS_UID", "ffeeddccbbaa998877665544", 1);
    sign(0, 7, sig);                                         /* signed for the other chip, old=240117 */
    unsetenv("CEMS_UID");
    CHECK_EQ(plat_serial_assign(7, sig, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_E_SIG);
    /* survives a power cut (factory region) */
    boot();
    CHECK_EQ(plat_serial(), 240117u);
    plat_serial_key_override_for_test(NULL);
    for (int k = 0; k < 2; ++k) mbedtls_ecdsa_free(&sk[k]);
    mbedtls_ctr_drbg_free(&rng);
}

/* a backup file carries the serial but never gives it to a unit, even at factory level */
static void t_backup(void) {
    static uint8_t file[16 * 1024];
    memset(fram, 0, sizeof fram);
    boot();
    keys();
    plat_serial_key_override_for_test(pub[0]);
    uint8_t sig[64]; sign(0, 555, sig);
    CHECK_EQ(plat_serial_assign(555, sig, PLAT_SRC_WEB, PLAT_LVL_FACTORY), SER_OK);
    settle();
    uint32_t n = param_store_export(file, sizeof file);
    CHECK(n > 0);
    memset(fram, 0, sizeof fram);                            /* another unit (or this one, wiped) */
    boot();
    CHECK_EQ(plat_serial(), 0u);
    CHECK_EQ(param_store_import(file, n, PLAT_LVL_FACTORY), 0);
    param_store_scan_service(); param_publish();
    settle();
    CHECK_EQ(plat_serial(), 0u);
    plat_serial_key_override_for_test(NULL);
    for (int k = 0; k < 2; ++k) mbedtls_ecdsa_free(&sk[k]);
    mbedtls_ctr_drbg_free(&rng);
}

/* the web API: GET shows the chip ID and the text to sign; POST needs factory and the token */
static char cap[32 * 1024]; static size_t capn;
static int cap_send(void *ctx, const void *p, size_t n) {
    (void)ctx; if (capn + n >= sizeof cap) return -1;
    memcpy(cap + capn, p, n); capn += n; cap[capn] = 0; return 0;
}
static char sid[AUTH_TOKEN_LEN + 1];
static uint8_t scratch[20 * 1024];
static int call(const char *method, const char *path, const char *body) {
    static char raw[4096];
    size_t bl = body ? strlen(body) : 0;
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nHost: unit\r\nX-CEMS: 1\r\nConnection: close\r\n%s%s%sContent-Length: %zu\r\n\r\n%s",
             method, path, sid[0] ? "Cookie: sid=" : "", sid, sid[0] ? "\r\n" : "", bl, body ? body : "");
    http_req_t r;
    int h = http_parse_head(raw, (uint32_t)strlen(raw), &r);
    if (h <= 0) return -1;
    r.body = (const uint8_t *)raw + h; r.body_len = r.content_length;
    http_out_t o; out_init(&o, cap_send, NULL); capn = 0;
    http_handle(&r, &o, tnow);
    int st = 0; sscanf(cap, "HTTP/1.1 %d", &st);
    const char *ck = strstr(cap, "Set-Cookie: sid=");
    if (ck) { ck += 16; if (*ck == ';') sid[0] = 0; else { memcpy(sid, ck, AUTH_TOKEN_LEN); sid[AUTH_TOKEN_LEN] = 0; } }
    return st;
}
static bool has(const char *s) { return strstr(cap, s) != NULL; }

static void t_web(void) {
    memset(fram, 0, sizeof fram);
    boot();
    keys();
    http_cfg_t c = { .product = "CEMS Supervisor", .fw_version = "0.1.0-test", .scratch = scratch,
                     .scratch_size = sizeof scratch, .wait = store_tick };
    http_api_init(&c);
    sid[0] = 0;
    CHECK_EQ(call("GET", "/api/serial?new=42", NULL), 200);
    CHECK(has("\"serial\":0")); CHECK(has("\"uid\":\"00112233445566778899aabb\"")); CHECK(has("\"key\":false"));
    CHECK(has("\"message\":\"CEMS-SERIAL v1 uid=00112233445566778899aabb old=0 new=42\""));
    uint8_t sig[64]; char hex[129], body[256];
    plat_serial_key_override_for_test(pub[0]);
    sign(0, 42, sig); hex_encode(sig, 64, hex);
    snprintf(body, sizeof body, "serial=42&sig=%s", hex);
    CHECK_EQ(call("POST", "/api/serial", body), 401);                     /* not logged in */
    CHECK_EQ(call("POST", "/api/login", "level=admin&password=2222"), 200);
    CHECK_EQ(call("POST", "/api/serial", body), 403); CHECK(has("factory login"));
    CHECK_EQ(call("POST", "/api/login", "level=factory&password=9999"), 200);
    CHECK_EQ(call("POST", "/api/serial", "serial=42&sig=00"), 400);
    CHECK_EQ(call("POST", "/api/serial", "serial=-1&sig=00"), 400);
    snprintf(body, sizeof body, "serial=43&sig=%s", hex);
    CHECK_EQ(call("POST", "/api/serial", body), 403); CHECK(has("token not valid"));
    /* the ordinary write path is refused even at factory level */
    CHECK_EQ(call("POST", "/api/write", "id=50&v=42"), 403);
    snprintf(body, sizeof body, "serial=42&sig=%s", hex);
    CHECK_EQ(call("POST", "/api/serial", body), 200); CHECK(has("\"serial\":42"));
    settle();
    CHECK_EQ(call("GET", "/api/serial", NULL), 200); CHECK(has("\"serial\":42")); CHECK(has("\"key\":true"));
    CHECK(!has("message"));
    CHECK_EQ(call("GET", "/api/params", NULL), 200); CHECK(has("\"dev\""));
    plat_serial_key_override_for_test(NULL);
    for (int k = 0; k < 2; ++k) mbedtls_ecdsa_free(&sk[k]);
    mbedtls_ctr_drbg_free(&rng);
}

void test_serial_all(void) {
    RUN(t_ordinary_writes_refused);
    RUN(t_message);
    RUN(t_assign);
    RUN(t_backup);
    RUN(t_web);
}
