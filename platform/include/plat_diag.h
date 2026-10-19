/* plat_diag.h - fault record that survives a reset (not a power cycle).
 *
 * Placed in a .noinit RAM section (see docs/CUBEMX_SETUP.md, linker script), so
 * after a watchdog or fault reset the next boot can show which task stalled or
 * what failed, on the touchscreen and web page. It is also the input the
 * firmware-update "confirm or roll back" step will use later. */
#ifndef PLAT_DIAG_H
#define PLAT_DIAG_H
#include <stdint.h>
#include <stdbool.h>
#include "bsp.h"

typedef enum {
    PLAT_FAULT_NONE = 0,
    PLAT_FAULT_TASK_SILENT,     /* task_monitor: a task stopped checking in */
    PLAT_FAULT_STACK_OVERFLOW,  /* FreeRTOS stack overflow hook */
    PLAT_FAULT_MALLOC_FAILED,   /* FreeRTOS heap exhausted */
    PLAT_FAULT_ASSERT,          /* configASSERT */
    PLAT_FAULT_HARDFAULT        /* CPU fault handler (port) */
} plat_fault_t;

typedef struct {
    uint32_t magic;
    uint32_t boots;             /* resets since power-on */
    uint32_t wdg_resets;        /* of which IWDG resets */
    uint32_t fault;             /* plat_fault_t of the last recorded fault */
    int32_t  fault_task;        /* task_monitor id, or -1 */
    uint32_t fault_ms;          /* uptime when recorded */
    uint32_t fault_arg;         /* e.g. line number for an assert, PC for a hard fault */
    char     fault_name[16];    /* task name or file name */
    uint32_t last_reset_cause;  /* bsp_reset_cause_t of this boot */
    uint32_t check;             /* ~ of the sum of the words above */
} plat_diag_t;

extern plat_diag_t plat_diag;

void plat_diag_boot(bsp_reset_cause_t cause);   /* once, early in plat_start() */
void plat_diag_fault(plat_fault_t f, int32_t task, const char *name, uint32_t arg, uint32_t now_ms);
bool plat_diag_valid(void);

#endif
