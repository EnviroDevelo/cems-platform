/* fw_flash_sim.c - see fw_flash_sim.h */
#include <string.h>
#include "fw_flash_sim.h"

#define SECTOR 8192u
#define UNIT   16u

uint8_t *fw_flash_sim_active(fw_flash_sim_t *s) { return s->bank[s->swapped ? 1 : 0]; }
uint8_t *fw_flash_sim_free(fw_flash_sim_t *s)   { return s->bank[s->swapped ? 0 : 1]; }

static int sim_erase(fw_flash_t *f, uint32_t off) {
    fw_flash_sim_t *s = f->ctx;
    if (off >= s->size || off % SECTOR) { s->violations++; return -1; }
    memset(fw_flash_sim_free(s) + off, 0xFF, SECTOR);
    s->erases++;
    return 0;
}

static int sim_write(fw_flash_t *f, uint32_t off, const void *p, uint32_t n) {
    fw_flash_sim_t *s = f->ctx;
    if (off > s->size || n > s->size - off || off % UNIT || n % UNIT) { s->violations++; return -1; }
    uint8_t *b = fw_flash_sim_free(s) + off;
    for (uint32_t u = 0; u < n; u += UNIT) {
        if (s->fail_after_writes == 0) return -1;
        if (s->fail_after_writes > 0) s->fail_after_writes--;
        for (uint32_t k = 0; k < UNIT; ++k) if (b[u + k] != 0xFF) { s->violations++; return -1; }   /* ECC: once only */
        memcpy(b + u, (const uint8_t *)p + u, UNIT);
        s->writes++;
    }
    return 0;
}

static int sim_read(fw_flash_t *f, bool active, uint32_t off, void *p, uint32_t n) {
    fw_flash_sim_t *s = f->ctx;
    if (off > s->size || n > s->size - off) return -1;
    memcpy(p, (active ? fw_flash_sim_active(s) : fw_flash_sim_free(s)) + off, n);
    return 0;
}

static void sim_swap(fw_flash_t *f) {
    fw_flash_sim_t *s = f->ctx;
    s->swapped = !s->swapped;
    s->swaps++;
    s->reset_requested = true;
}

void fw_flash_sim_init(fw_flash_t *f, fw_flash_sim_t *s, uint8_t *bank0, uint8_t *bank1, uint32_t size) {
    memset(s, 0, sizeof *s);
    s->bank[0] = bank0; s->bank[1] = bank1; s->size = size;
    s->fail_after_writes = -1;
    f->bank_size = size; f->sector = SECTOR; f->prog_unit = UNIT;
    f->erase = sim_erase; f->write = sim_write; f->read = sim_read; f->swap_and_reset = sim_swap;
    f->ctx = s;
}
