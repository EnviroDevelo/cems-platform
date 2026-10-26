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
 * the header. Authenticity (decision D14): an ECDSA P-256 signature over the header
 * (which holds the payload's SHA-256), made by tools/mkfwimage.py with the product's
 * private key - a tech-level credential that never leaves the technicians - and
 * checked against the public key built into the firmware (fw_pubkey.c, made by
 * tools/fwkey.py). A build with a public key refuses unsigned or wrongly signed
 * images. A development build without one accepts unsigned images and says so on the
 * web page and the touchscreen.
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
#include <stddef.h>
#include "nvm.h"

#define FW_MAGIC        0x31574643u   /* "CFW1" */
#define FW_HDR_SIZE     256u
#ifndef FW_TRIAL_MS
#define FW_TRIAL_MS     60000u        /* healthy running time before a new image confirms itself */
#endif
extern uint32_t fw_trial_ms;          /* = FW_TRIAL_MS; the PC simulation shortens it */
#ifndef PLAT_FW_PRODUCT
#define PLAT_FW_PRODUCT 0x434D5331u   /* "1SMC": CEMS Supervisor hardware family 1 */
#endif

/* Header, little-endian, 256 bytes */
typedef struct {
    uint32_t magic;            /* FW_MAGIC */
    uint16_t hdr_size;         /* 256 */
    uint16_t hdr_version;      /* 1, or 2 (adds the fields after sig, and signs them) */
    uint32_t product;          /* must equal PLAT_FW_PRODUCT (hardware compatibility) */
    uint32_t version;          /* major << 16 | minor << 8 | patch */
    char     version_str[24];  /* e.g. "0.4.0", NUL-padded */
    uint32_t image_size;       /* payload bytes */
    uint32_t flags;            /* FW_F_SIGNED */
    uint32_t build_time;       /* s since 1970 */
    uint8_t  sha256[32];       /* of the payload */
    uint8_t  sig[64];          /* ECDSA P-256 r||s over SHA-256(header bytes 0..83), if signed */
    uint8_t  hw_min, hw_max;   /* v2: board revisions it runs on (straps, CORE_PCB.md 12), inclusive */
    uint16_t layout_version;   /* v2: settings layout it was built with (plat_app_t; 0 = not stated) */
    char     build_id[12];     /* v2: source revision (git short hash), NUL-padded */
    uint8_t  rsvd[84];
    uint32_t rsvd2;
    uint32_t hdr_crc;          /* CRC-32 of bytes 0..251 */
} fw_header_t;
#define FW_SIGNED_LEN 84u      /* header bytes covered by the signature (up to and including sha256) */
/* v2 also signs bytes FW_SIGNED2_OFF..251 (everything after the signature but the CRC):
 * the message is header[0..83] followed by header[148..251]. */
#define FW_SIGNED2_OFF 148u
_Static_assert(sizeof(fw_header_t) == FW_HDR_SIZE, "fw_header_t must be 256 bytes");
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
    FW_E_NO_IMAGE,      /* activate: nothing staged */
    FW_E_NOT_SAFE,      /* activate: the application is not in a safe state to restart (fw_safe_check) */
    FW_E_OLD,           /* older than the minimum version allowed (parameter 88, anti-rollback) */
    FW_E_WAIT_LOCAL,    /* activate from the web/PLC: now waiting for confirmation on the touchscreen */
    FW_E_HARDWARE       /* built for other board revisions (v2 header, the board's straps; CIP-03.2) */
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
    bool       signing_required;             /* false = development build: unsigned images accepted */
    bool       wait_local;                   /* an activation asked from afar waits for the touchscreen */
    uint8_t    wait_src, wait_level;         /* ... who asked */
    uint32_t   wait_left_ms;
    char       not_safe[48];                 /* why the last activation was refused as not safe ("" = it wasn't) */
    char       staged_build[12];             /* build ID of the staged image ("" = none stated) */
    uint8_t    staged_hw_min, staged_hw_max; /* board revisions it runs on (0..255 for a v1 image) */
} fw_status_t;

