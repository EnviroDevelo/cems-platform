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
 *
 * Several chips (as in the Smart Filter): at open every address from FRAM_I2C_ADDR
 * to 0x57 is probed and each chip's Device ID read (MB85RC: 3 bytes from reserved
 * address 0xF8; density = byte 1 & 0x0F, size = 1 KB << density: 256V 32 KB,
 * 512T 64 KB, 1MT 128 KB). The chips found are joined in address order into one
 * linear memory, so a 32 KB chip at 0x50 and another at 0x51 read as 64 KB.
 * A part above 64 KB (MB85RC1MT) takes two addresses: address bit 16 goes in the
 * A0 position of the device address, so it sits on an even address and its odd
 * neighbour is part of it. A transfer never crosses a chip or a 64 KB bank.
 *
 * The build's memory map is PLAT_FRAM_SIZE (128 KB by default, the product part).
 * If the chips together are smaller than the map they are refused, rather than
 * written past their end: build with PLAT_FRAM_SIZE=32768u for one 32 KB breakout.
 * Capacity above the map is kept for later use (nvm_fram_i2c_info()). The chips
 * must stay at the same addresses: removing one shifts the ones after it, and the
 * store then finds no valid image (CRC) rather than wrong data.
 * A lone chip at FRAM_I2C_ADDR that does not answer the ID read (a non-Fujitsu
 * part) is used as PLAT_FRAM_SIZE, as before; with ID-less chips alongside others
 * only the ones that report their size are used. */
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
#define ADDR_LAST 0x57u
#define MAX_CHIPS 8u
#define POLL_TIMEOUT_MS 1000u
#define XFER_TIMEOUT_MS 2000u
#define CHUNK 0x4000u          /* split long transfers; well under the HAL's 16-bit count */

extern I2C_HandleTypeDef FRAM_I2C_HANDLE;
static TaskHandle_t waiter;
static volatile int xfer_rc;

typedef struct { uint8_t addr; uint32_t base, size; } chip_t;
static chip_t   chips[MAX_CHIPS];
static unsigned n_chips;
static uint32_t total_size;

/* HAL device address for linear address a, within chip c */
static uint16_t dev_addr(const chip_t *c, uint32_t a) {
    uint16_t d = (uint16_t)(c->addr << 1);
    if ((a - c->base) & 0x10000u) d |= 0x02u;   /* A16 in the A0 select position (MB85RC1MT) */
    return d;
}

static const chip_t *chip_at(uint32_t a) {
    for (unsigned k = 0; k < n_chips; ++k)
        if (a >= chips[k].base && a - chips[k].base < chips[k].size) return &chips[k];
    return NULL;
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

static int xfer(bool wr, const chip_t *c, uint32_t a, uint8_t *p, uint16_t n) {
    uint16_t da = dev_addr(c, a), ma = (uint16_t)(a - c->base);
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
        const chip_t *c = chip_at(addr);
        if (!c) return -1;
        uint32_t n = len > CHUNK ? CHUNK : len;
        uint32_t off = addr - c->base;
        uint32_t to_end = c->size - off;                       /* not past this chip */
        uint32_t to_bank_end = 0x10000u - (off & 0xFFFFu);     /* nor across A16 */
        if (n > to_end) n = to_end;
        if (n > to_bank_end) n = to_bank_end;
        int rc = xfer(wr, c, addr, p, (uint16_t)n);
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

/* Size from the Device ID, 0 = no ID answer (not a Fujitsu MB85RC with ID). */
static uint32_t chip_size(uint8_t a) {
    uint8_t id[3] = { 0 };
    if (HAL_I2C_Mem_Read(&FRAM_I2C_HANDLE, 0xF8u, (uint16_t)(a << 1), I2C_MEMADD_SIZE_8BIT, id, 3, 10) != HAL_OK) return 0;
    if (id[0] != 0x00u || (id[1] & 0xF0u) != 0xA0u) return 0;          /* manufacturer 0x00A = Fujitsu */
    unsigned dens = id[1] & 0x0Fu;
    return dens >= 1u && dens <= 7u ? 1024u << dens : 0u;               /* 2 KB .. 128 KB */
}

void nvm_fram_i2c_info(unsigned *n, uint32_t *bytes) { *n = n_chips; *bytes = total_size; }

bool nvm_fram_i2c_open(nvm_dev_t *d) {
    n_chips = 0; total_size = 0;
    bool lone_unknown = false;
    for (unsigned a = FRAM_I2C_ADDR; a <= ADDR_LAST && n_chips < MAX_CHIPS; ++a) {
        if (HAL_I2C_IsDeviceReady(&FRAM_I2C_HANDLE, (uint16_t)(a << 1), 3, 10) != HAL_OK) continue;
        uint32_t sz = chip_size((uint8_t)a);
        if (sz == 0) { if (a == FRAM_I2C_ADDR) lone_unknown = true; continue; }
        if (sz > 65536u && (a & 1u)) continue;                         /* a 1MT cannot start on an odd address */
        chips[n_chips++] = (chip_t){ .addr = (uint8_t)a, .base = total_size, .size = sz };
        total_size += sz;
        if (sz > 65536u) ++a;                                          /* its A16 half answers here too */
    }
    if (n_chips == 0 && lone_unknown) {                                /* one chip without an ID: as configured */
        chips[n_chips++] = (chip_t){ .addr = FRAM_I2C_ADDR, .base = 0, .size = PLAT_FRAM_SIZE };
        total_size = PLAT_FRAM_SIZE;
    }
    if (n_chips == 0 || total_size < PLAT_FRAM_SIZE) return false;     /* smaller than the map: never wrap */
    d->read = fram_read; d->write = fram_write; d->size = total_size; d->persistent = true; d->ctx = NULL;
    d->parts = (uint8_t)n_chips;
    return true;
}

#else
bool nvm_fram_i2c_open(nvm_dev_t *d) { (void)d; return false; }    /* no I2C FRAM configured */
void nvm_fram_i2c_info(unsigned *n, uint32_t *bytes) { *n = 0; *bytes = 0; }
#endif
