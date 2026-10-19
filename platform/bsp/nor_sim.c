/* nor_sim.c - NOR flash in RAM (see nor.h). */
#include <string.h>
#include "nor.h"

#define SECTOR 4096u
#define PAGE   256u

static int sim_read(nor_dev_t *d, uint32_t a, void *buf, uint32_t len) {
    nor_sim_t *s = d->ctx;
    if (s->dead || a > s->size || len > s->size - a) return -1;
    memcpy(buf, s->mem + a, len);
    return 0;
}

static int sim_prog(nor_dev_t *d, uint32_t a, const void *buf, uint32_t len) {
    nor_sim_t *s = d->ctx;
    if (s->dead || a > s->size || len > s->size - a) return -1;
    if (len && (a / PAGE) != ((a + len - 1u) / PAGE)) s->page_violations++;
    s->progs++;
    const uint8_t *p = buf;
    for (uint32_t i = 0; i < len; ++i) {
        if (s->cut_after_bytes == 0) { s->dead = true; return -1; }   /* power lost part way */
        if (s->cut_after_bytes > 0) s->cut_after_bytes--;
        if (s->prog_fail_after == 0) return -1;            /* the write stops part way */
        if (s->prog_fail_after > 0) s->prog_fail_after--;
        uint8_t v = p[i];
        if (s->stuck_bits && a + i == s->stuck_addr) v |= s->stuck_bits;
        if ((s->mem[a + i] & v) != v) s->page_violations++;  /* would need an erase first */
        s->mem[a + i] &= v;                                 /* NOR: program clears bits only */
        s->bytes_programmed++;
    }
    return 0;
}

static int sim_erase(nor_dev_t *d, uint32_t a) {
    nor_sim_t *s = d->ctx;
    if (s->dead || a >= s->size) return -1;
    uint32_t base = a - a % SECTOR;
    s->erases++;
    if (s->cut_after_erases == 0) { memset(s->mem + base, 0xFF, SECTOR / 3u); s->dead = true; return -1; }
    if (s->cut_after_erases > 0) s->cut_after_erases--;
    if (s->erase_fail_after == 0) {                         /* interrupted erase */
        memset(s->mem + base, 0xFF, SECTOR / 2u);
        return -1;
    }
    if (s->erase_fail_after > 0) s->erase_fail_after--;
    memset(s->mem + base, 0xFF, SECTOR);
    return 0;
}

void nor_sim_init(nor_dev_t *d, nor_sim_t *s, uint8_t *mem, uint32_t size, bool persistent, bool blank) {
    memset(s, 0, sizeof *s);
    s->mem = mem;
    s->size = size;
    s->prog_fail_after = -1;
    s->erase_fail_after = -1;
    s->cut_after_bytes = -1;
    s->cut_after_erases = -1;
    if (blank) memset(mem, 0xFF, size);
    d->read = sim_read;
    d->prog = sim_prog;
    d->erase = sim_erase;
    d->size = size;
    d->sector = SECTOR;
    d->page = PAGE;
    d->persistent = persistent;
    d->ctx = s;
}
