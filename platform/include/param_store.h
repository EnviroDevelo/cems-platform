/* param_store.h - non-volatile instrument data (design spec "Non-volatile data
 * storage", "Parameter store rules").
 *
 * What is stored
 *   - every parameter with PF_PERSIST (param.h), and
 *   - retained blocks: strings, arrays and structs the application keeps across
 *     power-off (the old project retains 342 tags, 10.8 KB, mostly strings and
 *     buffers; 20_Data_Dictionary.md). Each block is registered with an ID.
 *
 * Two independent regions, each with two copies (A/B):
 *   FACTORY - calibration, serial, hardware revision (PF_FACTORY / RF_FACTORY).
 *             Never touched by "restore defaults".
 *   USER    - everything else that persists.
 *
 * Image format (per copy): 32-byte header + records.
 *   header: magic "CEMP", header size, layout version, region, sequence,
 *           payload length, payload CRC-32, header CRC-32
 *   record: u16 id, u16 length, data, padded to 4 bytes
 * Records are found by ID, so new firmware loads older images directly: unknown
 * IDs are skipped, missing IDs keep their defaults, and calibration survives.
 * A layout change that renumbers or rescales data goes through the migrate hook.
 *
 * Writes alternate between the copies with sequence + 1; boot uses the newest
 * copy that passes both CRCs. A power cut part way through a write therefore
 * always leaves the previous copy intact (tested at every byte offset).
 *
 * Fail safe: if neither FACTORY copy is valid, PSTAT_CAL_INVALID is set and stays
 * set; the application must block measurement (param_store_calibration_valid()).
 * The defaults are NOT written over the factory region until someone actually
 * changes a factory value, so a corrupted image is never silently replaced.
 *
 * Threading
 *   param_store_boot()          once, before the first scan (single-threaded)
 *   param_store_scan_service()  scan task, end of every scan
 *   param_store_poll()          store task, periodically
 *   export / import             store task (the HTTP module will hand requests over) */
#ifndef PARAM_STORE_H
#define PARAM_STORE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "nvm.h"
#include "param.h"

#define RF_FACTORY 0x01u

typedef struct {
    uint16_t    id;        /* shares the parameter ID space, >= PARAM_ID_APP_FIRST */
    uint16_t    size;      /* bytes */
    uint8_t     flags;     /* RF_FACTORY */
    void       *ptr;
    const void *dflt;      /* power-up / restore-defaults content; NULL = all zero */
    const char *name;
} retain_def_t;

#define RETAIN(ID, NAME, VAR, FLAGS, DFLT) \
    { .id = (ID), .size = (uint16_t)sizeof(VAR), .flags = (FLAGS), .ptr = &(VAR), .dflt = (DFLT), .name = (NAME) }

/* Migration hook: called for every record of an image whose layout version is
 * older than the running firmware's. May change the data and length in place
 * (up to cap bytes). Return true to keep the record, false to drop it. */
typedef bool (*param_migrate_fn)(uint16_t from_version, uint16_t id, uint8_t *data, uint16_t *len, uint16_t cap);

/* Region layout in the NVM device (byte addresses). Default for a 128 KB FRAM. */
typedef struct {
    uint32_t fac_a, fac_b, fac_slot;
    uint32_t usr_a, usr_b, usr_slot;
} store_layout_t;

/* Memory map, chosen by FRAM size.
 *   32 KB (development part: Adafruit 1895 = MB85RC256V, I2C):
 *     FAC A 0x0000, FAC B 0x0400 (1 KB each), USR A 0x0800, USR B 0x4200 (14.5 KB each),
 *     0x7C00-0x7FFF (1 KB) reserved for the logger's write pointer.
 *     User copies hold the 10.8 KB the old project retains plus ~1.9 KB record
 *     overhead, leaving ~2 KB headroom (decision D6).
 *   128 KB (e.g. MB85RC1MT I2C, or an SPI part):
 *     FAC 0x00000/0x01000 (4 KB), USR 0x02000/0x0C000 (40 KB), 0x16000+ reserved. */
