/* plat_diag.c - see plat_diag.h */
#include <stddef.h>
#include <string.h>
#include "plat_diag.h"
#include "plat_config.h"

#define DIAG_MAGIC 0xCE5D1A60u

#if defined(PLAT_HOST)
plat_diag_t plat_diag;
#else
__attribute__((section(".noinit"))) plat_diag_t plat_diag;
#endif

static uint32_t diag_sum(const plat_diag_t *d) {
    const uint32_t *w = (const uint32_t *)d;
    uint32_t s = 0;
    for (size_t i = 0; i < offsetof(plat_diag_t, check) / 4u; ++i) s += w[i] ^ (uint32_t)(i * 0x9E3779B9u);
    return ~s;
}

bool plat_diag_valid(void) {
    return plat_diag.magic == DIAG_MAGIC && plat_diag.check == diag_sum(&plat_diag);
}

void plat_diag_boot(bsp_reset_cause_t cause) {
    if (cause == BSP_RESET_POWER_ON || cause == BSP_RESET_BROWNOUT || !plat_diag_valid()) {
        memset(&plat_diag, 0, sizeof plat_diag);   /* RAM content is random after power loss */
        plat_diag.magic = DIAG_MAGIC;
        plat_diag.fault_task = -1;
    }
    plat_diag.boots++;
    if (cause == BSP_RESET_IWDG) plat_diag.wdg_resets++;
    bool crash = cause == BSP_RESET_IWDG || cause == BSP_RESET_WWDG || (cause == BSP_RESET_SOFTWARE && plat_diag.fault_armed);
    if (!crash) {
        plat_diag.loop_crashes = 0; plat_diag.loop_run_ms = 0;
    } else if (plat_diag.loop_crashes == 0 || plat_diag.loop_run_ms + plat_diag.up_ms > PLAT_LOOP_WINDOW_MS) {
        plat_diag.loop_crashes = 1; plat_diag.loop_run_ms = plat_diag.up_ms;   /* the first of a new series */
    } else {
        plat_diag.loop_crashes++; plat_diag.loop_run_ms += plat_diag.up_ms;
    }
    plat_diag.up_ms = 0;
    plat_diag.fault_armed = 0;
    plat_diag.last_reset_cause = (uint32_t)cause;
    plat_diag.check = diag_sum(&plat_diag);
}

void plat_diag_fault(plat_fault_t f, int32_t task, const char *name, uint32_t arg, uint32_t now_ms) {
    if (plat_diag.magic != DIAG_MAGIC) { memset(&plat_diag, 0, sizeof plat_diag); plat_diag.magic = DIAG_MAGIC; }
    plat_diag.fault = (uint32_t)f;
    plat_diag.fault_task = task;
    plat_diag.fault_ms = now_ms;
    plat_diag.fault_arg = arg;
    plat_diag.fault_armed = 1;
    plat_diag.fault_lr = plat_diag.fault_cfsr = plat_diag.fault_hfsr = plat_diag.fault_addr = 0;
    memset(plat_diag.fault_name, 0, sizeof plat_diag.fault_name);
    if (name) strncpy(plat_diag.fault_name, name, sizeof plat_diag.fault_name - 1u);
    plat_diag.check = diag_sum(&plat_diag);
}

void plat_diag_tick(uint32_t now_ms) {
    plat_diag.up_ms = now_ms;
    if (plat_diag.loop_crashes && now_ms > PLAT_LOOP_WINDOW_MS) { plat_diag.loop_crashes = 0; plat_diag.loop_run_ms = 0; }
    plat_diag.check = diag_sum(&plat_diag);
}

bool plat_diag_reboot_loop(void) { return plat_diag.loop_crashes >= PLAT_LOOP_RESETS; }

/* CFSR bits used below (ARMv8-M) */
#define CFSR_MMARVALID  (1u << 7)
#define CFSR_BFARVALID  (1u << 15)
#define CFSR_STKOF      (1u << 20)
#define HFSR_VECTTBL    (1u << 1)

void plat_diag_cpu_fault(uint32_t pc, uint32_t lr, uint32_t cfsr, uint32_t hfsr,
                         uint32_t mmfar, uint32_t bfar, const char *task, uint32_t now_ms) {
    /* no printf here: this runs in the fault handler */
    const char *kind = (cfsr & CFSR_STKOF) ? "stack" : (cfsr & 0xFFFF0000u) ? "usage" : (cfsr & 0x0000FF00u) ? "bus"
                     : (cfsr & 0x000000FFu) ? "mem" : (hfsr & HFSR_VECTTBL) ? "vect" : "hard";
    char name[sizeof plat_diag.fault_name];
    size_t n = 0;
    while (kind[n]) { name[n] = kind[n]; ++n; }
    if ((cfsr & CFSR_STKOF) && task && task[0]) {      /* the task matters more than a PC that isn't there */
        n = 0; name[n++] = 's'; name[n++] = 't'; name[n++] = 'k'; name[n++] = ' ';
        while (*task && n < sizeof name - 1u) name[n++] = *task++;
    } else {
        name[n++] = ' '; name[n++] = '@';
        for (int sh = 28; sh >= 0; sh -= 4) name[n++] = "0123456789ABCDEF"[(pc >> sh) & 0xFu];
    }
    name[n] = 0;
    plat_diag_fault(PLAT_FAULT_HARDFAULT, -1, name, pc, now_ms);
    plat_diag.fault_lr = lr;
    plat_diag.fault_cfsr = cfsr;
    plat_diag.fault_hfsr = hfsr;
    plat_diag.fault_addr = (cfsr & CFSR_BFARVALID) ? bfar : (cfsr & CFSR_MMARVALID) ? mmfar : 0u;
    plat_diag.check = diag_sum(&plat_diag);
}
