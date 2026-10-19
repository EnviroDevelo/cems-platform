/* bsp_host.c - simulated board. Everything is a plain variable in bsp_host. */
#include <string.h>
#include "bsp_host.h"

bsp_host_t bsp_host;

void bsp_host_reset(void) {
    memset(&bsp_host, 0, sizeof bsp_host);
    bsp_host.cycles_per_us = 250;
    bsp_host.rtc_valid = true;
    bsp_host.rtc = (bsp_rtc_t){ .year = 2026, .month = 10, .day = 19, .hours = 12,
                                .minutes = 0, .seconds = 0, .dow_iso = 1 };   /* Monday */
    bsp_host.reset_cause = BSP_RESET_POWER_ON;
}

void bsp_host_advance_ms(uint32_t ms) {
    bsp_host.ms += ms;
    bsp_host.cycles += ms * 1000u * bsp_host.cycles_per_us;
}

void     bsp_init(void) { }
uint32_t bsp_ms(void) { return bsp_host.ms; }
uint32_t bsp_cycles(void) { return bsp_host.cycles; }
uint32_t bsp_cycles_per_us(void) { return bsp_host.cycles_per_us; }

bool bsp_rtc_read(bsp_rtc_t *t) {
    if (!bsp_host.rtc_valid) return false;
    *t = bsp_host.rtc;
    return true;
}

void bsp_io_read(plat_io_in_t *in) { *in = bsp_host.in; }
void bsp_io_write(const plat_io_out_t *out) { bsp_host.out = *out; bsp_host.io_writes++; }
void bsp_io_safe_state(void) { memset(&bsp_host.out, 0, sizeof bsp_host.out); bsp_host.safe_states++; }
void bsp_wdg_kick(void) { bsp_host.wdg_kicks++; }
bsp_reset_cause_t bsp_reset_cause(void) { return bsp_host.reset_cause; }
void bsp_system_reset(void) { bsp_host.resets++; }

/* Simulated 128 KB FRAM that survives "reboots" within one process. */
uint8_t   bsp_host_fram[128u * 1024u];
nvm_ram_t bsp_host_fram_ram;
static nvm_dev_t fram_dev;
nvm_dev_t *bsp_param_nvm(void) {
    nvm_ram_init(&fram_dev, &bsp_host_fram_ram, bsp_host_fram, sizeof bsp_host_fram, true);
    return &fram_dev;
}
