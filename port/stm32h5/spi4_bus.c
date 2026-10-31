/* spi4_bus.c - see spi4_bus.h */
#include "main.h"
#include <stdbool.h>
#include "spi4_bus.h"

#if defined(HAL_SPI_MODULE_ENABLED)
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#ifndef SPI4_BUS_HANDLE
#define SPI4_BUS_HANDLE hspi4
#endif
extern SPI_HandleTypeDef SPI4_BUS_HANDLE;

static SemaphoreHandle_t mtx;
static StaticSemaphore_t mtx_mem;
static SPI_InitTypeDef cube;             /* CubeMX's settings, taken on the first claim: the W5500's */
static bool have_cube;
static uint32_t cur_mode = 0xFFu, cur_presc = 0xFFFFFFFFu;

static bool sched(void) { return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING; }

/* the smallest divider (2..256) that keeps the clock at or below max_hz */
static uint32_t presc_for(uint32_t max_hz) {
    static const uint32_t p[8] = { SPI_BAUDRATEPRESCALER_2, SPI_BAUDRATEPRESCALER_4, SPI_BAUDRATEPRESCALER_8,
        SPI_BAUDRATEPRESCALER_16, SPI_BAUDRATEPRESCALER_32, SPI_BAUDRATEPRESCALER_64,
        SPI_BAUDRATEPRESCALER_128, SPI_BAUDRATEPRESCALER_256 };
    uint32_t k = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI4);
    for (unsigned i = 0; i < 8; ++i) if ((k >> (i + 1u)) <= max_hz) return p[i];
    return p[7];
}

void spi4_claim(uint8_t mode, uint32_t max_hz) {
    if (!mtx) {
        taskENTER_CRITICAL();
        if (!mtx) mtx = xSemaphoreCreateMutexStatic(&mtx_mem);
        taskEXIT_CRITICAL();
    }
    if (sched()) (void)xSemaphoreTake(mtx, portMAX_DELAY);
    if (!have_cube) { cube = SPI4_BUS_HANDLE.Init; have_cube = true; cur_mode = (cube.CLKPolarity == SPI_POLARITY_HIGH ? 2u : 0u) | (cube.CLKPhase == SPI_PHASE_2EDGE ? 1u : 0u); cur_presc = cube.BaudRatePrescaler; }
    uint32_t presc = max_hz ? presc_for(max_hz) : cube.BaudRatePrescaler;
    if (mode != cur_mode || presc != cur_presc) {
        SPI4_BUS_HANDLE.Init.CLKPolarity = (mode & 2u) ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW;
        SPI4_BUS_HANDLE.Init.CLKPhase = (mode & 1u) ? SPI_PHASE_2EDGE : SPI_PHASE_1EDGE;
        SPI4_BUS_HANDLE.Init.BaudRatePrescaler = presc;
        (void)HAL_SPI_Init(&SPI4_BUS_HANDLE);
        cur_mode = mode; cur_presc = presc;
    }
}

void spi4_release(void) {
    if (mtx && sched()) (void)xSemaphoreGive(mtx);
}
#else
void spi4_claim(uint8_t mode, uint32_t max_hz) { (void)mode; (void)max_hz; }
void spi4_release(void) {}
#endif
