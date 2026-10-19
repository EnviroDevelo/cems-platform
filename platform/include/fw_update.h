/* fw_update.h - firmware update over the network, with trial boot and rollback
 * (design spec "Firmware update"; decision D14).
 *
 * The STM32H573 has two 1 MB flash banks. The running image is always mapped at
 * the start of flash; the other bank is free. An update:
 *
 *   1. upload  the image file (.cfw) is streamed over HTTP into the free bank while
 *              the unit keeps running (no buffering of the whole file). The header is
 *              checked first (magic, CRC, product, size), the payload is hashed as it
 *              arrives, and at the end the flash is read back and hashed again.
 *   2. stage   the header goes into the last sector of that bank, so each bank says
 *              which version it holds.
 *   3. activate (admin) the boot record in FRAM says PENDING, the banks are swapped
 *              (option byte SWAP_BANK) and the unit restarts on the new image.
 *   4. trial   the new image runs in TRIAL. Once it has run healthily for
 *              FW_TRIAL_MS (scan running, every task checking in) it confirms itself:
 *              FW_UPDATE event, record IDLE.
 *   5. rollback if the trial image resets first (watchdog, fault, or a second power
 *              cut), the next boot swaps back to the previous image, which logs
 *              FW_ROLLBACK. The previous image is never erased by an update.
 *
 * Image file (.cfw, made by tools/mkfwimage.py): a 256-byte header, then the raw
 * binary linked for the start of flash. Integrity: SHA-256 over the payload, CRC over
 * the header. Authenticity: a signature hook (fw_signature_check) - unsigned images
 * are accepted while PLAT_FW_REQUIRE_SIGNED is 0 (decision D14).
 *
 * Threading
 *   fw_upload_*()        one uploader at a time (the HTTP task); writes the free bank only
 *   fw_status()          any task
 *   fw_request_activate() any task: the store task does the FRAM write and the swap
 *   fw_boot()            once at start-up, before the tasks (FRAM owner context)
 *   fw_service()         store task, periodically: activation, trial confirmation */
#ifndef FW_UPDATE_H
#define FW_UPDATE_H
#include <stdint.h>
#include <stdbool.h>
#include "nvm.h"

#define FW_MAGIC        0x31574643u   /* "CFW1" */
#define FW_HDR_SIZE     256u
#ifndef FW_TRIAL_MS
#define FW_TRIAL_MS     60000u        /* healthy running time before a new image confirms itself */
#endif
#ifndef PLAT_FW_REQUIRE_SIGNED
#define PLAT_FW_REQUIRE_SIGNED 0
#endif
#ifndef PLAT_FW_PRODUCT
#define PLAT_FW_PRODUCT 0x434D5331u   /* "1SMC": CEMS Supervisor hardware family 1 */
#endif

/* Header, little-endian, 256 bytes */
typedef struct {
    uint32_t magic;            /* FW_MAGIC */
    uint16_t hdr_size;         /* 256 */
    uint16_t hdr_version;      /* 1 */
    uint32_t product;          /* must equal PLAT_FW_PRODUCT (hardware compatibility) */
    uint32_t version;          /* major << 16 | minor << 8 | patch */
    char     version_str[24];  /* e.g. "0.4.0", NUL-padded */
    uint32_t image_size;       /* payload bytes */
    uint32_t flags;            /* FW_F_SIGNED */
    uint32_t build_time;       /* s since 1970 */
    uint8_t  sha256[32];       /* of the payload */
    uint8_t  sig[64];          /* ECDSA P-256 r||s over SHA-256(header bytes 0..111), if signed */
    uint8_t  rsvd[248 - 176];
    uint32_t rsvd2;
    uint32_t hdr_crc;          /* CRC-32 of bytes 0..251 */
} fw_header_t;
#define FW_F_SIGNED 0x0001u

