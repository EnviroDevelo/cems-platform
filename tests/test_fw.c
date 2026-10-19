/* test_fw.c - firmware upload, staging, activation, trial, confirm and rollback. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "tst.h"
#include "fw_update.h"
#include "fw_flash_sim.h"
#include "plat_sha256.h"
#include "plat_log.h"
#include "http_api.h"
#include "web_auth.h"
#include "param.h"
#include "param_store.h"
#include "bsp_host.h"

#define BANK (128u * 1024u)
static uint8_t b0[BANK], b1[BANK];
static fw_flash_t fl;
static fw_flash_sim_t fs;
static uint8_t fr_mem[512];
static nvm_dev_t fr;
static nvm_ram_t fr_ram;

/* the image file the tool would make */
static uint8_t file[BANK];
static uint32_t make_image(uint32_t payload, const char *ver, uint32_t product, uint32_t seed) {
    fw_header_t h;
    memset(&h, 0, sizeof h);
    h.magic = FW_MAGIC; h.hdr_size = FW_HDR_SIZE; h.hdr_version = 1;
    h.product = product; h.version = fw_parse_version(ver);
    strncpy(h.version_str, ver, sizeof h.version_str - 1);
    h.image_size = payload; h.build_time = 1792411200u;
    uint8_t *p = file + FW_HDR_SIZE;
    uint32_t x = seed;
    for (uint32_t k = 0; k < payload; ++k) { x = x * 1103515245u + 12345u; p[k] = (uint8_t)(x >> 16); }
    sha256_t s; sha256_init(&s); sha256_update(&s, p, payload); sha256_final(&s, h.sha256);
    h.hdr_crc = plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc));
    memcpy(file, &h, sizeof h);
    return FW_HDR_SIZE + payload;
}
static void reheader(void) {      /* recompute the CRC after editing the header in file[] */
    fw_header_t h; memcpy(&h, file, sizeof h);
    h.hdr_crc = plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc));
    memcpy(file, &h, sizeof h);
}

static fw_err_t upload(uint32_t len, uint32_t max_chunk) {
    fw_err_t e = fw_upload_begin(len);
    if (e != FW_OK) return e;
    for (uint32_t k = 0; k < len; ) {
        uint32_t n = 1u + (uint32_t)rand() % max_chunk;
        if (n > len - k) n = len - k;
        e = fw_upload_data(file + k, n);
        if (e != FW_OK) return e;
        k += n;
    }
    return fw_upload_end();
}

static void power_up(const char *running, uint8_t cause) {     /* a (re)boot of the unit */
    fw_reset_for_test();
    fs.reset_requested = false;
    fw_init(&fl, &fr, 128, running);
    fw_boot(cause, bsp_host.ms);
}

static void t_header_helpers(void) {
    CHECK_EQ(fw_parse_version("1.2.3"), 0x010203);
    CHECK_EQ(fw_parse_version("0.4.0-dev"), 0x000400);
    CHECK_EQ(fw_parse_version("12"), 0x0C0000);
    CHECK_EQ(fw_parse_version(""), 0);
    make_image(1000, "0.4.0", PLAT_FW_PRODUCT, 1);
    fw_header_t h; memcpy(&h, file, sizeof h);
    CHECK(fw_header_ok(&h));
    h.version ^= 1; CHECK(!fw_header_ok(&h));
}

