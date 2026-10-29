/* test_fw.c - firmware upload, staging, activation, trial, confirm and rollback.
 * Requirements proved here: CIP-03.2, 03.3, 03.4, 03.5, 03.6, 03.7, 17.3, 20.2, 25.1, 19.2. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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
#include "tls.h"
#include "ul_system.h"
#include "plat_clock.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/sha256.h"

#define BANK (128u * 1024u)
static uint8_t b0[BANK], b1[BANK];
static fw_flash_t fl;
static fw_flash_sim_t fs;
static uint8_t fr_mem[1024];
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
static void make_v2(uint8_t hw_min, uint8_t hw_max, const char *build) {   /* a header-version-2 image */
    fw_header_t h; memcpy(&h, file, sizeof h);
    h.hdr_version = 2; h.hw_min = hw_min; h.hw_max = hw_max; h.layout_version = 3;
    memset(h.build_id, 0, sizeof h.build_id);
    strncpy(h.build_id, build, sizeof h.build_id);
    h.hdr_crc = plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc));
    memcpy(file, &h, sizeof h);
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

static void set_clock(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi) {
    RTC_Date.Year = (uint16_t)y; RTC_Date.Month = (uint8_t)mo; RTC_Date.Day_In_Month = (uint8_t)d;
    RTC_Time.Hours = (uint8_t)h; RTC_Time.Minutes = (uint8_t)mi; RTC_Time.Seconds = 0;
}
static void clock_off(void) { memset(&RTC_Date, 0, sizeof RTC_Date); memset(&RTC_Time, 0, sizeof RTC_Time); }

