/* eve_bus_stm32h5.c - SPI transport for the EVE display (eve.h) on the STM32H5.
 *
 * CubeMX: an SPI peripheral in full-duplex master mode, 8-bit, mode 0, software
 * NSS, plus two GPIO outputs labelled EVE_CS (idle high) and EVE_PD (the BT817's
 * PD_N, idle high = running). main.h (USER CODE section) may override:
 *   EVE_SPI_HANDLE  hspi3                         the HAL handle
 *   EVE_SPI_SLOW    SPI_BAUDRATEPRESCALER_32      <= 11 MHz while the BT817 starts
 *   EVE_SPI_FAST    SPI_BAUDRATEPRESCALER_8       <= 30 MHz afterwards
 * The prescalers depend on the SPI kernel clock: pick them so the two limits hold
 * **(check against the clock tree)**. Without EVE_CS there is no display and the
 * UI task idles. Single-channel SPI for now; QSPI is a later speed-up (spec). */
#include "main.h"
#include "eve.h"

#ifndef EVE_SPI_HANDLE
#define EVE_SPI_HANDLE hspi3
#endif
#ifndef EVE_SPI_SLOW
#define EVE_SPI_SLOW SPI_BAUDRATEPRESCALER_32
#endif
#ifndef EVE_SPI_FAST
#define EVE_SPI_FAST SPI_BAUDRATEPRESCALER_8
#endif

#if defined(HAL_SPI_MODULE_ENABLED) && defined(EVE_CS_Pin)
#include "FreeRTOS.h"
#include "task.h"

extern SPI_HandleTypeDef EVE_SPI_HANDLE;
#define TMO 100u

static void sel(eve_bus_t *b, bool on) { (void)b; HAL_GPIO_WritePin(EVE_CS_GPIO_Port, EVE_CS_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET); }

static int wr(eve_bus_t *b, const uint8_t *p, uint32_t n) {
    (void)b;
    while (n) {
        uint16_t k = n > 0x8000u ? 0x8000u : (uint16_t)n;
        if (HAL_SPI_Transmit(&EVE_SPI_HANDLE, (uint8_t *)(uintptr_t)p, k, TMO) != HAL_OK) return -1;
        p += k; n -= k;
    }
    return 0;
}

static int rd(eve_bus_t *b, uint8_t *p, uint32_t n) {
    (void)b;
    while (n) {
        uint16_t k = n > 0x8000u ? 0x8000u : (uint16_t)n;
        if (HAL_SPI_Receive(&EVE_SPI_HANDLE, p, k, TMO) != HAL_OK) return -1;
        p += k; n -= k;
    }
    return 0;
}

static void pwr(eve_bus_t *b, bool on) {
    (void)b;
#if defined(EVE_PD_Pin)
    HAL_GPIO_WritePin(EVE_PD_GPIO_Port, EVE_PD_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
    (void)on;
#endif
}

static void fast(eve_bus_t *b, bool on) {
    (void)b;
    SPI_HandleTypeDef *h = &EVE_SPI_HANDLE;
    __HAL_SPI_DISABLE(h);
    h->Init.BaudRatePrescaler = on ? EVE_SPI_FAST : EVE_SPI_SLOW;
    MODIFY_REG(h->Instance->CFG1, SPI_CFG1_MBR, h->Init.BaudRatePrescaler);
}

static void delay(uint32_t ms) {
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
    else HAL_Delay(ms);
}

eve_bus_t *eve_bus_stm32h5(void) {
    static eve_bus_t b = { .select = sel, .write = wr, .read = rd, .power = pwr, .fast = fast, .delay_ms = delay };
    return &b;
}

#else
eve_bus_t *eve_bus_stm32h5(void) { return NULL; }     /* no display configured in CubeMX */
#endif
