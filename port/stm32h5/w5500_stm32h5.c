/* w5500_stm32h5.c - the I/O port's W5500 (w5500.h, HARDWARE.md H5) on an STM32H5 SPI.
 *
 * CubeMX (docs/CUBEMX_SETUP.md 3h): an SPI of its own (product board: SPI4, PE12 SCK, PE13 MISO,
 * PE14 MOSI), full-duplex master, 8-bit, mode 0, software NSS, at most 33 MHz; GPIO outputs
 * labelled W5500_CS (PE11, idle high) and W5500_RST (PE10, idle high = running), and an input
 * labelled W5500_INT (PE9, pull-up; for a later interrupt-driven wait, not used yet). main.h
 * (USER CODE section) may override:
 *   W5500_SPI_HANDLE  hspi4                       the HAL handle
 * Without W5500_CS there is no I/O port: bsp_w5500() returns NULL and, with iob_link set to the
 * I/O port, every enabled I/O board reports "not responding".
 *
 * Transfers are short (a 3-byte header and at most one Modbus TCP frame), so they are polled;
 * the waits between them sleep the iobus task for a tick. */
#include "main.h"
#include "bsp.h"
#include "w5500.h"

#ifndef W5500_SPI_HANDLE
#define W5500_SPI_HANDLE hspi4
#endif

#if defined(HAL_SPI_MODULE_ENABLED) && defined(W5500_CS_Pin)
#include "FreeRTOS.h"
#include "task.h"

extern SPI_HandleTypeDef W5500_SPI_HANDLE;
#define TMO 10u

static void cs(w5500_bus_t *b, bool on) { (void)b; HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET); }

static void xfer(w5500_bus_t *b, const uint8_t *tx, uint8_t *rx, uint16_t n) {
    (void)b;
    if (tx && rx) (void)HAL_SPI_TransmitReceive(&W5500_SPI_HANDLE, (uint8_t *)(uintptr_t)tx, rx, n, TMO);
    else if (tx)  (void)HAL_SPI_Transmit(&W5500_SPI_HANDLE, (uint8_t *)(uintptr_t)tx, n, TMO);
    else if (rx)  (void)HAL_SPI_Receive(&W5500_SPI_HANDLE, rx, n, TMO);
}

static void rst(w5500_bus_t *b, bool on) {
    (void)b;
#if defined(W5500_RST_Pin)
    HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
    (void)on;
#endif
}

static void wait(w5500_bus_t *b, uint32_t ms) {
    (void)b;
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    else HAL_Delay(ms);
}

static w5500_bus_t bus = { .cs = cs, .xfer = xfer, .reset = rst, .wait = wait, .ms = bsp_ms };
struct w5500_bus *bsp_w5500(void) { return &bus; }
#else
struct w5500_bus *bsp_w5500(void) { return NULL; }
#endif