static void t_upload(void) {
    fw_flash_sim_init(&fl, &fs, b0, b1, BANK);
    memset(b0, 0xAB, BANK); memset(b1, 0x00, BANK);         /* free bank holds rubbish */
    nvm_ram_init(&fr, &fr_ram, fr_mem, sizeof fr_mem, true);
    memset(fr_mem, 0, sizeof fr_mem);
    srand(7);
    power_up("0.3.0", BSP_RESET_POWER_ON);
    fw_status_t st;
    fw_status(&st);
    CHECK(!st.staged); CHECK_EQ(st.state, FW_ST_IDLE); CHECK(!strcmp(st.running_version, "0.3.0"));

    uint32_t len = make_image(100000u + 7u, "0.4.0", PLAT_FW_PRODUCT, 2);   /* not a multiple of 16 */
    CHECK_EQ(upload(len, 3000), FW_OK);
    fw_status(&st);
    CHECK(st.staged); CHECK(!strcmp(st.staged_version, "0.4.0")); CHECK(!st.uploading);
    CHECK(memcmp(fw_flash_sim_free(&fs), file + FW_HDR_SIZE, len - FW_HDR_SIZE) == 0);
    CHECK(memcmp(fw_flash_sim_free(&fs) + BANK - 8192u, file, FW_HDR_SIZE) == 0);   /* trailer */
    CHECK(memcmp(fw_flash_sim_active(&fs), b0, 16) == 0 && b0[0] == 0xAB);         /* running bank untouched */
    CHECK_EQ(fs.violations, 0);
    /* still staged after a restart (read from the trailer) */
    power_up("0.3.0", BSP_RESET_POWER_ON);
    fw_status(&st); CHECK(st.staged); CHECK(!strcmp(st.staged_version, "0.4.0"));

    /* one byte at a time works too */
    len = make_image(5000, "0.4.1", PLAT_FW_PRODUCT, 3);
    CHECK_EQ(upload(len, 1), FW_OK);

    /* refusals: each leaves nothing staged */
    len = make_image(5000, "0.4.2", PLAT_FW_PRODUCT, 4);
    file[FW_HDR_SIZE + 4000] ^= 0x40;                        /* damaged in transfer */
    CHECK_EQ(upload(len, 700), FW_E_HASH);
    fw_status(&st); CHECK(!st.staged); CHECK_EQ(st.last_error, FW_E_HASH);
    CHECK_EQ(fw_request_activate(), FW_E_NO_IMAGE);

    len = make_image(5000, "0.4.2", 0x12345678u, 4);         /* other hardware */
    CHECK_EQ(upload(len, 700), FW_E_PRODUCT);
    len = make_image(5000, "0.4.2", PLAT_FW_PRODUCT, 4);
    file[3] ^= 1;                                            /* not an image */
    CHECK_EQ(upload(len, 700), FW_E_HEADER);
    len = make_image(5000, "0.4.2", PLAT_FW_PRODUCT, 4);
    CHECK_EQ(upload(len - 1, 700), FW_E_SIZE);               /* length disagrees with the header */
    CHECK_EQ(fw_upload_begin(BANK), FW_E_SIZE);              /* cannot fit beside the trailer */
    CHECK_EQ(fw_upload_begin(10), FW_E_SIZE);
    len = make_image(5000, "0.4.2", PLAT_FW_PRODUCT, 4);
    { fw_header_t h; memcpy(&h, file, sizeof h); h.flags = FW_F_SIGNED; memcpy(file, &h, sizeof h); reheader(); }
    CHECK_EQ(upload(len, 700), FW_E_SIGNATURE);              /* signed, but no key installed */
    /* busy, out of order, abort */
    CHECK_EQ(fw_upload_data(file, 10), FW_E_STATE);
    CHECK_EQ(fw_upload_end(), FW_E_STATE);
    len = make_image(5000, "0.4.3", PLAT_FW_PRODUCT, 5);
    CHECK_EQ(fw_upload_begin(len), FW_OK);
    CHECK_EQ(fw_upload_begin(len), FW_E_BUSY);
    CHECK_EQ(fw_upload_data(file, 300), FW_OK);
    fw_status(&st); CHECK(st.uploading); CHECK_EQ(st.upload_bytes, 300); CHECK_EQ(st.upload_total, len);
    CHECK_EQ(fw_request_activate(), FW_E_BUSY);
    fw_upload_abort();
    fw_status(&st); CHECK(!st.uploading); CHECK(!st.staged);
    /* flash write failure part way */
    fs.fail_after_writes = 100;
    CHECK_EQ(upload(len, 700), FW_E_FLASH);
    fs.fail_after_writes = -1;
    /* and a good one again */
    CHECK_EQ(upload(len, 4096), FW_OK);
    CHECK_EQ(fs.violations, 0);
}

