/* plat_cmdq.c - see plat_cmdq.h. Ring of PLAT_CMDQ_DEPTH entries; head/tail are
 * free-running counters, so full = (head - tail == depth). Both ends run inside a
 * short critical section (a few word copies), which is simpler than lock-free
 * MPSC and costs well under a microsecond on the H5. */
#include <string.h>
#include "plat_cmdq.h"
#include "plat_port.h"

/* head/tail wrap at 2^32; index = counter % depth stays continuous only if the
 * depth divides 2^32. */
_Static_assert((PLAT_CMDQ_DEPTH & (PLAT_CMDQ_DEPTH - 1u)) == 0u, "PLAT_CMDQ_DEPTH must be a power of 2");

void plat_cmdq_init(plat_cmdq_t *q) { memset(q, 0, sizeof *q); }

bool plat_cmdq_post(plat_cmdq_t *q, const plat_cmd_t *c) {
    bool ok;
    PLAT_CRIT_ENTER();
    ok = (uint32_t)(q->head - q->tail) < PLAT_CMDQ_DEPTH;
    if (ok) { q->buf[q->head % PLAT_CMDQ_DEPTH] = *c; q->head++; }
    else    { q->dropped++; }
    PLAT_CRIT_EXIT();
    return ok;
}

bool plat_cmdq_post_batch(plat_cmdq_t *q, const plat_cmd_t *c, uint32_t n) {
    bool ok;
    PLAT_CRIT_ENTER();
    ok = (uint32_t)(q->head - q->tail) + n <= PLAT_CMDQ_DEPTH;
    if (ok) for (uint32_t k = 0; k < n; ++k) { q->buf[q->head % PLAT_CMDQ_DEPTH] = c[k]; q->head++; }
    else q->dropped++;
    PLAT_CRIT_EXIT();
    return ok;
}

bool plat_cmdq_pop(plat_cmdq_t *q, plat_cmd_t *out) {
    bool ok;
    PLAT_CRIT_ENTER();
    ok = q->head != q->tail;
    if (ok) { *out = q->buf[q->tail % PLAT_CMDQ_DEPTH]; q->tail++; }
    PLAT_CRIT_EXIT();
    return ok;
}

uint32_t plat_cmdq_count(const plat_cmdq_t *q) { return (uint32_t)(q->head - q->tail); }
