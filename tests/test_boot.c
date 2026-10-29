/* test_boot.c - the first-stage bootloader's decision (boot_core.c) on two simulated banks.
 * Requirements proved here: CIP-04.1 (the update never touches it, with test_fw t_boot_offset),
 * CIP-04.2 (a damaged running image swaps to the good one), CIP-20.7 (signature check). */
#include <stddef.h>
#include <string.h>
#include "tst.h"
#include "boot.h"
#include "fw_update.h"
#include "nvm.h"
#include "plat_sha256.h"

#define BANK   0x10000u            /* small banks keep the test fast; the logic only sees offsets */
#define SECT   0x800u
#define IOFF   0x2000u             /* "PLAT_BOOT_SIZE" */
static uint8_t bank[2][BANK];      /* physical banks */
static int     swapped;            /* SWAP_BANK: which physical bank is mapped at 0x08000000 */

static int rd(boot_io_t *io, bool mapped, uint32_t off, void *p, uint32_t n) {
    (void)io;
    if (off > BANK || n > BANK - off) return -1;
    memcpy(p, bank[mapped ? swapped : !swapped] + off, n);
    return 0;
}
static bool key_ok(const uint8_t hash[32], const uint8_t sig[64]) {   /* "signature" = the hash, for the test */
    return memcmp(hash, sig, 32) == 0;
}
static boot_io_t io = { BANK, SECT, IOFF, 0x20000000u, 0x200A0000u, rd, NULL, NULL };

/* an image in physical bank b: vectors, payload, header in the last sector */
static void put_image(int b, uint32_t size, uint8_t seed, bool sign, uint32_t reset) {
    memset(bank[b], 0xFF, BANK);
    uint8_t *img = bank[b] + IOFF;
    for (uint32_t k = 0; k < size; ++k) img[k] = (uint8_t)(seed + k * 7u);
    uint32_t v[2] = { 0x20090000u, reset ? reset : 0x08000000u + IOFF + 0x201u };
    memcpy(img, v, sizeof v);
    fw_header_t h;
    memset(&h, 0, sizeof h);
    h.magic = FW_MAGIC; h.hdr_size = FW_HDR_SIZE; h.hdr_version = 2; h.product = PLAT_FW_PRODUCT;
    h.version = 0x000901u; strcpy(h.version_str, "0.9.1"); h.image_size = size;
    sha256_t s; sha256_init(&s); sha256_update(&s, img, size); sha256_final(&s, h.sha256);
    if (sign) {
        h.flags = FW_F_SIGNED;
        uint8_t d[32];
        sha256_init(&s); sha256_update(&s, &h, FW_SIGNED_LEN);
        sha256_update(&s, (const uint8_t *)&h + FW_SIGNED2_OFF, (uint32_t)offsetof(fw_header_t, hdr_crc) - FW_SIGNED2_OFF);
        sha256_final(&s, d);
        memcpy(h.sig, d, 32);
    }
    h.hdr_crc = plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc));
    memcpy(bank[b] + BANK - SECT, &h, sizeof h);
}

/* one reset: decide, and carry out a swap as the target would */
static boot_action_t boot(uint32_t *trial, boot_report_t *r) {
    boot_action_t a = boot_decide(&io, trial, r);
    if (a == BOOT_SWAP) swapped = !swapped;
    return a;
}

static void t_good_image_starts(void) {
    swapped = 0; io.verify = NULL;
    put_image(0, 30000, 1, false, 0); put_image(1, 20000, 2, false, 0);
    uint32_t trial = 0x12345678u;                        /* power-up garbage */
    boot_report_t r;
    CHECK_EQ(boot(&trial, &r), BOOT_JUMP);
    CHECK_EQ(r.mapped, BOOT_IMG_OK); CHECK(r.vectors_ok); CHECK_EQ(trial, 0);
}

static void t_damaged_image_swaps_to_good(void) {
    swapped = 0; io.verify = NULL;
    put_image(0, 30000, 1, false, 0); put_image(1, 20000, 2, false, 0);
    bank[0][IOFF + 5000] ^= 0x40;                        /* a flipped bit in the running image */
    uint32_t trial = 0; boot_report_t r;
    CHECK_EQ(boot(&trial, &r), BOOT_SWAP);
    CHECK_EQ(r.mapped, BOOT_IMG_HASH); CHECK_EQ(r.other, BOOT_IMG_OK); CHECK_EQ(swapped, 1);
    CHECK_EQ(boot(&trial, &r), BOOT_JUMP);               /* the other bank now starts */
    CHECK_EQ(trial, 0);
}

static void t_never_ping_pong(void) {
    swapped = 0; io.verify = key_ok;                     /* production: no "try it anyway" */
    put_image(0, 30000, 1, true, 0); put_image(1, 20000, 2, true, 0);
    bank[0][IOFF + 100] ^= 1;                            /* the mapped image is bad, the other good ... */
    uint32_t trial = BOOT_SWAP_MAGIC; boot_report_t r;   /* ... but the bootloader swapped just before this start */
    CHECK_EQ(boot(&trial, &r), BOOT_HALT);               /* stop rather than swap back and forth */
    CHECK_EQ(r.mapped, BOOT_IMG_HASH); CHECK_EQ(r.other, BOOT_IMG_OK); CHECK_EQ(trial, 0);
    CHECK_EQ(boot(&trial, &r), BOOT_SWAP);               /* a later reset (no swap just before) tries again */
    io.verify = NULL;
}

