/* plat_cmdq.h - queue of external writes into the scan.
 *
 * The touch UI, web server and Modbus server never write application tags
 * directly: they post a plat_cmd_t here, and the scan task applies queued commands
 * between scans (before the logic runs), so the logic never sees a value change
 * half way through a scan. Many producers (lower-priority tasks), one consumer
 * (the scan task). The apply step goes through the shared parameter table, which
 * enforces range limits, interlocks and local/remote arbitration (next module).
 *
 * Not ISR-safe: post from tasks only. */
#ifndef PLAT_CMDQ_H
#define PLAT_CMDQ_H
#include <stdint.h>
#include <stdbool.h>
#include "plat_config.h"

typedef enum {
    PLAT_SRC_LOCAL = 0,   /* touchscreen */
    PLAT_SRC_WEB,
    PLAT_SRC_MODBUS,
    PLAT_SRC_INTERNAL     /* platform itself, e.g. parameter restore */
} plat_src_t;

typedef enum {
    PLAT_VAL_I32 = 0,
    PLAT_VAL_U32,
    PLAT_VAL_F32,
    PLAT_VAL_BOOL
} plat_val_kind_t;

/* One scalar value as carried between tasks. */
typedef union { int32_t i; uint32_t u; float f; bool b; } plat_val_t;

/* Write authority of the requester (from the UI or web login). */
typedef enum {
    PLAT_LVL_VIEW = 0,    /* not logged in: read only */
    PLAT_LVL_OPERATOR,
    PLAT_LVL_ADMIN,
    PLAT_LVL_FACTORY
} plat_level_t;

typedef struct {
    uint16_t   id;        /* parameter-table ID */
    uint8_t    src;       /* plat_src_t */
    uint8_t    kind;      /* plat_val_kind_t */
    uint8_t    level;     /* plat_level_t */
    uint8_t    rsvd[3];
    plat_val_t v;
    uint32_t   token;     /* caller's reference, returned with the result */
} plat_cmd_t;

typedef struct {
    plat_cmd_t buf[PLAT_CMDQ_DEPTH];
    volatile uint32_t head;   /* next write, producers */
    volatile uint32_t tail;   /* next read, consumer   */
    volatile uint32_t dropped;
} plat_cmdq_t;

void plat_cmdq_init(plat_cmdq_t *q);
bool plat_cmdq_post(plat_cmdq_t *q, const plat_cmd_t *c);   /* false if full (counted) */
/* All n or none, inside one critical section, so the scan sees the whole batch or none of it. */
bool plat_cmdq_post_batch(plat_cmdq_t *q, const plat_cmd_t *c, uint32_t n);
bool plat_cmdq_pop(plat_cmdq_t *q, plat_cmd_t *out);
uint32_t plat_cmdq_count(const plat_cmdq_t *q);

#endif
