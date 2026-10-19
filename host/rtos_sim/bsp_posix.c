/* bsp_posix.c - BSP for the FreeRTOS POSIX run: real time from the FreeRTOS tick,
 * simulated I/O, and a reset that ends the process with a report. */
#include <stdio.h>
#include <stdlib.h>
#include "FreeRTOS.h"
#include "task.h"
#include "bsp.h"
#include "plat_diag.h"

volatile uint32_t sim_kicks, sim_di;
plat_io_out_t sim_out;

void     bsp_init(void) { }
uint32_t bsp_ms(void) { return (uint32_t)xTaskGetTickCount(); }   /* 1 kHz tick */
uint32_t bsp_cycles(void) { return 0; }
uint32_t bsp_cycles_per_us(void) { return 1; }
bool bsp_rtc_read(bsp_rtc_t *t) { *t = (bsp_rtc_t){ 2026, 10, 19, 12, 0, 0, 1 }; return true; }
void bsp_io_read(plat_io_in_t *in) { in->di = sim_di; }
void bsp_io_write(const plat_io_out_t *out) { sim_out = *out; }
void bsp_io_safe_state(void) { sim_out = (plat_io_out_t){ 0 }; }
void bsp_wdg_kick(void) { sim_kicks++; }
bsp_reset_cause_t bsp_reset_cause(void) { return BSP_RESET_POWER_ON; }
void bsp_system_reset(void) {
    printf("RESET requested at %u ms: fault=%u task=%d name=%s arg=%u, outputs=0x%x, kicks=%u\n",
           (unsigned)bsp_ms(), (unsigned)plat_diag.fault, (int)plat_diag.fault_task, plat_diag.fault_name,
           (unsigned)plat_diag.fault_arg, (unsigned)sim_out.do_bits, (unsigned)sim_kicks);
    fflush(stdout);
    exit(plat_diag.fault == PLAT_FAULT_TASK_SILENT ? 42 : 1);
}

static uint8_t   fram[128u * 1024u];
static nvm_ram_t fram_ram;
static nvm_dev_t fram_dev;
nvm_dev_t *bsp_param_nvm(void) { nvm_ram_init(&fram_dev, &fram_ram, fram, sizeof fram, true); return &fram_dev; }
nor_dev_t *bsp_log_nor(void) {
    static uint8_t mem[64u * 1024u]; static nor_sim_t s; static nor_dev_t d; static bool done;
    if (!done) { nor_sim_init(&d, &s, mem, sizeof mem, true, true); done = true; }
    return &d;
}
#include "fw_flash_sim.h"
struct fw_flash *bsp_fw_flash(void) {
    static uint8_t b0[64u * 1024u], b1[64u * 1024u]; static fw_flash_sim_t s; static fw_flash_t f; static bool done;
    if (!done) { fw_flash_sim_init(&f, &s, b0, b1, sizeof b0); done = true; }
    return &f;
}
void bsp_random(void *buf, uint32_t len) { FILE *f = fopen("/dev/urandom", "rb"); if (!f || fread(buf, 1, len, f) != len) abort(); fclose(f); }
