/* plat_health.h - how much memory is left (design spec: "72-hour soak test with heap
 * and socket monitoring"). The supervisor task samples it every 5 s on the target;
 * the web page (/api/info "health"), the System screen and alarm 11 RESOURCE_LOW show
 * it, so a slow leak is seen long before it stops the unit.
 *
 * RESOURCE_LOW: the FreeRTOS heap's lowest-ever free space below PLAT_HEAP_LOW, or a
 * task's stack margin (lowest-ever unused) below PLAT_STACK_LOW words. */
#ifndef PLAT_HEALTH_H
#define PLAT_HEALTH_H
#include <stdint.h>
#include <stdbool.h>

#define PLAT_HEALTH_TASKS 16
#ifndef PLAT_HEAP_LOW
#define PLAT_HEAP_LOW  4096u      /* bytes */
#endif
#ifndef PLAT_STACK_LOW
#define PLAT_STACK_LOW 64u        /* words (256 bytes) */
#endif

typedef struct {
    char     name[12];
    uint16_t free_words;          /* the stack's lowest-ever unused space */
} plat_task_health_t;

typedef struct {
    bool     valid;               /* sampled at least once (false on the PC builds) */
    uint32_t heap_size, heap_free, heap_min;   /* FreeRTOS heap (lwIP's memory), bytes */
    uint8_t  n_tasks;
    plat_task_health_t task[PLAT_HEALTH_TASKS];
    uint32_t sampled_ms;
} plat_health_t;

extern plat_health_t plat_health;           /* written by the supervisor; a torn read only shows one stale figure */
bool plat_health_low(const plat_health_t *h, char *why, unsigned cap);   /* true = RESOURCE_LOW, with the reason */

#endif