static void t_activate_confirm(void) {
    fw_status_t st;
    uint32_t len = make_image(60000, "0.5.0", PLAT_FW_PRODUCT, 9);
    power_up("0.4.0", BSP_RESET_POWER_ON);
    CHECK_EQ(upload(len, 2048), FW_OK);
    uint32_t swaps = fs.swaps;
    CHECK_EQ(fw_request_activate(), FW_OK);
    fw_service(bsp_host.ms, true);
    CHECK_EQ(fs.swaps, swaps + 1);                           /* swapped and "reset" */
    CHECK(memcmp(fw_flash_sim_active(&fs), file + FW_HDR_SIZE, 60000) == 0);   /* the new image now runs */

    fw_trial_ms = 60000;
    power_up("0.5.0", BSP_RESET_SOFTWARE);                   /* first start of the new image */
    fw_status(&st); CHECK_EQ(st.state, FW_ST_TRIAL); CHECK_EQ(st.trial_left_ms, 60000);
    CHECK_EQ(fw_request_activate(), FW_E_STATE);             /* no second swap during a trial */
    uint32_t t = bsp_host.ms;
    fw_service(t, true);
    fw_service(t + 30000, true);
    fw_status(&st); CHECK_EQ(st.trial_left_ms, 30000);
    fw_service(t + 31000, false);                            /* unhealthy: the clock starts again */
    fw_service(t + 32000, true);
    fw_service(t + 91000, true);
    fw_status(&st); CHECK_EQ(st.state, FW_ST_TRIAL); CHECK_EQ(st.trial_left_ms, 1000);
    fw_service(t + 92000, true);
    fw_status(&st); CHECK_EQ(st.state, FW_ST_IDLE);          /* confirmed */
    CHECK(!st.staged);       /* the previous image was loaded with a debugger: no trailer, so not offered */
    /* confirmed survives restarts */
    power_up("0.5.0", BSP_RESET_IWDG);
    fw_status(&st); CHECK_EQ(st.state, FW_ST_IDLE); CHECK_EQ(fs.swaps, swaps + 1);
}

static void t_rollback(void) {
    fw_status_t st;
    uint32_t len = make_image(30000, "0.6.0", PLAT_FW_PRODUCT, 11);
    power_up("0.5.0", BSP_RESET_POWER_ON);
    CHECK_EQ(upload(len, 2048), FW_OK);
    CHECK_EQ(fw_request_activate(), FW_OK);
    uint32_t swaps = fs.swaps;
    fw_service(bsp_host.ms, true);
    CHECK_EQ(fs.swaps, swaps + 1);
    power_up("0.6.0", BSP_RESET_SOFTWARE);                   /* trial starts */
    /* one power cut during the trial is tolerated */
    power_up("0.6.0", BSP_RESET_POWER_ON);
    fw_status(&st); CHECK_EQ(st.state, FW_ST_TRIAL); CHECK_EQ(fs.swaps, swaps + 1);
    /* then the watchdog bites: back to the previous image */
    power_up("0.6.0", BSP_RESET_IWDG);
    CHECK_EQ(fs.swaps, swaps + 2);
    CHECK(fs.reset_requested);
    power_up("0.5.0", BSP_RESET_SOFTWARE);                   /* the previous image again */
    fw_status(&st); CHECK_EQ(st.state, FW_ST_IDLE); CHECK(st.rolled_back);
    CHECK(!strcmp(st.running_version, "0.5.0"));
    /* the failed image is still in the free bank: it can be tried again on purpose */
    CHECK(st.staged); CHECK(!strcmp(st.staged_version, "0.6.0"));

    /* a watchdog reset straight away in the trial: immediate rollback */
    CHECK_EQ(fw_request_activate(), FW_OK);
    fw_service(bsp_host.ms, true);
    power_up("0.6.0", BSP_RESET_SOFTWARE);
    power_up("0.6.0", BSP_RESET_IWDG);
    CHECK_EQ(fs.swaps, swaps + 4);
    power_up("0.5.0", BSP_RESET_SOFTWARE);
    fw_status(&st); CHECK_EQ(st.state, FW_ST_IDLE);

    /* never swap to a bank that went bad after staging */
    fw_flash_sim_free(&fs)[100] ^= 1;
    CHECK_EQ(fw_request_activate(), FW_OK);
    fw_service(bsp_host.ms, true);
    CHECK_EQ(fs.swaps, swaps + 4);
    fw_status(&st); CHECK(!st.staged); CHECK_EQ(st.last_error, FW_E_HASH);

    /* boot record lost (blank FRAM): nothing happens */
    memset(fr_mem, 0, sizeof fr_mem);
    power_up("0.5.0", BSP_RESET_IWDG);
    fw_status(&st); CHECK_EQ(st.state, FW_ST_IDLE); CHECK_EQ(fs.swaps, swaps + 4);
}