/* every firmware change is recorded with its date and time, in FRAM */
static void t_history(void) {
    fw_hist_t h[FW_HIST_MAX + 2];
    memset(fr_mem, 0, sizeof fr_mem);
    power_up("0.4.0", BSP_RESET_POWER_ON);
    CHECK_EQ(fw_history(h, FW_HIST_MAX), 0u);
    /* an update asked from the web by the factory login, at 09:30, kept at 09:32 */
    uint32_t len = make_image(20000, "0.5.0", PLAT_FW_PRODUCT, 3);
    CHECK_EQ(upload(len, 2048), FW_OK);
    set_clock(2026, 10, 20, 9, 30);
    CHECK_EQ(fw_request_activate_from(PLAT_SRC_WEB, PLAT_LVL_FACTORY), FW_OK);
    fw_service(bsp_host.ms, true);
    fw_trial_ms = 60000;
    power_up("0.5.0", BSP_RESET_SOFTWARE);
    set_clock(2026, 10, 20, 9, 32);
    uint32_t t = bsp_host.ms;
    fw_service(t, true); fw_service(t + 61000, true);
    CHECK_EQ(fw_history(h, FW_HIST_MAX), 1u);
    CHECK_EQ(h[0].kind, FWH_UPDATED);
    CHECK(!strcmp(h[0].from, "0.4.0")); CHECK(!strcmp(h[0].to, "0.5.0"));
    CHECK_EQ(h[0].src, PLAT_SRC_WEB); CHECK_EQ(h[0].level, PLAT_LVL_FACTORY);
    CHECK_EQ(h[0].flags, FWH_INST_CLOCK | FWH_DONE_CLOCK);
    CHECK_EQ(h[0].t_installed, plat_unix_from_civil(2026, 10, 20, 9, 30, 0));
    CHECK_EQ(h[0].t_done, plat_unix_from_civil(2026, 10, 20, 9, 32, 0));
    /* kept over restarts and power cuts */
    power_up("0.5.0", BSP_RESET_POWER_ON);
    CHECK_EQ(fw_history(h, FW_HIST_MAX), 1u); CHECK(!strcmp(h[0].to, "0.5.0"));
    /* a failed update is recorded as rolled back, newest first; this time the clock is not set */
    clock_off();
    len = make_image(20000, "0.6.0", PLAT_FW_PRODUCT, 4);
    CHECK_EQ(upload(len, 2048), FW_OK);
    CHECK_EQ(fw_request_activate(), FW_OK);
    fw_service(bsp_host.ms, true);
    power_up("0.6.0", BSP_RESET_SOFTWARE);
    power_up("0.6.0", BSP_RESET_IWDG);
    power_up("0.5.0", BSP_RESET_SOFTWARE);
    CHECK_EQ(fw_history(h, FW_HIST_MAX), 1u);                /* not yet: waits for the clock */
    set_clock(2026, 10, 21, 8, 0);
    fw_service(bsp_host.ms + 10, true);
    CHECK_EQ(fw_history(h, FW_HIST_MAX), 2u);
    CHECK_EQ(h[0].kind, FWH_ROLLED_BACK); CHECK(!strcmp(h[0].to, "0.6.0")); CHECK(!strcmp(h[0].from, "0.5.0"));
    CHECK_EQ(h[0].flags, FWH_DONE_CLOCK);                    /* installed while the clock was not set */
    CHECK_EQ(h[0].t_installed, 0u);
    CHECK_EQ(h[0].src, PLAT_SRC_LOCAL);
    CHECK_EQ(h[1].kind, FWH_UPDATED); CHECK(!strcmp(h[1].to, "0.5.0"));
    /* no clock for 3 s after a rollback: recorded anyway, without a time */
    clock_off();
    CHECK_EQ(fw_request_activate(), FW_OK);
    fw_service(bsp_host.ms, true);
    power_up("0.6.0", BSP_RESET_SOFTWARE);
    power_up("0.6.0", BSP_RESET_IWDG);
    power_up("0.5.0", BSP_RESET_SOFTWARE);
    fw_service(bsp_host.ms + 1000, true);
    CHECK_EQ(fw_history(h, FW_HIST_MAX), 2u);
    fw_service(bsp_host.ms + 3000, true);
    CHECK_EQ(fw_history(h, FW_HIST_MAX), 3u); CHECK_EQ(h[0].flags, 0); CHECK_EQ(h[0].t_done, 0u);
    /* only the last FW_HIST_MAX are kept */
    for (int k = 0; k < 6; ++k) {
        CHECK_EQ(fw_request_activate(), FW_OK);
        fw_service(bsp_host.ms, true);
        power_up("0.6.0", BSP_RESET_SOFTWARE);
        power_up("0.6.0", BSP_RESET_IWDG);
        power_up("0.5.0", BSP_RESET_SOFTWARE);
        fw_service(bsp_host.ms + 3000, true);
    }
    CHECK_EQ(fw_history(h, FW_HIST_MAX + 2), FW_HIST_MAX);
    for (size_t k = 0; k < FW_HIST_MAX; ++k) CHECK_EQ(h[k].kind, FWH_ROLLED_BACK);
    CHECK_EQ(fw_history(h, 2), 2u);
    /* a damaged copy: the other one is used */
    fr_mem[128 + 256 + 20] ^= 0xFF;
    power_up("0.5.0", BSP_RESET_POWER_ON);
    CHECK(fw_history(h, FW_HIST_MAX) >= FW_HIST_MAX - 1u);
    fw_trial_ms = FW_TRIAL_MS;
    clock_off();
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

/* ---------------- signatures (D14) ---------------- */
static mbedtls_ecdsa_context sk[2];
static uint8_t pub[2][65];
static mbedtls_ctr_drbg_context rng;

static void sign_file(int which) {            /* as tools/mkfwimage.py --key does */
    fw_header_t h; memcpy(&h, file, sizeof h);
    h.flags = FW_F_SIGNED;
    memset(h.sig, 0, sizeof h.sig);
    uint8_t dg[32], msg[FW_HDR_SIZE];
    size_t n = FW_SIGNED_LEN;
    memcpy(msg, &h, FW_SIGNED_LEN);
    if (h.hdr_version >= 2u) {                /* v2: the fields after the signature too */
        size_t more = offsetof(fw_header_t, hdr_crc) - FW_SIGNED2_OFF;
        memcpy(msg + n, (const uint8_t *)&h + FW_SIGNED2_OFF, more);
        n += more;
    }
    mbedtls_sha256(msg, n, dg, 0);
    mbedtls_mpi r, s2; mbedtls_mpi_init(&r); mbedtls_mpi_init(&s2);
    CHECK_EQ(mbedtls_ecdsa_sign(&sk[which].MBEDTLS_PRIVATE(grp), &r, &s2, &sk[which].MBEDTLS_PRIVATE(d), dg, 32,
                                mbedtls_ctr_drbg_random, &rng), 0);
    mbedtls_mpi_write_binary(&r, h.sig, 32);
    mbedtls_mpi_write_binary(&s2, h.sig + 32, 32);
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s2);
    memcpy(file, &h, sizeof h);
    reheader();
}