static void t_debugger_image_in_development(void) {
    swapped = 0; io.verify = NULL;
    put_image(0, 30000, 1, false, 0);
    memset(bank[0] + BANK - SECT, 0xFF, SECT);           /* no header: programmed by the debugger */
    memset(bank[1], 0xFF, BANK);
    uint32_t trial = 0; boot_report_t r;
    CHECK_EQ(boot(&trial, &r), BOOT_JUMP);
    CHECK_EQ(r.mapped, BOOT_IMG_NONE);
    io.verify = key_ok;                                  /* a production bootloader refuses it */
    CHECK_EQ(boot(&trial, &r), BOOT_HALT);
    memset(bank[0] + IOFF, 0xFF, 8);                     /* erased: no vectors, nothing to start at all */
    io.verify = NULL;
    CHECK_EQ(boot(&trial, &r), BOOT_HALT);
    CHECK(!r.vectors_ok);
}

static void t_signatures(void) {
    swapped = 0; io.verify = key_ok;
    put_image(0, 30000, 1, true, 0); put_image(1, 20000, 2, false, 0);
    uint32_t trial = 0; boot_report_t r;
    CHECK_EQ(boot(&trial, &r), BOOT_JUMP);               /* signed: starts */
    bank[0][BANK - SECT + offsetof(fw_header_t, sig) + 3] ^= 1;   /* wrong signature (header CRC redone) */
    fw_header_t h; memcpy(&h, bank[0] + BANK - SECT, sizeof h);
    h.hdr_crc = plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc)); memcpy(bank[0] + BANK - SECT, &h, sizeof h);
    CHECK_EQ(boot(&trial, &r), BOOT_HALT);               /* the other one is unsigned: no swap to it */
    CHECK_EQ(r.mapped, BOOT_IMG_SIGNATURE); CHECK_EQ(r.other, BOOT_IMG_SIGNATURE);
    put_image(1, 20000, 2, true, 0);
    trial = 0;
    CHECK_EQ(boot(&trial, &r), BOOT_SWAP);               /* ... a signed one: swap */
    io.verify = NULL;
}

static void t_header_checks(void) {
    swapped = 0; io.verify = NULL;
    put_image(1, 20000, 2, false, 0);
    fw_header_t h;
    struct { size_t off; uint32_t v; } cases[] = {
        { offsetof(fw_header_t, product), 0x11111111u },
        { offsetof(fw_header_t, image_size), BANK },     /* larger than the image area */
        { offsetof(fw_header_t, image_size), 0 },
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        put_image(0, 30000, 1, false, 0);
        memcpy(&h, bank[0] + BANK - SECT, sizeof h);
        memcpy((uint8_t *)&h + cases[c].off, &cases[c].v, 4);
        h.hdr_crc = plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc));
        memcpy(bank[0] + BANK - SECT, &h, sizeof h);
        CHECK_EQ(boot_check(&io, true), BOOT_IMG_HEADER);
    }
    put_image(0, 30000, 1, false, 0);
    bank[0][BANK - SECT + 20] ^= 1;                      /* CRC no longer matches */
    CHECK_EQ(boot_check(&io, true), BOOT_IMG_HEADER);
    put_image(0, 30000, 1, false, 0x08000000u + BANK - SECT + 1u);   /* reset handler outside the image */
    uint32_t trial = 0; boot_report_t r;
    CHECK_EQ(boot(&trial, &r), BOOT_SWAP);
    CHECK(!r.vectors_ok);
}

static void t_trial_counter(void) {
    swapped = 0; io.verify = NULL;
    put_image(0, 30000, 1, false, 0); put_image(1, 20000, 2, false, 0);
    swapped = 1;                                         /* the application swapped to the new image */
    uint32_t trial = BOOT_TRIAL_MAGIC; boot_report_t r;
    for (unsigned k = 1; k <= BOOT_TRIAL_MAX; ++k) {     /* it hangs before confirming: watchdog resets */
        CHECK_EQ(boot(&trial, &r), BOOT_JUMP);
        CHECK_EQ(trial, BOOT_TRIAL_MAGIC | k);
    }
    CHECK_EQ(boot(&trial, &r), BOOT_SWAP);               /* one start too many: back to the previous image */
    CHECK(r.rollback); CHECK_EQ(swapped, 0); CHECK_EQ(trial, BOOT_SWAP_MAGIC);
    CHECK_EQ(boot(&trial, &r), BOOT_JUMP);
    CHECK_EQ(trial, 0);
    /* confirmed in time: the application clears the register, nothing happens */
    swapped = 1; trial = BOOT_TRIAL_MAGIC;
    CHECK_EQ(boot(&trial, &r), BOOT_JUMP);
    trial = 0;                                           /* (fw_update confirms) */
    for (unsigned k = 0; k < 5u; ++k) CHECK_EQ(boot(&trial, &r), BOOT_JUMP);
    CHECK_EQ(swapped, 1);
    /* the counter runs out but the previous image is gone: keep the new one */
    trial = BOOT_TRIAL_MAGIC | BOOT_TRIAL_MAX;
    memset(bank[0] + BANK - SECT, 0xFF, SECT);
    CHECK_EQ(boot(&trial, &r), BOOT_JUMP);
    CHECK(r.rollback); CHECK_EQ(swapped, 1); CHECK_EQ(trial, 0);
}

void test_boot_all(void) {
    RUN(t_good_image_starts);
    RUN(t_damaged_image_swaps_to_good);
    RUN(t_never_ping_pong);
    RUN(t_debugger_image_in_development);
    RUN(t_signatures);
    RUN(t_header_checks);
    RUN(t_trial_counter);
}