/* ---------------- over HTTP ---------------- */
static char out[8192]; static size_t outn;
static int cap(void *ctx, const void *p, size_t n) { (void)ctx; if (outn + n >= sizeof out) return -1; memcpy(out + outn, p, n); outn += n; out[outn] = 0; return 0; }
static uint8_t scratch[4096];
static char sid[AUTH_TOKEN_LEN + 1];
static struct { const uint8_t *p; uint32_t left, chunk; } src;
static int rd(void *ctx, uint8_t *buf, uint32_t capn) {
    (void)ctx;
    uint32_t n = src.left < capn ? src.left : capn;
    if (n > src.chunk) n = src.chunk;
    if (!n) return -1;
    memcpy(buf, src.p, n); src.p += n; src.left -= n;
    return (int)n;
}
static int req(const char *method, const char *path, const uint8_t *body, uint32_t blen, uint32_t prefix) {
    static char raw[1024];
    snprintf(raw, sizeof raw, "%s %s HTTP/1.1\r\nX-CEMS: 1\r\n%s%s%sContent-Length: %u\r\n\r\n", method, path,
             sid[0] ? "Cookie: sid=" : "", sid, sid[0] ? "\r\n" : "", blen);
    http_req_t r;
    if (http_parse_head(raw, (uint32_t)strlen(raw), &r) <= 0) return -1;
    http_out_t o; out_init(&o, cap, NULL); outn = 0;
    if (http_wants_stream(&r)) {
        r.body = body; r.body_len = prefix;
        src.p = body + prefix; src.left = blen - prefix; src.chunk = 1460;
        http_handle_stream(&r, rd, NULL, &o, 1000);
    } else {
        r.body = body; r.body_len = blen;
        http_handle(&r, &o, 1000);
    }
    int st = 0; sscanf(out, "HTTP/1.1 %d", &st);
    const char *ck = strstr(out, "Set-Cookie: sid=");
    if (ck) { ck += 16; memcpy(sid, ck, AUTH_TOKEN_LEN); sid[AUTH_TOKEN_LEN] = 0; }
    return st;
}

static void t_http(void) {
    static uint8_t fram[PLAT_FRAM_SIZE];
    nvm_dev_t d; nvm_ram_t rm;
    nvm_ram_init(&d, &rm, fram, sizeof fram, true);
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    CHECK(param_store_init(&d, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    auth_init();
    http_cfg_t c = { .product = "CEMS", .fw_version = "0.5.0", .scratch = scratch, .scratch_size = sizeof scratch };
    http_api_init(&c);
    power_up("0.5.0", BSP_RESET_POWER_ON);
    sid[0] = 0;
    uint32_t len = make_image(20000, "0.7.0", PLAT_FW_PRODUCT, 21);
    CHECK_EQ(req("POST", "/api/fw/upload", file, len, 100), 401);           /* not logged in */
    CHECK_EQ(req("POST", "/api/login", (const uint8_t *)"level=operator&password=1111", 28, 0), 200);
    CHECK_EQ(req("POST", "/api/fw/upload", file, len, 100), 403);           /* operator may not */
    CHECK_EQ(req("POST", "/api/login", (const uint8_t *)"level=admin&password=2222", 25, 0), 200);
    CHECK_EQ(req("POST", "/api/fw/upload", file, len, 100), 403);           /* admin may not: tech level (D14) */
    CHECK_EQ(req("POST", "/api/login", (const uint8_t *)"level=factory&password=9999", 27, 0), 200);
    CHECK_EQ(req("POST", "/api/fw/upload", file, len, 100), 200);
    CHECK(strstr(out, "\"staged\":\"0.7.0\"") != NULL);
    CHECK_EQ(req("GET", "/api/fw", NULL, 0, 0), 200);
    CHECK(strstr(out, "\"running\":\"0.5.0\",\"state\":\"idle\"") != NULL);
    CHECK(strstr(out, "\"staged\":\"0.7.0\"") != NULL);
    file[FW_HDR_SIZE + 10] ^= 1;
    CHECK_EQ(req("POST", "/api/fw/upload", file, len, 0), 400);
    CHECK(strstr(out, "checksum mismatch") != NULL);
    file[FW_HDR_SIZE + 10] ^= 1;
    src.chunk = 1460;
    CHECK_EQ(req("POST", "/api/fw/activate", NULL, 0, 0), 404);             /* nothing staged now */
    CHECK_EQ(req("POST", "/api/fw/upload", file, len, len), 200);           /* whole body up front */
    CHECK_EQ(req("POST", "/api/fw/activate", NULL, 0, 0), 200);
    CHECK(strstr(out, "\"restarting\":true") != NULL);
    uint32_t swaps = fs.swaps;
    fw_service(1000, true);
    CHECK_EQ(fs.swaps, swaps + 1);
    sid[0] = 0;
}

void test_fw_all(void) {
    bsp_host_reset();
    bsp_host.ms = 5000;
    RUN(t_header_helpers);
    RUN(t_upload);
    RUN(t_activate_confirm);
    RUN(t_rollback);
    RUN(t_http);
    fw_reset_for_test();
    fw_trial_ms = FW_TRIAL_MS;
}
