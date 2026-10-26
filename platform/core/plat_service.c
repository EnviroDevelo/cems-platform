/* plat_service.c - see plat_service.h */
#include <string.h>
#include <math.h>
#include "plat_service.h"
#include "param.h"
#include "param_store.h"
#include "nvm.h"
#include "plat_clock.h"
#include "plat_log.h"
#include "plat_port.h"
#include "plat_cmdq.h"

#define STEP_MS 360000u                     /* 0.1 h */

typedef struct { uint32_t total_x10, base_x10, services, rsvd; } counters_t;
typedef struct { uint16_t head, count; uint32_t rsvd; plat_svc_entry_t e[PLAT_SVC_LOG]; } svc_log_t;

/* FACTORY region, this unit's own (RF_UNIT) */
static counters_t         cnt;
static svc_log_t          slog;
static plat_test_record_t test;

static const retain_def_t blocks[] = {
    { .id = 905, .size = sizeof cnt,  .ptr = &cnt,  .flags = RF_FACTORY | RF_UNIT, .name = "svc_hours" },
    { .id = 906, .size = sizeof slog, .ptr = &slog, .flags = RF_FACTORY | RF_UNIT, .name = "svc_log" },
    { .id = 907, .size = sizeof test, .ptr = &test, .flags = RF_FACTORY | RF_UNIT, .name = "svc_test" },
};
const retain_def_t *plat_service_blocks(size_t *n) { *n = sizeof blocks / sizeof blocks[0]; return blocks; }

uint32_t plat_svc_hours_tag, plat_svc_since_tag, plat_config_crc_tag;   /* parameters 330-332 */

static volatile uint32_t gen;               /* odd while the scan task changes a block */
static uint32_t acc_ms, last_ms, crc_ms;
static bool     started;

static struct {                             /* one waiting write of each kind (any task -> scan) */
    volatile bool log, test;
    plat_svc_entry_t   e;
    plat_test_record_t t;
} req;

static void copy_text(char *dst, size_t cap, const char *src) {
    memset(dst, 0, cap);
    for (size_t k = 0; src && src[k] && k + 1 < cap; ++k) dst[k] = src[k];
}

static uint32_t clock_now(void) { uint32_t t; return plat_clock_now(&t) ? t : 0u; }

int plat_service_log(const char *tech, const char *work, uint8_t src, uint8_t level) {
    if (level < PLAT_LVL_FACTORY) return -3;
    if (!tech || !tech[0] || !work || !work[0]) return -2;
    int r = 0;
    PLAT_CRIT_ENTER();
    if (req.log) r = -1;
    else {
        memset(&req.e, 0, sizeof req.e);
        req.e.level = level; req.e.src = src;
        copy_text(req.e.tech, sizeof req.e.tech, tech);
        copy_text(req.e.work, sizeof req.e.work, work);
        req.log = true;
    }
    PLAT_CRIT_EXIT();
    return r;
}

int plat_service_test(const char *station, const uint8_t *result, unsigned n, uint8_t src, uint8_t level) {
    (void)src;
    if (level < PLAT_LVL_FACTORY) return -3;
    if (!result || n == 0 || n > PLAT_SVC_TESTS) return -2;
    for (unsigned k = 0; k < n; ++k) if (result[k] > PLAT_TEST_FAIL) return -2;
    int r = 0;
    PLAT_CRIT_ENTER();
    if (req.test) r = -1;
    else {
        memset(&req.t, 0, sizeof req.t);
        copy_text(req.t.station, sizeof req.t.station, station);
        req.t.n = (uint8_t)n; req.t.level = level;
        bool pass = true;
        for (unsigned k = 0; k < n; ++k) { req.t.result[k] = result[k]; pass = pass && result[k] == PLAT_TEST_PASS; }
        req.t.passed = pass;
        req.test = true;
    }
    PLAT_CRIT_EXIT();
    return r;
}

