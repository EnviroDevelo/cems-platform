/* rs485_stm32h5.c - the RS-485 port for the I/O bus (iobus.h, D27) on an STM32H5 USART.
 *
 * CubeMX (docs/CUBEMX_SETUP.md 3f): one USART in "Asynchronous" mode with Hardware Flow
 * Control (RS485), so the USART drives the transceiver's DE pin itself; 115200 baud,
 * 8 data bits, no parity, 1 stop bit; its global interrupt enabled at a priority the
 * FreeRTOS API allows (numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY). In main.h
 * (USER CODE section) name its handle:
 *     #define BSP_RS485_HUART huart2
 * Without that define there is no port: bsp_rs485() returns NULL and every enabled
 * I/O board reports "not responding".
 *
 * A request: transmit with interrupts, then receive until the line goes idle (one
 * character time of silence ends a Modbus RTU frame), waiting at most the reply
 * timeout. The task sleeps on a notification meanwhile, so the bus costs almost no
 * CPU. The transceiver's receiver is assumed disabled while transmitting (RE tied to
 * DE), so the request is not received back.
 *
 * NOTE: HAL_UART_TxCpltCallback, HAL_UARTEx_RxEventCallback and HAL_UART_ErrorCallback
 * are global in the HAL. If another UART uses interrupts, dispatch by handle in one
 * shared file (as for the I2C FRAM's callbacks). */
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bsp.h"
#include "iobus.h"

#if defined(HAL_UART_MODULE_ENABLED) && defined(BSP_RS485_HUART)
extern UART_HandleTypeDef BSP_RS485_HUART;
#define H (&BSP_RS485_HUART)

static TaskHandle_t       waiter;
static volatile uint16_t  rx_len;
static volatile int       rx_rc;

static void wake(void) {
    BaseType_t woke = pdFALSE;
    if (waiter) vTaskNotifyGiveFromISR(waiter, &woke);
    portYIELD_FROM_ISR(woke);
}
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *h) { if (h == H) wake(); }
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *h, uint16_t size) {
    if (h == H) { rx_len = size; rx_rc = 0; wake(); }
}
void HAL_UART_ErrorCallback(UART_HandleTypeDef *h) {
    if (h == H) { rx_rc = -1; wake(); }       /* noise, framing or overrun: the frame is lost */
}

static uint16_t xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t timeout_ms) {
    (void)p;
    waiter = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0);
    (void)HAL_UART_AbortReceive(H);           /* the tail of a late reply from the last request */
    __HAL_UART_CLEAR_FLAG(H, UART_CLEAR_OREF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_PEF | UART_CLEAR_IDLEF);

    /* 87 us per character at 115200: allow the frame time plus a margin */
    TickType_t tx_ticks = pdMS_TO_TICKS(3u + (uint32_t)n / 8u);
    if (HAL_UART_Transmit_IT(H, (uint8_t *)(uintptr_t)tx, n) != HAL_OK) { waiter = NULL; return 0; }
    if (ulTaskNotifyTake(pdTRUE, tx_ticks) == 0) { (void)HAL_UART_AbortTransmit(H); waiter = NULL; return 0; }

    rx_len = 0;
    rx_rc = -2;
    if (HAL_UARTEx_ReceiveToIdle_IT(H, rx, cap) != HAL_OK) { waiter = NULL; return 0; }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) == 0) {
        (void)HAL_UART_AbortReceive(H);       /* no reply */
        waiter = NULL;
        return 0;
    }
    waiter = NULL;
    return rx_rc == 0 ? rx_len : 0;
}

static iob_port_t port = { .xfer = xfer };
struct iob_port *bsp_rs485(void) { return &port; }

#else
struct iob_port *bsp_rs485(void) { return NULL; }
#endif
