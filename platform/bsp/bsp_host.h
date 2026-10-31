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
    uint32_t rtc_writes;
    plat_io_in_t  in;            /* returned by bsp_io_read */
    plat_io_out_t out;           /* last bsp_io_write */
    uint32_t io_writes;
    uint32_t wdg_kicks;
    uint32_t safe_states;
    uint32_t resets;
    bsp_reset_cause_t reset_cause;
    struct iob_port *rs485;      /* returned by bsp_rs485 (a simulated bus, iob_node_sim.h) */
    struct w5500_bus *w5500;     /* returned by bsp_w5500 (a simulated chip, w5500_sim.h) */
    uint32_t supply_mv;          /* returned by bsp_supply_mv (0 = not measured) */
    bool     straps_fitted;      /* bsp_hw_straps returns hw_straps (false = no straps) */
    bool     role_fitted, role_a, role_b;   /* bsp_role_read: the role jumper J7 (plat_role.h) */
    uint8_t  hw_straps;
} bsp_host_t;

extern bsp_host_t bsp_host;
extern uint8_t   bsp_host_fram[128u * 1024u];   /* persists across simulated reboots */
extern nvm_ram_t bsp_host_fram_ram;
extern uint8_t   bsp_host_nor[256u * 1024u];   /* the data-log flash, persists across simulated reboots */
extern nor_sim_t bsp_host_nor_sim;
void bsp_host_reset(void);
void bsp_host_advance_ms(uint32_t ms);   /* moves ms and cycles together */

#endif