/* CRC-32 over (ID, value) of every user setting */
static uint32_t config_crc(void) {
    uint32_t c = 0;
    for (size_t i = 0; i < param_count(); ++i) {
        const param_def_t *d = param_at(i);
        if (!(d->flags & PF_PERSIST) || (d->flags & (PF_FACTORY | PF_SECRET | PF_RO))) continue;
        uint8_t id[2] = { (uint8_t)d->id, (uint8_t)(d->id >> 8) };
        c = plat_crc32(c, id, 2);
        c = plat_crc32(c, d->ptr, PARAM_IS_BLOB(d) ? d->size : (uint32_t)param_ctype_size(d->ctype));
    }
    return c;
}

void plat_service_scan(uint32_t now_ms) {
    if (!started) { started = true; last_ms = now_ms; crc_ms = now_ms - 1000u; }
    acc_ms += now_ms - last_ms;
    last_ms = now_ms;
    if (acc_ms >= STEP_MS || req.log || req.test) {
        ++gen;
        while (acc_ms >= STEP_MS) { acc_ms -= STEP_MS; cnt.total_x10++; }
        if (req.log) {
            plat_svc_entry_t e = req.e;
            e.time = clock_now();
            unsigned at = slog.head % PLAT_SVC_LOG;
            slog.e[at] = e;
            slog.head = (uint16_t)((at + 1u) % PLAT_SVC_LOG);
            if (slog.count < PLAT_SVC_LOG) slog.count++;
            cnt.base_x10 = cnt.total_x10;            /* "since service" starts again */
            cnt.services++;
            req.log = false;
            plat_log_event(LOG_EV_SERVICE, e.src, e.level, cnt.services, NAN, NAN, e.tech);
        }
        if (req.test) {
            test = req.t;
            test.time = clock_now();
            req.test = false;
            plat_log_event(LOG_EV_SERVICE, test.level ? PLAT_SRC_WEB : PLAT_SRC_INTERNAL, test.level, 0,
                           NAN, test.passed, test.passed ? "test pass" : "test fail");
        }
        ++gen;
    }
    plat_svc_hours_tag = cnt.total_x10;
    plat_svc_since_tag = cnt.total_x10 >= cnt.base_x10 ? cnt.total_x10 - cnt.base_x10 : 0u;
    if ((uint32_t)(now_ms - crc_ms) >= 1000u) { crc_ms = now_ms; plat_config_crc_tag = config_crc(); }
}

static void copy(void *dst, const void *src, size_t n) {
    for (int tries = 0; tries < 8; ++tries) {
        uint32_t g = gen;
        if (g & 1u) continue;
        memcpy(dst, src, n);
        if (gen == g) return;
    }
    memcpy(dst, src, n);
}

uint32_t plat_service_hours_x10(void) { return plat_svc_hours_tag; }
uint32_t plat_service_since_x10(void) { return plat_svc_since_tag; }
uint32_t plat_config_crc(void)        { return plat_config_crc_tag; }

bool plat_service_entry(unsigned i, plat_svc_entry_t *out) {
    unsigned n = slog.count, head = slog.head;
    if (i >= n || n > PLAT_SVC_LOG || head >= PLAT_SVC_LOG) return false;
    copy(out, &slog.e[(head + PLAT_SVC_LOG - 1u - i) % PLAT_SVC_LOG], sizeof *out);
    return true;
}

bool plat_service_test_record(plat_test_record_t *out) {
    copy(out, &test, sizeof *out);
    return out->n != 0 && out->n <= PLAT_SVC_TESTS;
}

void plat_service_reset_for_test(void) {
    memset(&cnt, 0, sizeof cnt); memset(&slog, 0, sizeof slog); memset(&test, 0, sizeof test);
    memset(&req, 0, sizeof req);
    acc_ms = 0; started = false;
    plat_svc_hours_tag = plat_svc_since_tag = plat_config_crc_tag = 0;
}
