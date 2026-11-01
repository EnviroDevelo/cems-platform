/* spi4_bus.h - the CORE PCB's SPI4 (PE12 SCK, PE13 MISO, PE14 MOSI), shared by the W5500 and the
 * I/O node's five field chips (CORE_PCB.md section 14). Each user claims the bus for a whole
 * transaction (chip select low to high) with its SPI mode and clock limit; the claim takes a
 * FreeRTOS mutex and re-programs the SPI only when the mode or the divider changes.
 *   W5500:         mode 0, max_hz 0 = CubeMX's own setting (up to about 30 MHz)
 *   field chips:   mode 0 or 1 (ADS124S08), 4 MHz
 *   expansion:     CS_EXP PD3 (CORE_PCB.md 16), held high until a module driver exists (AD5755-1 planned)
 * Before the scheduler runs, the claim does not block (one task at a time then anyway). */
#ifndef SPI4_BUS_H
#define SPI4_BUS_H
#include <stdint.h>

void spi4_claim(uint8_t mode, uint32_t max_hz);
void spi4_release(void);

#endif
