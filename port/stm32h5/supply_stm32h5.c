/* supply_stm32h5.c - the 24 V supply reading (bsp_supply_mv, plat_power.h; CORE_PCB.md 12).
 *
 * CubeMX (docs/CUBEMX_SETUP.md 3i): ADC1, 12-bit, scan mode with two regular ranks, software
 * trigger, single conversion, no DMA: rank 1 = PC0 (the supply-sense divider), rank 2 = VREFINT
 * (to know VDDA). Sampling time at least 47.5 cycles on both (VREFINT needs it). Label PC0
 * SUPPLY_SENSE. main.h (USER CODE section) may override:
 *   SUPPLY_ADC_HANDLE  hadc1        the HAL handle
 *   SUPPLY_DIVIDER_X100 1320        the divider ratio x 100 (13.20 on the core board)
 * Without SUPPLY_SENSE the board does not measure it: bsp_supply_mv() returns false, no
 * power-fail shutdown and no SUPPLY alarm.
 *
 * One reading is two conversions (about 4 us at 32 MHz ADC clock), taken every ms by the ctrl
 * task; the platform averages 16. The reading is about 0.4 V low (after the reverse diode). */
#include "main.h"
#include "bsp.h"

#ifndef SUPPLY_ADC_HANDLE
#define SUPPLY_ADC_HANDLE hadc1
#endif
#ifndef SUPPLY_DIVIDER_X100
#define SUPPLY_DIVIDER_X100 1320u
#endif

#if defined(HAL_ADC_MODULE_ENABLED) && defined(SUPPLY_SENSE_Pin)
extern ADC_HandleTypeDef SUPPLY_ADC_HANDLE;

bool bsp_supply_mv(uint32_t *mv) {
    uint32_t raw, vref;
    if (HAL_ADC_Start(&SUPPLY_ADC_HANDLE) != HAL_OK) return false;
    bool ok = HAL_ADC_PollForConversion(&SUPPLY_ADC_HANDLE, 1) == HAL_OK;
    raw = HAL_ADC_GetValue(&SUPPLY_ADC_HANDLE);
    ok = ok && HAL_ADC_PollForConversion(&SUPPLY_ADC_HANDLE, 1) == HAL_OK;
    vref = HAL_ADC_GetValue(&SUPPLY_ADC_HANDLE);
    (void)HAL_ADC_Stop(&SUPPLY_ADC_HANDLE);
    if (!ok || vref == 0) return false;
    /* VDDA = VREFINT_CAL_VREF x VREFINT_CAL / vref (the factory value, taken at 3.3 V, 12-bit) */
    uint64_t vdda = (uint64_t)VREFINT_CAL_VREF * *VREFINT_CAL_ADDR / vref;
    *mv = (uint32_t)((uint64_t)raw * vdda * SUPPLY_DIVIDER_X100 / (4095u * 100u));
    return true;
}
#else
bool bsp_supply_mv(uint32_t *mv) { (void)mv; return false; }
#endif
