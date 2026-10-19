/* nvm_fram_spi.c - SPI FRAM (MB85RS / FM25 / CY15B command set) for the parameter store.
 *
 * CubeMX: an SPI peripheral in full-duplex master mode, 8-bit, mode 0 (CPOL 0,
 * CPHA 0), software NSS, plus a GPIO output labelled FRAM_CS (idle high).
 * main.h (USER CODE section) may override:
 *   FRAM_SPI_HANDLE  hspi1     the HAL handle
 *   FRAM_SIZE        PLAT_FRAM_SIZE  bytes
 *   FRAM_ADDR_BYTES  3         2 for parts up to 64 KB, 3 above
 *
 * Commands used: WREN 06h, WRDI 04h, RDSR 05h, WRSR 01h, READ 03h, WRITE 02h.
 * FRAM writes at bus speed with no page boundaries or erase, so one command
 * writes any length. Detection toggles the write-enable latch (works for every
 * vendor, unlike RDID). */
#include "main.h"
#include "nvm.h"
#include "plat_config.h"

#ifndef FRAM_SPI_HANDLE
#define FRAM_SPI_HANDLE hspi1
#endif
#ifndef FRAM_SIZE
#define FRAM_SIZE PLAT_FRAM_SIZE
#endif
#ifndef FRAM_ADDR_BYTES
#define FRAM_ADDR_BYTES 3
#endif

#if defined(HAL_SPI_MODULE_ENABLED) && defined(FRAM_CS_Pin)

extern SPI_HandleTypeDef FRAM_SPI_HANDLE;
#define SPI_TIMEOUT_MS 100u
#define CMD_WREN 0x06u
#define CMD_WRDI 0x04u
#define CMD_RDSR 0x05u
#define CMD_WRSR 0x01u
#define CMD_READ 0x03u
#define CMD_WRITE 0x02u
#define SR_WEL   0x02u
#define SR_BP    0x0Cu    /* block protect bits */
#define SR_WPEN  0x80u

static inline void cs(bool active) {
    HAL_GPIO_WritePin(FRAM_CS_GPIO_Port, FRAM_CS_Pin, active ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static int cmd_only(uint8_t c) {
    cs(true);
    HAL_StatusTypeDef r = HAL_SPI_Transmit(&FRAM_SPI_HANDLE, &c, 1, SPI_TIMEOUT_MS);
    cs(false);
    return r == HAL_OK ? 0 : -1;
}

static int read_sr(uint8_t *sr) {
    uint8_t tx[2] = { CMD_RDSR, 0xFF }, rx[2] = { 0, 0 };
    cs(true);
    HAL_StatusTypeDef r = HAL_SPI_TransmitReceive(&FRAM_SPI_HANDLE, tx, rx, 2, SPI_TIMEOUT_MS);
    cs(false);
    *sr = rx[1];
    return r == HAL_OK ? 0 : -1;
}

static int send_addr(uint8_t c, uint32_t a) {
    uint8_t h[4]; unsigned n = 0;
    h[n++] = c;
#if FRAM_ADDR_BYTES == 3
    h[n++] = (uint8_t)(a >> 16);
#endif
    h[n++] = (uint8_t)(a >> 8);
    h[n++] = (uint8_t)a;
    return HAL_SPI_Transmit(&FRAM_SPI_HANDLE, h, (uint16_t)n, SPI_TIMEOUT_MS) == HAL_OK ? 0 : -1;
}

static int fram_read(nvm_dev_t *d, uint32_t addr, void *buf, uint32_t len) {
    if (addr > d->size || len > d->size - addr) return -1;
    if (len == 0) return 0;
    int rc = 0;
    cs(true);
    if (send_addr(CMD_READ, addr) != 0) rc = -1;
    for (uint8_t *p = buf; rc == 0 && len; ) {
        uint16_t n = len > 0x8000u ? 0x8000u : (uint16_t)len;
        if (HAL_SPI_Receive(&FRAM_SPI_HANDLE, p, n, SPI_TIMEOUT_MS) != HAL_OK) rc = -1;
        p += n; len -= n;
    }
    cs(false);
    return rc;
}

static int fram_write(nvm_dev_t *d, uint32_t addr, const void *buf, uint32_t len) {
    if (addr > d->size || len > d->size - addr) return -1;
    if (len == 0) return 0;
    if (cmd_only(CMD_WREN) != 0) return -1;
    int rc = 0;
    cs(true);
    if (send_addr(CMD_WRITE, addr) != 0) rc = -1;
    for (const uint8_t *p = buf; rc == 0 && len; ) {
        uint16_t n = len > 0x8000u ? 0x8000u : (uint16_t)len;
        if (HAL_SPI_Transmit(&FRAM_SPI_HANDLE, (uint8_t *)(uintptr_t)p, n, SPI_TIMEOUT_MS) != HAL_OK) rc = -1;
        p += n; len -= n;
    }
    cs(false);                       /* the write completes on CS rising; the latch clears itself */
    return rc;
}

/* Present if the write-enable latch can be set and cleared. Clears block protection. */
static bool fram_probe(void) {
    uint8_t sr = 0;
    cs(false);
    if (cmd_only(CMD_WREN) != 0 || read_sr(&sr) != 0 || !(sr & SR_WEL)) return false;
    if (cmd_only(CMD_WRDI) != 0 || read_sr(&sr) != 0 || (sr & SR_WEL)) return false;
    if (sr & (SR_BP | SR_WPEN)) {
        uint8_t w[2] = { CMD_WRSR, 0x00 };
        if (cmd_only(CMD_WREN) != 0) return false;
        cs(true); (void)HAL_SPI_Transmit(&FRAM_SPI_HANDLE, w, 2, SPI_TIMEOUT_MS); cs(false);
        if (read_sr(&sr) != 0 || (sr & (SR_BP | SR_WPEN))) return false;
    }
    return true;
}

bool nvm_fram_spi_open(nvm_dev_t *d) {
    if (!fram_probe()) return false;
    d->read = fram_read; d->write = fram_write; d->size = FRAM_SIZE; d->persistent = true; d->ctx = NULL;
    return true;
}

#else
bool nvm_fram_spi_open(nvm_dev_t *d) { (void)d; return false; }    /* no FRAM configured in CubeMX */
#endif
