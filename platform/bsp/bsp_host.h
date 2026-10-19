/* bsp_host.h - controls for the simulated board (host tests and PC sim only). */
#ifndef BSP_HOST_H
#define BSP_HOST_H
#include "bsp.h"

typedef struct {
    uint32_t ms;                 /* simulated uptime */
    uint32_t cycles;             /* simulated CPU cycles */
    uint32_t cycles_per_us;
    bool     rtc_valid;
    bsp_rtc_t rtc;               /* returned as-is by bsp_rtc_read */
    plat_io_in_t  in;            /* returned by bsp_io_read */
    plat_io_out_t out;           /* last bsp_io_write */
    uint32_t io_writes;
    uint32_t wdg_kicks;
    uint32_t safe_states;
    uint32_t resets;
    bsp_reset_cause_t reset_cause;
} bsp_host_t;

extern bsp_host_t bsp_host;
extern uint8_t   bsp_host_fram[128u * 1024u];   /* persists across simulated reboots */
extern nvm_ram_t bsp_host_fram_ram;
void bsp_host_reset(void);
void bsp_host_advance_ms(uint32_t ms);   /* moves ms and cycles together */

#endif
