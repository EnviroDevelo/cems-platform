/* bsp.h - board support interface. One implementation per board:
 *   platform/bsp/bsp_host.c     - simulated board for host tests and the PC sim
 *   port/stm32h5/bsp_stm32h5.c  - STM32H5 on CubeMX-generated HAL handles
 * Nothing above this layer includes a HAL header. */
#ifndef BSP_H
#define BSP_H
#include <stdint.h>
#include <stdbool.h>
#include "plat_io.h"
#include "nvm.h"
#include "nor.h"

/* Calendar time from the hardware RTC. dow_iso: 1 = Monday ... 7 = Sunday
 * (STM32 RTC convention). The scan engine converts to UniLogic's 1 = Sunday. */
typedef struct {
    uint16_t year;     /* full year, e.g. 2026 */
    uint8_t  month;    /* 1..12 */
    uint8_t  day;      /* 1..31 */
    uint8_t  hours, minutes, seconds;
    uint8_t  dow_iso;  /* 1..7, Monday = 1 */
} bsp_rtc_t;

typedef enum {
    BSP_RESET_UNKNOWN = 0,
    BSP_RESET_POWER_ON,
    BSP_RESET_PIN,
    BSP_RESET_SOFTWARE,
    BSP_RESET_IWDG,
    BSP_RESET_WWDG,
    BSP_RESET_BROWNOUT
} bsp_reset_cause_t;

void     bsp_init(void);                 /* after the CubeMX MX_*_Init calls */
uint32_t bsp_ms(void);                   /* free-running ms, wraps at 2^32   */
uint32_t bsp_cycles(void);               /* free-running CPU cycle count, wraps; differences only */
uint32_t bsp_cycles_per_us(void);        /* e.g. 250 at 250 MHz */
bool     bsp_rtc_read(bsp_rtc_t *t);     /* false if the RTC is not set/valid */
bool     bsp_rtc_write(const bsp_rtc_t *t);   /* set the calendar (scan task only, via plat_clock); false on failure */
void     bsp_io_read(plat_io_in_t *in);
void     bsp_io_write(const plat_io_out_t *out);
void     bsp_io_safe_state(void);        /* all outputs to their defined safe state */
void     bsp_wdg_kick(void);
bsp_reset_cause_t bsp_reset_cause(void); /* read once at boot, then cleared */
void     bsp_system_reset(void);

/* The parameter NVM: the FRAM if it answers, otherwise a RAM fallback with
 * persistent = false (the store then reports PSTAT_NVM_VOLATILE). */
nvm_dev_t *bsp_param_nvm(void);

/* The data-log flash (serial NOR): the chip if it answers, otherwise a small RAM
 * stand-in with persistent = false when none is configured (LSTAT_VOLATILE), or
 * NULL when one is configured but does not answer (logging off, alarm). */
nor_dev_t *bsp_log_nor(void);

/* The two program-flash banks for firmware updates (fw_update.h); NULL if not available. */
struct fw_flash;
struct fw_flash *bsp_fw_flash(void);

/* The display's SPI transport (eve.h); NULL if this hardware has no display. */
struct eve_bus;
struct eve_bus *bsp_eve_bus(void);

/* Random bytes for password salts and web session tokens (hardware RNG on the H5). */
void bsp_random(void *buf, uint32_t len);

#endif
