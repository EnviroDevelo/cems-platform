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
    uint32_t fault_lr;          /* hard fault: the LR stacked by the CPU (the caller of the faulting function) */
    uint32_t fault_cfsr;        /* hard fault: SCB->CFSR (which fault: mem, bus, usage) */
    uint32_t fault_hfsr;        /* hard fault: SCB->HFSR (escalated, vector table read) */
    uint32_t fault_addr;        /* hard fault: the data address that faulted (MMFAR/BFAR), 0 if not known */
    uint32_t up_ms;             /* this boot's uptime, refreshed by the supervisor (how long the last boot ran) */
    uint32_t fault_armed;       /* a fault was recorded since this boot: a software reset after it is a crash */
    uint32_t loop_crashes;      /* crashes in a row, with less than PLAT_LOOP_WINDOW_MS of running between the first and now */
    uint32_t loop_run_ms;       /* running time since the first of them */
    uint32_t check;             /* ~ of the sum of the words above */
} plat_diag_t;

extern plat_diag_t plat_diag;

void plat_diag_boot(bsp_reset_cause_t cause);   /* once, early in plat_start() */

/* Reboot loop (CIP-08.5). A crash is a watchdog reset, or a software reset after a recorded
 * fault (plat_diag_fault: task silent, stack, heap, assert, CPU fault). PLAT_LOOP_RESETS
 * crashes with less than PLAT_LOOP_WINDOW_MS of running in total is a loop: the unit starts in
 * SAFE_MODE. A power cycle, a clean restart (firmware update) or a boot that runs for the
 * whole window clears it. */
void plat_diag_tick(uint32_t now_ms);           /* supervisor: keeps up_ms; clears the count after a long run */
bool plat_diag_reboot_loop(void);
void plat_diag_fault(plat_fault_t f, int32_t task, const char *name, uint32_t arg, uint32_t now_ms);
bool plat_diag_valid(void);

/* CPU fault (D4), called by the port's fault handler before the reset. pc and lr are
 * the stacked values (0 if the stack could not be read); cfsr, hfsr, mmfar and bfar are
 * the SCB registers as found; task is the running task's name, or NULL. Names the fault
 * "bus @0801A2C4" (kind and PC), or "stk scan" for a task that ran past its stack limit
 * (no usable PC then), which is what the touchscreen, the web page and the next boot's
 * event-log entry show. */
void plat_diag_cpu_fault(uint32_t pc, uint32_t lr, uint32_t cfsr, uint32_t hfsr,
                         uint32_t mmfar, uint32_t bfar, const char *task, uint32_t now_ms);

#endif
