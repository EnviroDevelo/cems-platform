/* nvm_ram.c - RAM-backed nvm device, with power-cut injection for tests. */
#include <string.h>
#include "nvm.h"

static int ram_read(nvm_dev_t *d, uint32_t addr, void *buf, uint32_t len) {
    nvm_ram_t *r = d->ctx;
    if (addr > r->size || len > r->size - addr) return -1;
    memcpy(buf, r->mem + addr, len);
    return 0;
}

static int ram_write(nvm_dev_t *d, uint32_t addr, const void *buf, uint32_t len) {
    nvm_ram_t *r = d->ctx;
    if (addr > r->size || len > r->size - addr) return -1;
    r->writes++;
    uint32_t n = len;
    if (r->fail_after >= 0 && (long)len > r->fail_after) n = (uint32_t)r->fail_after;
    memcpy(r->mem + addr, buf, n);
    r->bytes_written += n;
    if (r->fail_after >= 0) {
        r->fail_after -= (long)n;
        if (n < len) return -2;                 /* power lost part way */
    }
    return 0;
}

void nvm_ram_init(nvm_dev_t *d, nvm_ram_t *r, uint8_t *mem, uint32_t size, bool persistent) {
    r->mem = mem; r->size = size; r->fail_after = -1; r->writes = 0; r->bytes_written = 0;
    d->read = ram_read; d->write = ram_write; d->size = size; d->persistent = persistent; d->ctx = r; d->parts = persistent ? 1u : 0u;
}
