/* plat_seqlock.h - publish data from the scan to lower-priority readers.
 *
 * The scan task (writer) copies the values that UI/web/Modbus display into a
 * snapshot at the end of every scan. Readers copy the snapshot out and retry if
 * a scan wrote it meanwhile. The writer never blocks or waits for a reader.
 *
 * RULE: a task with HIGHER priority than the writer must not depend on a read
 * succeeding (it would preempt the writer mid-write and could never see an even
 * sequence); plat_seq_read is bounded and returns false in that case. */
#ifndef PLAT_SEQLOCK_H
#define PLAT_SEQLOCK_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "plat_port.h"

typedef struct { volatile uint32_t seq; } plat_seqlock_t;

static inline void plat_seq_write_begin(plat_seqlock_t *l) { l->seq++; PLAT_DMB(); }
static inline void plat_seq_write_end(plat_seqlock_t *l)   { PLAT_DMB(); l->seq++; }

/* Test hook: called between the copy and the re-check (unit tests only). */
#if defined(PLAT_SEQ_TEST)
extern void (*plat_seq_test_hook)(void);
#endif

static inline bool plat_seq_read(const plat_seqlock_t *l, void *dst, const void *src,
                                 size_t n, unsigned max_tries) {
    for (unsigned k = 0; k < max_tries; ++k) {
        uint32_t s0 = l->seq;
        if (s0 & 1u) continue;                 /* write in progress */
        PLAT_DMB();
        memcpy(dst, src, n);
#if defined(PLAT_SEQ_TEST)
        if (plat_seq_test_hook) plat_seq_test_hook();
#endif
        PLAT_DMB();
        if (l->seq == s0) return true;
    }
    return false;
}

#endif
