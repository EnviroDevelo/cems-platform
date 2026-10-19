/* nor_spi.c - JEDEC serial NOR flash on SPI for the data logger (W25Q, MX25L,
 * IS25LP, AT25SF and similar, 1-bit SPI).
 *
 * CubeMX: an SPI peripheral in full-duplex master mode, 8-bit, mode 0, software
 * NSS, up to the part's read clock for 03h/13h (typically 50 MHz), plus a GPIO
 * output labelled NOR_CS (idle high). main.h (USER CODE section) may override:
 *   NOR_SPI_HANDLE  hspi2   the HAL handle (may be the FRAM's SPI: the store task owns both)
 *
 * The size comes from the JEDEC ID (9Fh). Parts above 16 MB use the 4-byte-address
 * opcodes (13h read, 12h program, 21h sector erase), so the chip's address mode
 * register is never changed. Commands: 06h WREN, 05h RDSR, 01h WRSR, 03h READ,
 * 02h PP, 20h SE 4 KB, ABh release from power-down, 66h/99h reset.
 *
 * Waiting: page program polls the status register (well under 1 ms for 64 bytes);
 * a sector erase (typ. 45 ms, max. 400 ms) sleeps 1 ms between polls once the
 * scheduler runs. The store task's watchdog allowance (2 s) covers both. */
#include "main.h"
#include "nor.h"

#ifndef NOR_SPI_HANDLE
#define NOR_SPI_HANDLE hspi2
#endif

#if defined(HAL_SPI_MODULE_ENABLED) && defined(NOR_CS_Pin)
#include "FreeRTOS.h"
#include "task.h"

extern SPI_HandleTypeDef NOR_SPI_HANDLE;
#define TMO_MS 100u
#define SR_WIP 0x01u
#define SR_WEL 0x02u
#define SR_BP  0x3Cu          /* block-protect bits (BP0..BP3) */

static bool four_byte;

static inline void cs(bool active) {
    HAL_GPIO_WritePin(NOR_CS_GPIO_Port, NOR_CS_Pin, active ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static int tx(const uint8_t *p, uint32_t n) {
    while (n) {
        uint16_t k = n > 0x8000u ? 0x8000u : (uint16_t)n;
        if (HAL_SPI_Transmit(&NOR_SPI_HANDLE, (uint8_t *)(uintptr_t)p, k, TMO_MS) != HAL_OK) return -1;
        p += k; n -= k;
    }
    return 0;
}

static int rx(uint8_t *p, uint32_t n) {
    while (n) {
        uint16_t k = n > 0x8000u ? 0x8000u : (uint16_t)n;
        if (HAL_SPI_Receive(&NOR_SPI_HANDLE, p, k, TMO_MS) != HAL_OK) return -1;
        p += k; n -= k;
    }
    return 0;
}

static int cmd(uint8_t c) { cs(true); int r = tx(&c, 1); cs(false); return r; }

static int rdsr(uint8_t *sr) {
    uint8_t t[2] = { 0x05u, 0xFFu }, r[2] = { 0, 0xFFu };
    cs(true);
    HAL_StatusTypeDef s = HAL_SPI_TransmitReceive(&NOR_SPI_HANDLE, t, r, 2, TMO_MS);
    cs(false);
    *sr = r[1];
    return s == HAL_OK ? 0 : -1;
}

static int cmd_addr(uint8_t c3, uint8_t c4, uint32_t a) {
    uint8_t h[5]; unsigned n = 0;
    h[n++] = four_byte ? c4 : c3;
    if (four_byte) h[n++] = (uint8_t)(a >> 24);
    h[n++] = (uint8_t)(a >> 16);
    h[n++] = (uint8_t)(a >> 8);
    h[n++] = (uint8_t)a;
    return tx(h, n);
}

static int wait_ready(uint32_t timeout_ms, bool sleep) {
    uint32_t t0 = HAL_GetTick();
    for (;;) {
        uint8_t sr;
        if (rdsr(&sr) != 0) return -1;
        if (!(sr & SR_WIP)) return 0;
        if (HAL_GetTick() - t0 > timeout_ms) return -1;
        if (sleep && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) vTaskDelay(1);
    }
}

static int write_enable(void) {
    uint8_t sr;
    if (cmd(0x06u) != 0 || rdsr(&sr) != 0 || !(sr & SR_WEL)) return -1;
    return 0;
}

static int nor_read(nor_dev_t *d, uint32_t a, void *buf, uint32_t len) {
    if (a > d->size || len > d->size - a) return -1;
    if (!len) return 0;
    cs(true);
    int r = cmd_addr(0x03u, 0x13u, a);
    if (r == 0) r = rx(buf, len);
    cs(false);
    return r;
}

static int nor_prog(nor_dev_t *d, uint32_t a, const void *buf, uint32_t len) {
    if (a > d->size || len > d->size - a || (a % d->page) + len > d->page) return -1;
    if (!len) return 0;
    if (write_enable() != 0) return -1;
    cs(true);
    int r = cmd_addr(0x02u, 0x12u, a);
    if (r == 0) r = tx(buf, len);
    cs(false);
    if (wait_ready(10u, false) != 0) r = -1;
    return r;
}

static int nor_erase(nor_dev_t *d, uint32_t a) {
    if (a >= d->size) return -1;
    a -= a % d->sector;
    if (write_enable() != 0) return -1;
    cs(true);
    int r = cmd_addr(0x20u, 0x21u, a);
    cs(false);
    if (wait_ready(1000u, true) != 0) r = -1;
    return r;
}

bool nor_spi_open(nor_dev_t *d) {
    cs(false);
    (void)cmd(0xABu);                     /* release from deep power-down */
    for (volatile int k = 0; k < 2000; ++k) { }   /* tRES1 ~ 3 us */
    (void)cmd(0x66u); (void)cmd(0x99u);   /* software reset: a known state after a warm reset */
    HAL_Delay(1);
    uint8_t t[4] = { 0x9Fu, 0xFFu, 0xFFu, 0xFFu }, id[4] = { 0 };
    cs(true);
    HAL_StatusTypeDef s = HAL_SPI_TransmitReceive(&NOR_SPI_HANDLE, t, id, 4, TMO_MS);
    cs(false);
    if (s != HAL_OK || id[1] == 0x00u || id[1] == 0xFFu) return false;     /* nothing there */
    if (id[3] < 0x10u || id[3] > 0x21u) return false;                     /* 64 KB .. 1 Gbit */
    uint32_t size = 1u << id[3];
    four_byte = size > (16u * 1024u * 1024u);
    uint8_t sr;
    if (rdsr(&sr) != 0) return false;
    if (sr & SR_BP) {                     /* clear block protection */
        uint8_t w[2] = { 0x01u, 0x00u };
        if (write_enable() != 0) return false;
        cs(true); (void)tx(w, 2); cs(false);
        if (wait_ready(50u, false) != 0 || rdsr(&sr) != 0 || (sr & SR_BP)) return false;
    }
    d->read = nor_read; d->prog = nor_prog; d->erase = nor_erase;
    d->size = size; d->sector = 4096u; d->page = 256u; d->persistent = true; d->ctx = NULL;
    return true;
}

#else
bool nor_spi_open(nor_dev_t *d) { (void)d; return false; }   /* no log flash configured in CubeMX */
#endif
