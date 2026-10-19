/* nvm_fram_i2c.c - I2C FRAM (Fujitsu MB85RC series) for the parameter store.
 * Development part: Adafruit 1895 breakout = MB85RC256V, 32 KB, address 0x50
 * (A0..A2 low), WP pulled down on the board (writes enabled), up to 1 MHz.
 *
 * CubeMX: an I2C peripheral in I2C mode, Fast mode Plus (1 MHz) or Fast mode
 * (400 kHz); for the DMA transfer mode, a GPDMA channel each for I2Cx_TX and
 * I2Cx_RX linked to the handle, and the I2C event + error interrupts enabled
 * at priority >= 5 (FreeRTOS-safe). In main.h (USER CODE section):
 *   #define FRAM_I2C_HANDLE   hi2c1     required: selects this driver
 *   #define FRAM_I2C_ADDR     0x50      7-bit address (0x50 | A2A1A0)
 *   #define FRAM_I2C_MODE     2         0 = polling, 1 = interrupt, 2 = DMA (default)
 *
 * Why not plain polling: a 12.5 KB save takes ~110 ms at 1 MHz (~280 ms at
 * 400 kHz). Polling would spin the store task (priority 10) for that long and
 * starve Modbus, HTTP and the UI; interrupt mode costs one IRQ per byte. DMA
 * moves the data with no CPU time and the store task sleeps until it is done.
 * Before the scheduler starts (the boot load) every mode uses polling.
 *
 * Writes need no page handling or erase: one memory-write of any length.
 * Parts above 64 KB put address bit 16 in the device address (MB85RC1MT):
 * handled by FRAM_SIZE > 65536. */
#include "main.h"
#include "nvm.h"
#include "plat_config.h"

#if defined(HAL_I2C_MODULE_ENABLED) && defined(FRAM_I2C_HANDLE)
#include "FreeRTOS.h"
#include "task.h"

#ifndef FRAM_I2C_ADDR
#define FRAM_I2C_ADDR 0x50u
#endif
#ifndef FRAM_I2C_MODE
#define FRAM_I2C_MODE 2
#endif
#ifndef FRAM_SIZE
#define FRAM_SIZE PLAT_FRAM_SIZE
#endif
#define POLL_TIMEOUT_MS 1000u
#define XFER_TIMEOUT_MS 2000u
#define CHUNK 0x4000u          /* split long transfers; well under the HAL's 16-bit count */

extern I2C_HandleTypeDef FRAM_I2C_HANDLE;
static TaskHandle_t waiter;
static volatile int xfer_rc;

static uint16_t dev_addr(uint32_t a) {
    uint16_t d = (uint16_t)(FRAM_I2C_ADDR << 1);
#if FRAM_SIZE > 65536u
    if (a & 0x10000u) d |= 0x02u;          /* A16 in the A0 select position on 128 KB parts:
                                              confirm against the chosen part's datasheet */
#else
    (void)a;
#endif
    return d;
}

static bool rtos_running(void) {
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

/* completion from the HAL callbacks (ISR context) */
static void done_from_isr(int rc) {
    xfer_rc = rc;
    BaseType_t woke = pdFALSE;
    if (waiter) vTaskNotifyGiveFromISR(waiter, &woke);
    portYIELD_FROM_ISR(woke);
}
void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *h) { if (h == &FRAM_I2C_HANDLE) done_from_isr(0); }
void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *h) { if (h == &FRAM_I2C_HANDLE) done_from_isr(0); }
void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *h)     { if (h == &FRAM_I2C_HANDLE) done_from_isr(-1); }
/* NOTE: these three callbacks are global in the HAL. If another I2C device is
 * added, move them to one shared file that dispatches by handle. */

static int xfer(bool wr, uint32_t a, uint8_t *p, uint16_t n) {
    uint16_t da = dev_addr(a), ma = (uint16_t)a;
    if (FRAM_I2C_MODE == 0 || !rtos_running()) {
        HAL_StatusTypeDef r = wr
            ? HAL_I2C_Mem_Write(&FRAM_I2C_HANDLE, da, ma, I2C_MEMADD_SIZE_16BIT, p, n, POLL_TIMEOUT_MS)
            : HAL_I2C_Mem_Read(&FRAM_I2C_HANDLE, da, ma, I2C_MEMADD_SIZE_16BIT, p, n, POLL_TIMEOUT_MS);
        return r == HAL_OK ? 0 : -1;
    }
    waiter = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0);                 /* clear any stale notification */
    HAL_StatusTypeDef r;
#if FRAM_I2C_MODE == 2
    r = wr ? HAL_I2C_Mem_Write_DMA(&FRAM_I2C_HANDLE, da, ma, I2C_MEMADD_SIZE_16BIT, p, n)
           : HAL_I2C_Mem_Read_DMA(&FRAM_I2C_HANDLE, da, ma, I2C_MEMADD_SIZE_16BIT, p, n);
#else
    r = wr ? HAL_I2C_Mem_Write_IT(&FRAM_I2C_HANDLE, da, ma, I2C_MEMADD_SIZE_16BIT, p, n)
           : HAL_I2C_Mem_Read_IT(&FRAM_I2C_HANDLE, da, ma, I2C_MEMADD_SIZE_16BIT, p, n);
#endif
    if (r != HAL_OK) { waiter = NULL; return -1; }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(XFER_TIMEOUT_MS)) == 0) {
        (void)HAL_I2C_Master_Abort_IT(&FRAM_I2C_HANDLE, da);   /* bus stuck: give up this transfer */
        waiter = NULL;
        return -2;
    }
    waiter = NULL;
    return xfer_rc;
}

static int fram_rw(nvm_dev_t *d, bool wr, uint32_t addr, uint8_t *p, uint32_t len) {
    if (addr > d->size || len > d->size - addr) return -1;
    while (len) {
        uint32_t n = len > CHUNK ? CHUNK : len;
#if FRAM_SIZE > 65536u
        uint32_t to_bank_end = 0x10000u - (addr & 0xFFFFu);  /* a transfer must not cross A16 */
        if (n > to_bank_end) n = to_bank_end;
#endif
        int rc = xfer(wr, addr, p, (uint16_t)n);
        if (rc != 0) return rc;
        addr += n; p += n; len -= n;
    }
    return 0;
}

static int fram_read(nvm_dev_t *d, uint32_t addr, void *buf, uint32_t len) {
    return fram_rw(d, false, addr, buf, len);
}
static int fram_write(nvm_dev_t *d, uint32_t addr, const void *buf, uint32_t len) {
    return fram_rw(d, true, addr, (uint8_t *)(uintptr_t)buf, len);   /* the HAL API is not const */
}

bool nvm_fram_i2c_open(nvm_dev_t *d) {
    if (HAL_I2C_IsDeviceReady(&FRAM_I2C_HANDLE, (uint16_t)(FRAM_I2C_ADDR << 1), 3, 10) != HAL_OK) return false;
    d->read = fram_read; d->write = fram_write; d->size = FRAM_SIZE; d->persistent = true; d->ctx = NULL;
    return true;
}

#else
bool nvm_fram_i2c_open(nvm_dev_t *d) { (void)d; return false; }    /* no I2C FRAM configured */
#endif