/* The board revision from its straps (bsp_hw_straps, read in fw_init): 0.., or -1 if this board
 * has none (then the hardware range of an image is not checked). Parameter 319. */
int fw_board_rev(void);

/* Build ID of the running firmware: PLAT_BUILD_ID, set by the build (git short hash); "dev" if not. */
const char *fw_build_id(void);

/* Update history (FRAM, survives power cuts and "restore defaults"): the last
 * FW_HIST_MAX firmware changes, newest first. Times are the instrument's clock (local
 * standard time, s since 1970); a time is 0 with its FWH_*_CLOCK flag clear if the
 * clock was not set then. */
#define FW_HIST_MAX 6u
enum { FWH_UPDATED = 1,        /* the new firmware passed its trial and was kept */
       FWH_ROLLED_BACK = 2 };  /* it failed its trial: the unit went back to "from" */
#define FWH_INST_CLOCK 0x01u   /* t_installed is a real time */
#define FWH_DONE_CLOCK 0x02u   /* t_done is a real time */
typedef struct {
    uint32_t t_installed;      /* restart onto the new firmware */
    uint32_t t_done;           /* kept (trial passed) or rolled back */
    uint8_t  kind;             /* FWH_* */
    uint8_t  flags;            /* FWH_*_CLOCK */
    uint8_t  src, level;       /* who asked for the restart (plat_src_t, plat_level_t) */
    char     from[12], to[12]; /* versions */
} fw_hist_t;
size_t fw_history(fw_hist_t *out, size_t max);   /* any task (a copy); returns the entries */

/* Set up. fram_addr: 128 bytes for the boot record (STORE_META_ADDR + 128) and,
 * from fram_addr + 256, 464 bytes for the update history (STORE_META_ADDR + 384..847).
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

fw_err_t fw_request_activate(void);           /* any task; the store task swaps (as from the touchscreen) */
/* The same, saying who asks (plat_src_t, plat_level_t). Checks, in order: an image is
 * staged and allowed (minimum version), the application says it is safe to restart
 * (fw_safe_check), and, when parameter 87 fw_local_confirm is on and the request is
 * not from the touchscreen, holds it for 5 minutes for the touchscreen to confirm
 * (FW_E_WAIT_LOCAL). Every outcome is in the event log. */
fw_err_t fw_request_activate_from(uint8_t src, uint8_t level);
void     fw_local_answer(bool yes, uint8_t level);   /* the touchscreen's answer to a held activation */
/* The application's safe-state check (plat_app_t.fw_safe; NULL = always safe): false
 * and a reason (e.g. "a calibration is running") refuses an activation. Called from
 * the requesting task and again by the store task just before the swap. */
extern bool (*fw_safe_check)(char *why, size_t cap);
void     fw_service(uint32_t now_ms, bool healthy);   /* store task */
void     fw_status(fw_status_t *out);
const char *fw_err_name(fw_err_t e);

/* Signatures (fw_sig.c): true if sig (r||s) is a valid ECDSA P-256 signature of hash
 * by the product key; false always when the build has no key. */
bool fw_signature_check(const uint8_t hash[32], const uint8_t sig[64]);
bool fw_signing_required(void);              /* this build has the product's public key */
extern const uint8_t fw_pubkey[65];          /* uncompressed P-256 point (fw_pubkey.c) */
extern const bool    fw_pubkey_present;
void fw_pubkey_override_for_test(const uint8_t *q65);   /* NULL = back to the built-in key */

/* Called by fw_service just before the swap (the store task flushes the parameter
 * store and the log through it). May be NULL. */
extern void (*fw_before_swap)(void);

/* Helpers, also used by the tests and the image tool's self-check. */
bool     fw_header_ok(const fw_header_t *h);
uint32_t fw_parse_version(const char *s);     /* "1.2.3-dev" -> 0x010203 */

void fw_reset_for_test(void);

#endif
