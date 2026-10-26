/* iob_net.c - see iob_net.h. Wire layout of a discovery message (IOB_DISC_LEN bytes, big-endian):
 *   0  magic "CEMSIOB1"     8  version (1)     9  op        10  seq
 *  12  uid (6 x u16)       24  mac (6)        30  serial (16, NUL padded)
 *  46  fw_version  48  fw_build  50  hw_rev  52  flags
 *  54  ip  58  mask  62  gw  66  master_ip  70  address  71..79 reserved (0) */
#include <string.h>
#include "iob_net.h"
#include "iobus.h"
#include "mb_rtu.h"

#define VERSION 1u

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

size_t iob_disc_encode(const iob_disc_t *m, uint8_t *b) {
    memset(b, 0, IOB_DISC_LEN);
    memcpy(b, IOB_DISC_MAGIC, 8);
    b[8] = VERSION;
    b[9] = m->op;
    put16(b + 10, m->seq);
    for (unsigned k = 0; k < 6; ++k) put16(b + 12 + 2 * k, m->uid[k]);
    memcpy(b + 24, m->mac, 6);
    for (unsigned k = 0; k < 16 && m->serial[k]; ++k) b[30 + k] = (uint8_t)m->serial[k];
    put16(b + 46, m->fw_version);
    put16(b + 48, m->fw_build);
    put16(b + 50, m->hw_rev);
    put16(b + 52, m->flags);
    put32(b + 54, m->ip);
    put32(b + 58, m->mask);
    put32(b + 62, m->gw);
    put32(b + 66, m->master_ip);
    b[70] = m->address;
    return IOB_DISC_LEN;
}

bool iob_disc_decode(const uint8_t *b, size_t len, iob_disc_t *m) {
    if (len != IOB_DISC_LEN || memcmp(b, IOB_DISC_MAGIC, 8) != 0 || b[8] != VERSION) return false;
    if (b[9] < IOB_DISC_FIND || b[9] > IOB_DISC_SET) return false;
    memset(m, 0, sizeof *m);
    m->op = b[9];
    m->seq = get16(b + 10);
    for (unsigned k = 0; k < 6; ++k) m->uid[k] = get16(b + 12 + 2 * k);
    memcpy(m->mac, b + 24, 6);
    for (unsigned k = 0; k < 16; ++k) {
        uint8_t c = b[30 + k];
        if (c < 0x20 || c > 0x7E) break;           /* printable ASCII only, up to the first NUL */
        m->serial[k] = (char)c;
    }
    m->fw_version = get16(b + 46);
    m->fw_build = get16(b + 48);
    m->hw_rev = get16(b + 50);
    m->flags = get16(b + 52);
    m->ip = get32(b + 54);
    m->mask = get32(b + 58);
    m->gw = get32(b + 62);
    m->master_ip = get32(b + 66);
    m->address = b[70];
    return true;
}

static bool host_ok(uint32_t ip, uint32_t mask) {
    uint32_t host = ip & ~mask;
    uint8_t top = (uint8_t)(ip >> 24);
    return top != 0 && top != 127 && top < 224 && host != 0 && host != ~mask;   /* not network, not broadcast */
}

bool iob_net_settings_ok(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t master_ip, uint8_t address) {
    if (mask < 0xFF000000u || mask > 0xFFFFFFFCu || (~mask & (~mask + 1u)) != 0) return false;  /* contiguous /8../30 */
    if (!host_ok(ip, mask) || !host_ok(master_ip, mask) || ip == master_ip) return false;
    if ((ip & mask) != (master_ip & mask)) return false;
    if (gw && (!host_ok(gw, mask) || (gw & mask) != (ip & mask) || gw == ip)) return false;
    return address >= 1 && address <= IOB_ADDR_MAX;
}

bool iob_disc_set_accept(const iob_disc_t *m, const uint16_t uid[6], uint32_t cur_master_ip, uint32_t from_ip) {
    if (m->op != IOB_DISC_SET || memcmp(m->uid, uid, sizeof m->uid) != 0) return false;
    if (!iob_net_settings_ok(m->ip, m->mask, m->gw, m->master_ip, m->address)) return false;
    return cur_master_ip == 0 || from_ip == cur_master_ip;
}

size_t iob_mbap_from_rtu(const uint8_t *rtu, size_t len, uint16_t tid, uint8_t *out) {
    if (len < 4 || len - 2 + 6 > IOB_MBAP_MAX) return 0;
    if (mbr_crc16(rtu, len - 2) != (uint16_t)(rtu[len - 2] | rtu[len - 1] << 8)) return 0;
    size_t n = len - 2;                            /* unit id + PDU */
    put16(out, tid);
    put16(out + 2, 0);
    put16(out + 4, (uint16_t)n);
    memcpy(out + 6, rtu, n);
    return 6 + n;
}

int iob_mbap_complete(const uint8_t *b, size_t len) {
    if (len < 7) return 0;
    uint16_t n = get16(b + 4);
    if (get16(b + 2) != 0 || n < 2 || 6u + n > IOB_MBAP_MAX) return -1;
    return len >= 6u + n ? (int)(6u + n) : 0;
}

size_t iob_rtu_from_mbap(const uint8_t *b, size_t len, uint16_t *tid, uint8_t *out) {
    int c = iob_mbap_complete(b, len);
    if (c <= 0 || (size_t)c != len) return 0;
    size_t n = len - 6;
    memcpy(out, b + 6, n);
    uint16_t crc = mbr_crc16(out, n);
    out[n] = (uint8_t)crc;
    out[n + 1] = (uint8_t)(crc >> 8);
    if (tid) *tid = get16(b);
    return n + 2;
}

void iob_mac_from_uid(const uint16_t uid[6], uint8_t mac[6]) {
    /* fold the 96-bit ID into 40 bits (CRC-16 of it plus its low words), first octet 02: locally administered */
    uint8_t raw[12];
    for (unsigned k = 0; k < 6; ++k) { raw[2 * k] = (uint8_t)(uid[k] >> 8); raw[2 * k + 1] = (uint8_t)uid[k]; }
    uint16_t c = mbr_crc16(raw, 12);
    mac[0] = 0x02;
    mac[1] = (uint8_t)(c >> 8);
    mac[2] = (uint8_t)c;
    mac[3] = (uint8_t)(raw[11] ^ raw[5]);
    mac[4] = (uint8_t)(raw[10] ^ raw[4] ^ raw[9]);
    mac[5] = (uint8_t)(raw[8] ^ raw[3] ^ raw[0]);
}
