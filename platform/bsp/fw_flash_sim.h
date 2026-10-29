/* fw_flash_sim.h - two flash banks in RAM with the H5's rules (erase to 0xFF,
 * 16-byte program units, each unit programmed once after an erase), for host tests
 * and the PC simulation. */
#ifndef FW_FLASH_SIM_H
#define FW_FLASH_SIM_H
#include "fw_update.h"

typedef struct {
    uint8_t *bank[2];          /* physical banks */
    uint32_t size;
    bool     swapped;          /* bank 1 is the running one */
    uint32_t swaps, erases, writes, violations;
    long     fail_after_writes;    /* -1 = never */
    bool     reset_requested;
    bool     trial;            /* the bootloader's trial mark (boot.h) */
    uint32_t trial_marks;      /* calls to trial_mark */
} fw_flash_sim_t;

void fw_flash_sim_init(fw_flash_t *f, fw_flash_sim_t *s, uint8_t *bank0, uint8_t *bank1, uint32_t size);
uint8_t *fw_flash_sim_active(fw_flash_sim_t *s);
uint8_t *fw_flash_sim_free(fw_flash_sim_t *s);

#endif
