/* plat_service.h - the unit's service history: operating hours, a service log, the
 * manufacturing test record, and a checksum of its configuration (Common Instrument Platform
 * Specification CIP-22.2, CIP-22.4, CIP-23.4, CIP-05.4).
 *
 * Operating hours: total powered hours and hours since the last service, in tenths of an hour
 * (so the store writes the factory image once every 6 minutes at most). A service log entry
 * starts "since service" again.
 *
 * Service log: the last PLAT_SVC_LOG entries (date, who, technician, the work done), entered at
 * factory login from the web page (POST /api/service/log) or by the platform's own callers.
 *
 * Test record: the result of the manufacturing test (date, station, pass/fail per test, up to
 * PLAT_SVC_TESTS tests in the order the test procedure defines), written at factory login
 * (POST /api/service/test).
 *
 * All three are kept in the FACTORY region of FRAM (dual copy; survive firmware updates and
 * restore defaults) and belong to this unit: a backup carries them for reference, but restoring
 * it into another unit never copies them (RF_UNIT).
 *
 * Configuration checksum: CRC-32 over the ID and value of every user setting (PF_PERSIST, not
 * factory, not secret), worked out once a second. Two units with the same checksum have the
 * same settings. Parameter 332, /api/info "config_crc", the System screen.
 *
 * Threading: writes are posted from any task and applied by the scan task (which owns the
 * blocks the store snapshots); readers copy under a sequence counter. */
#ifndef PLAT_SERVICE_H
#define PLAT_SERVICE_H
#include <stdint.h>
#include <stdbool.h>
#include "plat_config.h"

#ifndef PLAT_SVC_LOG
#if PLAT_FRAM_SIZE >= 131072u
#define PLAT_SVC_LOG   8u
#else
#define PLAT_SVC_LOG   2u       /* the 32 KB development FRAM */
#endif
#endif
#define PLAT_SVC_TESTS 24u

typedef struct {                /* 64 bytes */
    uint32_t time;              /* local s since 1970 (0 = clock not set) */
    uint8_t  level, src, rsvd[2];
    char     tech[16];          /* who did it, NUL-padded */
    char     work[40];          /* what was done */
} plat_svc_entry_t;

enum { PLAT_TEST_NOT_RUN = 0, PLAT_TEST_PASS, PLAT_TEST_FAIL };
typedef struct {                /* 44 bytes */
    uint32_t time;              /* 0 = no test recorded */
    char     station[12];
    uint8_t  n, level, passed, rsvd;   /* tests in result[]; passed = every one of them passed */
    uint8_t  result[PLAT_SVC_TESTS];   /* PLAT_TEST_* */
} plat_test_record_t;

void plat_service_scan(uint32_t now_ms);           /* scan task, every scan (plat_sys pre-logic) */

/* ---- writers: any task. 0 = queued, -1 a write is already waiting, -2 bad argument,
 *      -3 login level below factory ---- */
int plat_service_log(const char *tech, const char *work, uint8_t src, uint8_t level);
int plat_service_test(const char *station, const uint8_t *result, unsigned n, uint8_t src, uint8_t level);

/* ---- readers: any task ---- */
uint32_t plat_service_hours_x10(void);             /* total powered hours x 10 */
uint32_t plat_service_since_x10(void);             /* hours since the last service log entry x 10 */
bool     plat_service_entry(unsigned i, plat_svc_entry_t *out);   /* i = 0 newest; false = no more */
bool     plat_service_test_record(plat_test_record_t *out);       /* false = none recorded */
uint32_t plat_config_crc(void);                    /* the configuration checksum */

void plat_service_reset_for_test(void);

#endif
