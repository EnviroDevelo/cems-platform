/* nvm.h - byte-addressable non-volatile memory (the parameter FRAM).
 * Implementations: platform/bsp/nvm_ram.c (simulation, and the fallback when no
 * FRAM is fitted) and port/stm32h5/nvm_fram_spi.c (SPI FRAM).
 * Only the store task uses the device, so no locking is needed. */
#ifndef NVM_H
#define NVM_H
#include <stdint.h>
#include <stdbool.h>

typedef struct nvm_dev {
    int  (*read)(struct nvm_dev *d, uint32_t addr, void *buf, uint32_t len);        /* 0 = ok */
    int  (*write)(struct nvm_dev *d, uint32_t addr, const void *buf, uint32_t len); /* 0 = ok */
    uint32_t size;        /* bytes */
    bool     persistent;  /* false for the RAM fallback: contents lost at power-off */
    void    *ctx;
} nvm_dev_t;

/* RAM device. fail_after: number of further bytes a write may store before it
 * "loses power" (stops and returns an error); -1 = never. For tests. */
typedef struct {
    uint8_t *mem;
    uint32_t size;
    long     fail_after;
    uint32_t writes, bytes_written;
} nvm_ram_t;

void nvm_ram_init(nvm_dev_t *d, nvm_ram_t *r, uint8_t *mem, uint32_t size, bool persistent);

uint32_t plat_crc32(uint32_t crc, const void *data, uint32_t len);   /* IEEE 802.3, start with 0 */

#endif
