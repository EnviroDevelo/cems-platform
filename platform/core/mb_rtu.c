/* mb_rtu.c - see mb_rtu.h */
#include <string.h>
#include "mb_rtu.h"

uint16_t mbr_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned b = 0; b < 8; ++b) c = (c & 1u) ? (uint16_t)((c >> 1) ^ 0xA001u) : (uint16_t)(c >> 1);
    }
    return c;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }

static size_t finish(uint8_t *f, size_t n) {
    uint16_t c = mbr_crc16(f, n);
    f[n] = (uint8_t)c;            /* low byte first */
    f[n + 1] = (uint8_t)(c >> 8);
    return n + 2;
}

static int addr_ok(uint8_t a) { return a >= 1 && a <= 247; }

size_t mbr_req_read(uint8_t *f, uint8_t addr, uint16_t start, uint16_t n) {
    if (!addr_ok(addr) || n == 0 || n > 125) return 0;
    f[0] = addr; f[1] = 3; put16(f + 2, start); put16(f + 4, n);
    return finish(f, 6);
}

size_t mbr_req_write1(uint8_t *f, uint8_t addr, uint16_t reg, uint16_t v) {
    if (!addr_ok(addr)) return 0;
    f[0] = addr; f[1] = 6; put16(f + 2, reg); put16(f + 4, v);
    return finish(f, 6);
}

size_t mbr_req_write(uint8_t *f, uint8_t addr, uint16_t start, const uint16_t *v, uint16_t n) {
    if (!addr_ok(addr) || n == 0 || n > MBR_REGS_MAX) return 0;
    f[0] = addr; f[1] = 16; put16(f + 2, start); put16(f + 4, n); f[6] = (uint8_t)(2u * n);
    for (uint16_t i = 0; i < n; ++i) put16(f + 7 + 2u * i, v[i]);
    return finish(f, 7u + 2u * n);
}

size_t mbr_req_rw(uint8_t *f, uint8_t addr, uint16_t rd_start, uint16_t rd_n,
                  uint16_t wr_start, const uint16_t *wv, uint16_t wr_n) {
    if (!addr_ok(addr) || rd_n == 0 || rd_n > 125 || wr_n == 0 || wr_n > 121) return 0;
    f[0] = addr; f[1] = 23;
    put16(f + 2, rd_start); put16(f + 4, rd_n);
    put16(f + 6, wr_start); put16(f + 8, wr_n); f[10] = (uint8_t)(2u * wr_n);
    for (uint16_t i = 0; i < wr_n; ++i) put16(f + 11 + 2u * i, wv[i]);
    return finish(f, 11u + 2u * wr_n);
}

int mbr_check_reply(const uint8_t *req, size_t req_len, const uint8_t *rx, size_t rx_len,
                    uint16_t *regs, uint16_t n_regs, uint8_t *exc) {
    if (rx_len == 0) return MBR_NO_REPLY;
    if (rx_len < 5 || req_len < 8) return MBR_SHORT;
    if (mbr_crc16(rx, rx_len - 2) != (uint16_t)(rx[rx_len - 2] | (uint16_t)rx[rx_len - 1] << 8)) return MBR_CRC;
    if (rx[0] != req[0]) return MBR_WRONG_ADDR;
    uint8_t fc = req[1];
    if (rx[1] == (uint8_t)(fc | 0x80u)) {
        if (exc) *exc = rx[2];
        return MBR_EXCEPTION;
    }
    if (rx[1] != fc) return MBR_WRONG_FUNC;
    switch (fc) {
    case 3: case 23: {
        uint16_t want = get16(req + 4);
        if (want != n_regs) return MBR_BAD_FORMAT;
        if (rx[2] != 2u * want) return MBR_BAD_FORMAT;
        if (rx_len != 5u + 2u * want) return MBR_SHORT;
        for (uint16_t i = 0; i < want; ++i) regs[i] = get16(rx + 3 + 2u * i);
        return MBR_OK;
    }
    case 6: case 16:
        if (rx_len != 8) return MBR_SHORT;
        return memcmp(rx + 2, req + 2, 4) == 0 ? MBR_OK : MBR_BAD_FORMAT;   /* echo of address and value/count */
    default:
        return MBR_WRONG_FUNC;
    }
}

const char *mbr_result_name(int r) {
    static const char *const n[] = { "ok", "no reply", "short reply", "CRC error", "wrong address",
                                     "wrong function", "bad format", "exception" };
    return (r >= 0 && r <= MBR_EXCEPTION) ? n[r] : "?";
}