static void t_signed(void) {
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
        CHECK_EQ(n, 65);
    }
    fw_status_t st;
    CHECK(!fw_signing_required());                       /* this development build has no key */
    fw_pubkey_override_for_test(pub[0]);                 /* a production build */
    CHECK(fw_signing_required());
    power_up("0.5.0", BSP_RESET_POWER_ON);
    fw_status(&st); CHECK(st.signing_required);
    uint32_t len = make_image(20000, "0.8.0", PLAT_FW_PRODUCT, 31);
    CHECK_EQ(upload(len, 2048), FW_E_SIGNATURE);        /* unsigned: refused */
    sign_file(1);
    CHECK_EQ(upload(len, 2048), FW_E_SIGNATURE);        /* signed with another key: refused */
    sign_file(0);
    CHECK_EQ(upload(len, 2048), FW_OK);                  /* the product key: accepted */
    fw_status(&st); CHECK(st.staged); CHECK(!strcmp(st.staged_version, "0.8.0"));
    /* someone edits a signed header (another version number) and fixes the CRC: refused */
    { fw_header_t h; memcpy(&h, file, sizeof h); strcpy(h.version_str, "9.9.9"); h.version = 0x090909; memcpy(file, &h, sizeof h); reheader(); }
    CHECK_EQ(upload(len, 2048), FW_E_SIGNATURE);
    /* or the payload (its SHA-256 no longer matches the signed one) */
    make_image(20000, "0.8.0", PLAT_FW_PRODUCT, 31); sign_file(0);
    file[FW_HDR_SIZE + 5] ^= 1;
    CHECK_EQ(upload(len, 2048), FW_E_HASH);
    /* header version 2 signs the board range and build ID as well */
    make_image(20000, "0.8.1", PLAT_FW_PRODUCT, 32); make_v2(0, 3, "abc1234"); sign_file(0);
    CHECK_EQ(upload(len, 2048), FW_OK);
    { fw_header_t h; memcpy(&h, file, sizeof h); h.hw_max = 9; memcpy(file, &h, sizeof h); reheader(); }
    CHECK_EQ(upload(len, 2048), FW_E_SIGNATURE);         /* the range widened after signing */
    fw_pubkey_override_for_test(NULL);
    for (int k = 0; k < 2; ++k) mbedtls_ecdsa_free(&sk[k]);
    mbedtls_ctr_drbg_free(&rng); mbedtls_entropy_free(&ent);
}

/* ---------------- board revision range, build ID (CIP-01.3, 03.2, 25.1) ---------------- */
static void t_hw_range(void) {
    fw_status_t st;
    bsp_host.straps_fitted = true; bsp_host.hw_straps = 1;      /* a Rev B board */
    power_up("0.5.0", BSP_RESET_POWER_ON);
    CHECK_EQ(fw_board_rev(), 1);
    CHECK(fw_build_id()[0] != 0);
    uint32_t len = make_image(30000, "0.9.0", PLAT_FW_PRODUCT, 41);
    make_v2(2, 3, "deadbeef01");
    CHECK_EQ(upload(len, 4096), FW_E_HARDWARE);           /* Rev C/D only */
    CHECK(!strcmp(fw_err_name(FW_E_HARDWARE), "built for another board revision"));
    make_v2(0, 1, "deadbeef01");
    CHECK_EQ(upload(len, 4096), FW_OK);
    fw_status(&st);
    CHECK(!strcmp(st.staged_build, "deadbeef01"));
    CHECK_EQ(st.staged_hw_min, 0); CHECK_EQ(st.staged_hw_max, 1);
    /* the straps read differently at the next start (another board): refused at activation */
    bsp_host.hw_straps = 3;
    power_up("0.5.0", BSP_RESET_POWER_ON);
    CHECK_EQ(fw_request_activate(), FW_E_HARDWARE);
    /* a v1 image states no range; a board without straps checks none */
    make_image(30000, "0.9.1", PLAT_FW_PRODUCT, 42);
    CHECK_EQ(upload(len, 4096), FW_OK);
    fw_status(&st); CHECK_EQ(st.staged_hw_max, 255); CHECK(!st.staged_build[0]);
    bsp_host.straps_fitted = false;
    power_up("0.5.0", BSP_RESET_POWER_ON);
    CHECK_EQ(fw_board_rev(), -1);
    make_image(30000, "0.9.2", PLAT_FW_PRODUCT, 43); make_v2(3, 3, "x");
    CHECK_EQ(upload(len, 4096), FW_OK);
}

