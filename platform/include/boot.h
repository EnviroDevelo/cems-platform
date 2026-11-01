/* boot.h - the first-stage bootloader (CIP-04.1, CIP-20.7; Gary, 29 Oct: our own, not
 * ST OEMiRoT). docs/BOOTLOADER.md has the whole design; in short:
 *
 * Flash, each 1 MB bank (8 KB sectors):
 *   0x00000 .. PLAT_BOOT_SIZE-1   the bootloader: the same copy in both banks, write
 *                                 protected (WRP), so whichever bank SWAP_BANK maps at
 *                                 0x08000000 starts with it. An update never writes here.
 *   PLAT_BOOT_SIZE ..             the application image, linked for 0x08000000 + PLAT_BOOT_SIZE
 *   last sector                   the image's header (fw_header_t), written by an update
 *                                 (fw_update.h) or at the factory (tools/mkfwimage.py --factory)
 *
 * At every reset the bootloader checks the image in the bank mapped at 0x08000000 (header
 * CRC, product, size, SHA-256 of the image, and the ECDSA P-256 signature when the build
 * has the product's public key) and starts it. If it is not good it swaps to the other
 * bank when that one holds a good image, else it stops (recovery: SWD or the ST ROM
 * bootloader through BOOT0).
 *
 * Trial counter (the case fw_update.h cannot cover: an image too broken to reach
 * fw_boot()): before swapping to a new image the application writes BOOT_TRIAL_MAGIC
 * into a backup register (TAMP, kept over resets); the bootloader counts every start
 * while it is set, and after BOOT_TRIAL_MAX starts swaps back. The application clears
 * it once the image has confirmed itself, and before its own rollback swap. A power cut
 * clears the register too; the FRAM boot record (fw_update.c) then decides, as before.
 *
 * boot_decide() is the whole decision, on an abstract flash, so it is host-tested
 * (tests/test_boot.c); boot/boot_main.c is the target shell around it. */
#ifndef BOOT_H
#define BOOT_H
#include <stdint.h>
#include <stdbool.h>

#ifndef PLAT_BOOT_SIZE
#define PLAT_BOOT_SIZE 0x8000u        /* 32 KB (4 sectors); 0 = no bootloader (image at the start of flash) */
#endif
#define BOOT_TRIAL_MAGIC 0xB0070000u  /* | starts so far, in the low 16 bits */
#define BOOT_TRIAL_MAX   3u           /* starts of a new image without confirmation before rolling back */
#define BOOT_SWAP_MAGIC  0xB00A5A5Au  /* the bootloader itself swapped banks: never swap straight back */
/* where the counter lives on the STM32H5: the last TAMP backup register (kept over resets,
 * not used by anything else; cleared by a power cut unless VBAT is backed) */
#define BOOT_TRIAL_REG   (TAMP->BKP31R)
/* what the bootloader found at this start, for the application to show (/api/fw "bootloader"):
 * the next backup register down, boot_report_word() */
#define BOOT_REPORT_REG  (TAMP->BKP30R)
#define BOOT_REPORT_MAGIC 0xB5000000u
#define BOOT_VERSION     1u            /* bumped when the bootloader's behaviour changes */

typedef struct boot_io {
    uint32_t bank_size;               /* bytes per bank */
    uint32_t sector;                  /* the header lives in the last one */
    uint32_t image_off;               /* PLAT_BOOT_SIZE */
    uint32_t ram_lo, ram_hi;          /* plausible initial stack pointers (vector table check) */
    /* read from the bank mapped at 0x08000000 (mapped = true) or the other one */
    int  (*read)(struct boot_io *io, bool mapped, uint32_t off, void *p, uint32_t n);
    /* ECDSA P-256 check of hash against sig with the product key; NULL = no key in this
     * build (development): unsigned images are accepted, as by fw_update */
    bool (*verify)(const uint8_t hash[32], const uint8_t sig[64]);
    void *ctx;
} boot_io_t;

typedef enum { BOOT_JUMP, BOOT_SWAP, BOOT_HALT } boot_action_t;

typedef enum {
    BOOT_IMG_OK = 0,
    BOOT_IMG_NONE,                    /* no header: programmed by the debugger */
    BOOT_IMG_HEADER,                  /* damaged header, other product, or too large */
    BOOT_IMG_HASH,                    /* image does not match its SHA-256 */
    BOOT_IMG_SIGNATURE                /* unsigned or wrongly signed, and the build has a key */
} boot_img_t;

typedef struct {
    boot_img_t mapped, other;         /* what was found (other: only if it was looked at) */
    bool       rollback;              /* the trial counter ran out */
    bool       vectors_ok;            /* the mapped bank starts with a plausible vector table */
    bool       after_swap;            /* this start follows a swap the bootloader made itself */
} boot_report_t;

/* trial: the backup register, read and updated in place. */
boot_action_t boot_decide(boot_io_t *io, uint32_t *trial, boot_report_t *rep);
boot_img_t    boot_check(boot_io_t *io, bool mapped);

/* the report register: magic | version << 16 | after_swap << 10 | vectors_ok << 9 | rollback << 8 |
 * other << 5 | mapped << 2 | action. decode: false if w is not a report (no bootloader ran). */
uint32_t boot_report_word(boot_action_t a, const boot_report_t *r);
bool     boot_report_decode(uint32_t w, boot_action_t *a, boot_report_t *r, unsigned *version);
const char *boot_img_name(boot_img_t i);    /* "ok", "none", "header", "hash", "signature" */

#endif
