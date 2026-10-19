/* plat_diag.c - see plat_diag.h */
#include <stddef.h>
#include <string.h>
#include "plat_diag.h"

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
    plat_diag.last_reset_cause = (uint32_t)cause;
    plat_diag.check = diag_sum(&plat_diag);
}

void plat_diag_fault(plat_fault_t f, int32_t task, const char *name, uint32_t arg, uint32_t now_ms) {
    if (plat_diag.magic != DIAG_MAGIC) { memset(&plat_diag, 0, sizeof plat_diag); plat_diag.magic = DIAG_MAGIC; }
    plat_diag.fault = (uint32_t)f;
    plat_diag.fault_task = task;
    plat_diag.fault_ms = now_ms;
    plat_diag.fault_arg = arg;
    memset(plat_diag.fault_name, 0, sizeof plat_diag.fault_name);
    if (name) strncpy(plat_diag.fault_name, name, sizeof plat_diag.fault_name - 1u);
    plat_diag.check = diag_sum(&plat_diag);
}