/* ---------------- gates: minimum version, safe state, local confirmation ---------------- */
static bool safe_now = true;
static bool safe_cb(char *why, size_t cap) { if (!safe_now) snprintf(why, cap, "a calibration is running"); return safe_now; }
static void setp(uint16_t id, uint32_t v) {
    plat_cmd_t c = { .id = id, .src = PLAT_SRC_INTERNAL, .kind = PLAT_VAL_U32, .v.u = v };
    CHECK_EQ(param_apply(&c), PERR_OK);
    param_publish();
}

static void t_gates(void) {
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    param_bind(NULL, NULL);
    param_set_defaults(PARAM_ALL);
    param_publish();
    fw_status_t st;
    memset(fr_mem, 0, sizeof fr_mem);                             /* a fresh boot record */
    power_up("0.9.0", BSP_RESET_POWER_ON);
    /* anti-rollback: nothing older than 0.8.0 */
    setp(PARAM_ID_FW_MIN_VERSION, 0x000800u);
    uint32_t len = make_image(20000, "0.7.9", PLAT_FW_PRODUCT, 41);
    CHECK_EQ(upload(len, 4096), FW_E_OLD);
    len = make_image(20000, "0.8.0", PLAT_FW_PRODUCT, 42);
    CHECK_EQ(upload(len, 4096), FW_OK);
    setp(PARAM_ID_FW_MIN_VERSION, 0x000801u);                    /* raised after the upload */
    CHECK_EQ(fw_request_activate(), FW_E_OLD);
    setp(PARAM_ID_FW_MIN_VERSION, 0);
    /* the application's safe state */
    fw_safe_check = safe_cb;
    safe_now = false;
    uint32_t swaps = fs.swaps;
    CHECK_EQ(fw_request_activate_from(PLAT_SRC_WEB, PLAT_LVL_FACTORY), FW_E_NOT_SAFE);
    fw_status(&st); CHECK(!strcmp(st.not_safe, "a calibration is running"));
    safe_now = true;
    CHECK_EQ(fw_request_activate_from(PLAT_SRC_WEB, PLAT_LVL_FACTORY), FW_OK);
    safe_now = false;                                            /* a calibration started meanwhile */
    fw_service(bsp_host.ms, true);
    CHECK_EQ(fs.swaps, swaps);                                   /* checked again before the swap */
    fw_status(&st); CHECK_EQ(st.last_error, FW_E_NOT_SAFE);
    safe_now = true;
    /* local confirmation: a web request waits for the touchscreen */
    setp(PARAM_ID_FW_LOCAL_CONFIRM, 1);
    CHECK_EQ(fw_request_activate_from(PLAT_SRC_WEB, PLAT_LVL_FACTORY), FW_E_WAIT_LOCAL);
    fw_service(bsp_host.ms, true);
    fw_status(&st); CHECK(st.wait_local); CHECK_EQ(st.wait_src, PLAT_SRC_WEB); CHECK(st.wait_left_ms > 290000u);
    CHECK_EQ(fs.swaps, swaps);
    fw_local_answer(false, 0);                                   /* "No" on the touchscreen */
    fw_service(bsp_host.ms, true);
    fw_status(&st); CHECK(!st.wait_local); CHECK_EQ(fs.swaps, swaps);
    CHECK_EQ(fw_request_activate_from(PLAT_SRC_WEB, PLAT_LVL_FACTORY), FW_E_WAIT_LOCAL);
    fw_service(bsp_host.ms, true);
    fw_service(bsp_host.ms + 5u * 60u * 1000u, true);            /* nobody answered in 5 minutes */
    fw_status(&st); CHECK(!st.wait_local);
    fw_local_answer(true, 0);                                    /* too late: nothing happens */
    fw_service(bsp_host.ms, true); CHECK_EQ(fs.swaps, swaps);
    CHECK_EQ(fw_request_activate_from(PLAT_SRC_LOCAL, 0), FW_OK);   /* on the touchscreen itself: no extra question */
    fw_service(bsp_host.ms, true); CHECK_EQ(fs.swaps, swaps + 1);
    setp(PARAM_ID_FW_LOCAL_CONFIRM, 0);
    fw_safe_check = NULL;
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

/* ---------------- behind the bootloader (boot.h) ---------------- */
static void t_boot_offset(void) {
    const uint32_t ioff = 2u * 8192u;                        /* PLAT_BOOT_SIZE: two sectors here */
    fw_flash_sim_init(&fl, &fs, b0, b1, BANK);
    fl.image_off = ioff;
    memset(b0, 0xB5, BANK); memset(b1, 0xB5, BANK);          /* "bootloader" in both banks */
    nvm_ram_init(&fr, &fr_ram, fr_mem, sizeof fr_mem, true);
    memset(fr_mem, 0, sizeof fr_mem);
    fw_status_t st;
    power_up("0.8.0", BSP_RESET_POWER_ON);
    CHECK(!fs.trial);
    fs.trial = true;                                         /* a stale mark ... */
    power_up("0.8.0", BSP_RESET_POWER_ON);
    CHECK(!fs.trial);                                        /* ... is cleared at an ordinary start */

    uint32_t len = make_image(BANK - 8192u - ioff + 1u, "0.9.0", PLAT_FW_PRODUCT, 21);   /* one byte too large */
    CHECK_EQ(upload(len, 3000), FW_E_SIZE);
    len = make_image(BANK - 8192u - ioff, "0.9.0", PLAT_FW_PRODUCT, 21);                 /* fills the image area */
    CHECK_EQ(upload(len, 3000), FW_OK);
    fw_status(&st); CHECK(st.staged);
    uint8_t *fb = fw_flash_sim_free(&fs);
    CHECK(memcmp(fb + ioff, file + FW_HDR_SIZE, len - FW_HDR_SIZE) == 0);
    unsigned keep = 0;
    for (uint32_t k = 0; k < ioff; ++k) keep += fb[k] == 0xB5;
    CHECK_EQ(keep, ioff);                                    /* the bootloader was never erased */
    CHECK_EQ(fs.violations, 0);

    CHECK_EQ(fw_request_activate(), FW_OK);
    fw_service(bsp_host.ms, true);
    CHECK(fs.trial); CHECK(fs.swapped);                      /* marked before the swap */
    fw_trial_ms = 1000;
    power_up("0.9.0", BSP_RESET_SOFTWARE);
    CHECK(fs.trial);                                         /* still counting during the trial */
    CHECK_EQ(fw_running_check(BANK), 1);                     /* the running image checks out at its offset */
    uint32_t t = bsp_host.ms;
    fw_service(t, true);
    fw_service(t + 1500, true);
    fw_status(&st); CHECK_EQ(st.state, FW_ST_IDLE);
    CHECK(!fs.trial);                                        /* confirmed */

    /* a rollback clears the mark before swapping back */
    len = make_image(20000, "0.9.1", PLAT_FW_PRODUCT, 22);
    CHECK_EQ(upload(len, 3000), FW_OK);
    CHECK_EQ(fw_request_activate(), FW_OK);
    fw_service(bsp_host.ms, true);
    CHECK(fs.trial);
    power_up("0.9.1", BSP_RESET_SOFTWARE);
    power_up("0.9.1", BSP_RESET_IWDG);
    CHECK(fs.reset_requested); CHECK(!fs.trial);
    power_up("0.9.0", BSP_RESET_SOFTWARE);
    fw_status(&st); CHECK(st.rolled_back); CHECK(!fs.trial);

    /* the bootloader rolls back by itself (the new image never reached fw_boot): the
     * previous image must not swap "back" to the one that failed */
    CHECK_EQ(fw_request_activate(), FW_OK);                  /* 0.9.1 again, still staged */
    fw_service(bsp_host.ms, true);
    CHECK(fs.trial);
    uint32_t sw = fs.swaps;
    fs.swapped = !fs.swapped; fs.trial = false;              /* what the bootloader does after 3 starts */
    power_up("0.9.0", BSP_RESET_IWDG);
    CHECK_EQ(fs.swaps, sw); CHECK(!fs.reset_requested);      /* no swap */
    fw_status(&st); CHECK_EQ(st.state, FW_ST_IDLE); CHECK(st.rolled_back);
    CHECK(!strcmp(st.running_version, "0.9.0"));
    CHECK(st.staged); CHECK(!strcmp(st.staged_version, "0.9.1"));
    power_up("0.9.0", BSP_RESET_POWER_ON);                   /* and it stays that way */
    fw_status(&st); CHECK_EQ(st.state, FW_ST_IDLE); CHECK_EQ(fs.swaps, sw);
    fl.image_off = 0;
}

void test_fw_all(void) {
    bsp_host_reset();
    bsp_host.ms = 5000;
    RUN(t_header_helpers);
    RUN(t_upload);
    RUN(t_activate_confirm);
    RUN(t_rollback);
    RUN(t_history);
    RUN(t_signed);
    RUN(t_hw_range);
    RUN(t_http);
    RUN(t_gates);
    RUN(t_boot_offset);
    fw_reset_for_test();
    fw_trial_ms = FW_TRIAL_MS;
}
