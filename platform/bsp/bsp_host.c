/* bsp_host.c - simulated board. Everything is a plain variable in bsp_host. */
#include <stdlib.h>
#include <stdio.h>
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

bool bsp_rtc_write(const bsp_rtc_t *t) { bsp_host.rtc = *t; bsp_host.rtc_valid = true; bsp_host.rtc_writes++; return true; }

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

uint8_t   bsp_host_nor[256u * 1024u];
nor_sim_t bsp_host_nor_sim;
nor_dev_t *bsp_log_nor(void) {
    static nor_dev_t d;
    static bool done;
    if (!done) { nor_sim_init(&d, &bsp_host_nor_sim, bsp_host_nor, sizeof bsp_host_nor, true, true); done = true; }
    return &d;
}

#include "fw_flash_sim.h"
uint8_t bsp_host_bank[2][256u * 1024u];          /* small banks: enough for the tests */
fw_flash_sim_t bsp_host_fw_sim;
struct fw_flash *bsp_fw_flash(void) {
    static fw_flash_t f;
    static bool done;
    if (!done) { fw_flash_sim_init(&f, &bsp_host_fw_sim, bsp_host_bank[0], bsp_host_bank[1], sizeof bsp_host_bank[0]); done = true; }
    return &f;
}

void bsp_random(void *buf, uint32_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len) { uint8_t *p = buf; for (uint32_t k = 0; k < len; ++k) p[k] = (uint8_t)(k * 37u + 11u); }
    if (f) fclose(f);
}

/* the chip's unique ID: CEMS_UID (24 hex digits) or a fixed stand-in */
struct iob_port *bsp_rs485(void) { return bsp_host.rs485; }
struct w5500_bus *bsp_w5500(void) { return bsp_host.w5500; }
bool bsp_supply_mv(uint32_t *mv) { *mv = bsp_host.supply_mv; return bsp_host.supply_mv != 0; }
bool bsp_hw_straps(uint8_t *rev) { *rev = bsp_host.hw_straps; return bsp_host.straps_fitted; }

void bsp_chip_uid(uint8_t uid[12]) {
    static const uint8_t dflt[12] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb };
    const char *e = getenv("CEMS_UID");
    for (int k = 0; k < 12; ++k) {
        unsigned b;
        uid[k] = e && strlen(e) == 24 && sscanf(e + 2 * k, "%2x", &b) == 1 ? (uint8_t)b : dflt[k];
    }
}