#include "plat_config.h"
#if PLAT_FRAM_SIZE >= 131072u
#define STORE_FAC_SLOT  4096u
#define STORE_USR_SLOT  40960u
#define STORE_FAC_A     0x00000u
#define STORE_FAC_B     0x01000u
#define STORE_USR_A     0x02000u
#define STORE_USR_B     0x0C000u
#define STORE_META_ADDR 0x16000u
#else
#define STORE_FAC_SLOT  1024u
#define STORE_USR_SLOT  14848u
#define STORE_FAC_A     0x0000u
#define STORE_FAC_B     0x0400u
#define STORE_USR_A     0x0800u
#define STORE_USR_B     0x4200u
#define STORE_META_ADDR 0x7C00u
#endif
#ifndef STORE_SAVE_PERIOD_MS
#define STORE_SAVE_PERIOD_MS 1000u       /* changes reach FRAM within about 1 s */
#endif
#define STORE_HDR_SIZE  32u
extern const store_layout_t store_layout_default;   /* the map above */

/* Status bits (also published as parameter PARAM_ID_STORE_STATUS) */
#define PSTAT_FAC_VALID      0x0001u  /* a valid factory image was loaded */
#define PSTAT_USR_VALID      0x0002u  /* a valid user image was loaded */
#define PSTAT_CAL_INVALID    0x0004u  /* ALARM: no valid factory data, block measurement */
#define PSTAT_USR_DEFAULTED  0x0008u  /* warning: user settings are defaults (no valid image) */
#define PSTAT_NVM_VOLATILE   0x0010u  /* running on the RAM fallback: nothing survives power-off */
#define PSTAT_WRITE_ERROR    0x0020u  /* last write failed verification (retried next period) */
#define PSTAT_MIGRATED       0x0040u  /* an older layout was loaded and migrated */
#define PSTAT_REC_SKIPPED    0x0080u  /* a record was ignored: unknown ID, wrong size, out of range */
#define PSTAT_NEWER_LAYOUT   0x0100u  /* image came from a newer firmware (downgrade) */
#define PSTAT_TABLE_ERROR    0x0200u  /* registry error: duplicate ID or region too small */

typedef struct {
    uint32_t saves_fac, saves_usr, write_errors, skipped_records;
    uint32_t seq_fac, seq_usr;
    uint32_t payload_fac, payload_usr;    /* bytes */
} store_stats_t;

/* Register blocks and bind the device. Call after param_init() and before boot. */
bool param_store_init(nvm_dev_t *dev, const store_layout_t *layout, const retain_def_t *blocks, size_t n_blocks,
                      uint16_t layout_version, param_migrate_fn migrate);

/* Set every parameter and block to its default, then load both regions. */
void param_store_boot(void);

void param_store_scan_service(void);          /* scan task, end of every scan */
void param_store_poll(uint32_t now_ms);       /* store task; returns quickly, call every 10..100 ms */

bool          param_store_calibration_valid(void);
uint32_t      param_store_status(void);
store_stats_t param_store_stats(void);

/* Backup file: both regions as currently stored, wrapped with its own CRC.
 * Returns bytes written to buf, 0 if cap is too small or nothing is stored. */
uint32_t param_store_export(uint8_t *buf, uint32_t cap);

/* Restore from a backup file. Checks every CRC first; the factory part is only
 * applied when level == PLAT_LVL_FACTORY. Values are applied at the end of the
 * next scan and saved by the following poll. Returns 0 if accepted, < 0 if the
 * file is invalid (nothing changed). The buffer must stay valid until
 * param_store_import_busy() returns false. */
int  param_store_import(const uint8_t *buf, uint32_t len, uint8_t level);
bool param_store_import_busy(void);

#endif
