/* boot_core.c - see boot.h. No HAL, no RTOS: built into the bootloader and the host tests. */
#include <stddef.h>
#include <string.h>
#include "boot.h"
#include "fw_update.h"
#include "nvm.h"
#include "plat_sha256.h"

#define CHUNK 512u

static uint32_t hdr_off(const boot_io_t *io) { return io->bank_size - io->sector; }

boot_img_t boot_check(boot_io_t *io, bool mapped) {
    fw_header_t h;
    if (io->read(io, mapped, hdr_off(io), &h, sizeof h) != 0) return BOOT_IMG_HEADER;
    if (h.magic != FW_MAGIC) return BOOT_IMG_NONE;
    if (h.hdr_size != FW_HDR_SIZE || (h.hdr_version != 1u && h.hdr_version != 2u) ||
        h.hdr_crc != plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc)) ||
        h.product != PLAT_FW_PRODUCT || h.image_size == 0u || h.image_size > hdr_off(io) - io->image_off)
        return BOOT_IMG_HEADER;
    sha256_t s;
    uint8_t buf[CHUNK], dig[32];
    sha256_init(&s);
    for (uint32_t off = 0; off < h.image_size; off += CHUNK) {
        uint32_t n = h.image_size - off < CHUNK ? h.image_size - off : CHUNK;
        if (io->read(io, mapped, io->image_off + off, buf, n) != 0) return BOOT_IMG_HASH;
        sha256_update(&s, buf, n);
    }
    sha256_final(&s, dig);
    if (memcmp(dig, h.sha256, 32) != 0) return BOOT_IMG_HASH;
    if (io->verify) {                                   /* a build with the product key */
        if (!(h.flags & FW_F_SIGNED)) return BOOT_IMG_SIGNATURE;
        sha256_init(&s);
        sha256_update(&s, &h, FW_SIGNED_LEN);
        if (h.hdr_version >= 2u)
            sha256_update(&s, (const uint8_t *)&h + FW_SIGNED2_OFF, (uint32_t)offsetof(fw_header_t, hdr_crc) - FW_SIGNED2_OFF);
        sha256_final(&s, dig);
        if (!io->verify(dig, h.sig)) return BOOT_IMG_SIGNATURE;
    }
    return BOOT_IMG_OK;
}

/* initial stack pointer in RAM, reset handler in this bank's image area, Thumb bit set */
static bool vectors_ok(boot_io_t *io) {
    uint32_t v[2];
    if (io->read(io, true, io->image_off, v, sizeof v) != 0) return false;
    uint32_t reset_off = (v[1] & ~1u) - 0x08000000u;
    return v[0] > io->ram_lo && v[0] <= io->ram_hi && (v[1] & 1u) &&
           reset_off >= io->image_off && reset_off < hdr_off(io);
}

boot_action_t boot_decide(boot_io_t *io, uint32_t *trial, boot_report_t *rep) {
    boot_report_t r = { .mapped = BOOT_IMG_NONE, .other = BOOT_IMG_NONE };
    bool dev = io->verify == NULL;
    bool after_swap = *trial == BOOT_SWAP_MAGIC;        /* this bootloader swapped just before this start */
    if ((*trial & 0xFFFF0000u) == BOOT_TRIAL_MAGIC) {   /* a new image on trial */
        uint32_t n = (*trial & 0xFFFFu) + 1u;
        if (n > BOOT_TRIAL_MAX) {                       /* it never confirmed: back to the previous one */
            *trial = 0;
            r.rollback = true;
            r.other = boot_check(io, false);
            if (r.other == BOOT_IMG_OK) { *trial = BOOT_SWAP_MAGIC; if (rep) *rep = r; return BOOT_SWAP; }
        } else {
            *trial = BOOT_TRIAL_MAGIC | n;
        }
    } else {
        *trial = 0;                                     /* power-up garbage or cleared */
    }
    r.vectors_ok = vectors_ok(io);
    r.mapped = boot_check(io, true);
    boot_action_t a;
    if (r.mapped == BOOT_IMG_OK && r.vectors_ok) a = BOOT_JUMP;
    else if (dev && r.mapped == BOOT_IMG_NONE && r.vectors_ok) a = BOOT_JUMP;   /* programmed by the debugger */
    else {
        if (!r.rollback) r.other = boot_check(io, false);
        if (r.other == BOOT_IMG_OK && !after_swap) { *trial = BOOT_SWAP_MAGIC; a = BOOT_SWAP; }   /* never back and forth */
        else if (dev && r.vectors_ok) a = BOOT_JUMP;    /* development: try it anyway */
        else a = BOOT_HALT;
    }
    if (rep) *rep = r;
    return a;
}