/* The flash banks, from the port (port/stm32h5/fw_flash_stm32h5.c) or a simulation. */
typedef struct fw_flash {
    uint32_t bank_size;        /* 1 MB on the H573 */
    uint32_t sector;           /* erase unit, 8 KB */
    uint32_t prog_unit;        /* program unit, 16 bytes (quad-word) */
    int  (*erase)(struct fw_flash *f, uint32_t off);                       /* free bank, sector at off */
    int  (*write)(struct fw_flash *f, uint32_t off, const void *p, uint32_t n);   /* free bank, aligned */
    int  (*read)(struct fw_flash *f, bool active, uint32_t off, void *p, uint32_t n);
    void (*swap_and_reset)(struct fw_flash *f);   /* toggles the bank swap; on the target does not return */
    void *ctx;
} fw_flash_t;

typedef enum {
    FW_OK = 0,
    FW_E_BUSY,          /* another upload is running */
    FW_E_STATE,         /* call out of order */
    FW_E_HEADER,        /* not an image file, or damaged header */
    FW_E_PRODUCT,       /* built for other hardware */
    FW_E_SIZE,          /* larger than the bank, or file length disagrees */
    FW_E_FLASH,         /* erase/write/read-back failed */
    FW_E_HASH,          /* SHA-256 mismatch (damaged in transfer or in flash) */
    FW_E_SIGNATURE,     /* signature missing or wrong */
    FW_E_NO_IMAGE       /* activate: nothing staged */
} fw_err_t;

typedef enum {          /* boot record state (FRAM) */
    FW_ST_IDLE = 0,
    FW_ST_PENDING,      /* swap done, new image not yet started */
    FW_ST_TRIAL,        /* new image running, not yet confirmed */
    FW_ST_ROLLED_BACK   /* trial failed, back on the previous image */
} fw_state_t;

typedef struct {
    fw_state_t state;
    uint32_t   upload_bytes, upload_total;   /* progress of a running upload */
    bool       uploading;
    bool       staged;                       /* a verified image waits in the free bank */
    char       staged_version[24];
    char       running_version[24];
    uint32_t   trial_left_ms;                /* in TRIAL */
    fw_err_t   last_error;
    bool       rolled_back;                  /* this boot came back from a failed trial */
} fw_status_t;

/* Set up. fram_addr: 64 bytes for the boot record (STORE_META_ADDR + 128).
 * running_version: this image's version text. */
void fw_init(fw_flash_t *flash, nvm_dev_t *fram, uint32_t fram_addr, const char *running_version);

/* Start-up: TRIAL after a reset -> roll back (does not return on the target);
 * PENDING -> TRIAL; ROLLED_BACK -> log it, IDLE. reset_cause: bsp_reset_cause_t. */
void fw_boot(uint8_t reset_cause, uint32_t now_ms);

/* Streaming upload. begin with the total file length; feed any chunk sizes. */
fw_err_t fw_upload_begin(uint32_t file_len);
fw_err_t fw_upload_data(const uint8_t *p, uint32_t n);
fw_err_t fw_upload_end(void);     /* checks hash and signature, stages the image */
void     fw_upload_abort(void);

fw_err_t fw_request_activate(void);           /* any task; the store task swaps */
void     fw_service(uint32_t now_ms, bool healthy);   /* store task */
void     fw_status(fw_status_t *out);
const char *fw_err_name(fw_err_t e);

/* Signature hook: return true if sig is a valid signature of hash (32 bytes) by the
 * product key. The weak default accepts nothing (signed images then fail), so a
 * signing scheme is a strong definition of this function. */
bool fw_signature_check(const uint8_t hash[32], const uint8_t sig[64]);

/* Helpers, also used by the tests and the image tool's self-check. */
bool     fw_header_ok(const fw_header_t *h);
uint32_t fw_parse_version(const char *s);     /* "1.2.3-dev" -> 0x010203 */

void fw_reset_for_test(void);

#endif
