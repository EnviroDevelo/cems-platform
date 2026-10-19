/* nor.h - serial NOR flash for the data logger (design spec "Data logger").
 * NOR rules the logger relies on: erase sets a whole sector to 0xFF; program
 * can only clear bits (new = old AND data) and must stay inside one page.
 *
 * Implementations: platform/bsp/nor_sim.c (RAM, with power-cut injection; also
 * the stand-in when no flash is fitted) and port/stm32h5/nor_spi.c (JEDEC SPI NOR).
 * Only the store task uses the device, so no locking is needed. */
#ifndef NOR_H
#define NOR_H
#include <stdint.h>
#include <stdbool.h>

typedef struct nor_dev {
    int  (*read)(struct nor_dev *d, uint32_t addr, void *buf, uint32_t len);        /* 0 = ok */
    int  (*prog)(struct nor_dev *d, uint32_t addr, const void *buf, uint32_t len);  /* within one page */
    int  (*erase)(struct nor_dev *d, uint32_t addr);   /* the sector holding addr; may take ~0.4 s */
    uint32_t size;         /* bytes */
    uint32_t sector;       /* erase unit, 4096 */
    uint32_t page;         /* program unit, 256 */
    bool     persistent;   /* false for the RAM stand-in */
    void    *ctx;
} nor_dev_t;

/* Simulated NOR in RAM. For tests:
 *   prog_fail_after   further bytes that may be programmed before "power is lost"
 *                     (the rest of that call is not programmed, error returned); -1 = never
 *   erase_fail_after  further erases that succeed; the next one leaves the sector half
 *                     erased and returns an error; -1 = never
 *   cut_after_bytes / cut_after_erases
 *                     power cut: after that many more programmed bytes (or at that erase,
 *                     which is left half done) the device is dead: every call fails until
 *                     the test "powers it up" again (dead = false); -1 = never
 *   stuck_bits        if non-zero, every programmed byte at stuck_addr keeps these bits set
 *                     (a worn cell that no longer programs) */
typedef struct {
    uint8_t *mem;
    uint32_t size;
    long     prog_fail_after, erase_fail_after;
    long     cut_after_bytes, cut_after_erases;
    bool     dead;
    uint32_t stuck_addr;
    uint8_t  stuck_bits;
    uint32_t progs, erases, bytes_programmed;
    uint32_t page_violations;     /* program calls that crossed a page or set bits (driver bugs) */
} nor_sim_t;

void nor_sim_init(nor_dev_t *d, nor_sim_t *s, uint8_t *mem, uint32_t size, bool persistent, bool blank);

#endif
