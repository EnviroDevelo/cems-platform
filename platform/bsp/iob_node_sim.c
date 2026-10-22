/* iob_node_sim.c - see iob_node_sim.h */
#include <string.h>
#include "iob_node_sim.h"
#include "mb_rtu.h"
#include "bsp.h"

#define REG_SPACE 80u

void iob_node_init(iob_node_t *n, uint8_t address) {
    memset(n, 0, sizeof *n);
    n->address = address;
    n->powered = true;
    n->magic = IOB_MAGIC; n->map_version = IOB_MAP_VERSION;
    n->n_out_a = 8; n->n_out_b = 8; n->n_relays = 2; n->n_inputs = 8;
    n->fw_version = 0x0100; n->hw_rev = 1;
    for (unsigned i = 0; i < 6; ++i) n->uid[i] = (uint16_t)(0x1000u * address + i);
    n->field_a = n->field_b = true;
    iob_node_restart(n, false, 0);
    n->resets = 0;
}

void iob_node_restart(iob_node_t *n, bool wdg, uint32_t now) {
    n->out_cmd = n->relay_cmd = n->seq = n->echo = 0;
    n->hold_ds = 3000; n->filter_ms = 8; n->wb_en = 0; n->ol_en = 0xFF;
    n->config_needed = true;
    n->timeout_latched = false;
    n->wdg_latched = wdg;
    n->gate = false;                   /* closed until the first valid output write */
    n->last_write_ms = now;
    n->resets++;
}

void iob_node_tick(iob_node_t *n, uint32_t now) {
    if (n->gate && (uint32_t)(now - n->last_write_ms) >= (uint32_t)n->hold_ds * 100u) {
        n->gate = false;
        n->timeout_latched = true;
    }
}

uint16_t iob_node_reg(const iob_node_t *n, uint16_t r) {
    uint16_t st = 0;
    switch (r) {
    case IOB_R_OUT_CMD: return n->out_cmd;
    case IOB_R_RELAY_CMD: return n->relay_cmd;
    case IOB_R_CMD_SEQ: return n->seq;
    case IOB_R_STATUS:
        if (n->gate) st |= IOB_ST_OUT_ENABLED;
        if (n->timeout_latched) st |= IOB_ST_TIMEOUT;
        if (n->field_a) st |= IOB_ST_FIELD_A_OK;
        if (n->field_b) st |= IOB_ST_FIELD_B_OK;
        if (n->wdg_latched) st |= IOB_ST_WDG_RESET;
        if (n->config_needed) st |= IOB_ST_CONFIG_NEEDED;
        if (n->in_chip_fault) st |= IOB_ST_IN_CHIP;
        if (n->out_a_chip_fault) st |= IOB_ST_OUT_A_CHIP;
        return st;
    case IOB_R_INPUTS: return n->field_a ? n->inputs : 0;
    case IOB_R_OUT_APPLIED: {
        if (!n->gate) return 0;
        uint16_t v = n->out_cmd;
        if (n->b_fault & IOB_BF_WDG) v &= 0x00FFu;
        return v;
    }
    case IOB_R_RELAY_APPLIED: return n->gate ? (uint16_t)(n->relay_cmd & 3u) : 0;
    case IOB_R_A_OPEN: return (uint16_t)(n->a_open & n->ol_en);
    case IOB_R_A_OVERLOAD: return n->a_overload;
    case IOB_R_A_OVERTEMP: return n->a_overtemp;
    case IOB_R_B_FAULT: return n->b_fault;
    case IOB_R_WIREBREAK: return (uint16_t)(n->wirebreak & n->wb_en);
    case IOB_R_CMD_ECHO: return n->echo;
    case IOB_R_RX_ERRORS: return n->rx_errors;
    case IOB_R_RESETS: return n->resets;
    case IOB_R_HOLD_DS: return n->hold_ds;
    case IOB_R_FILTER_MS: return n->filter_ms;
    case IOB_R_WB_ENABLE: return n->wb_en;
    case IOB_R_OL_ENABLE: return n->ol_en;
    case IOB_R_FAULT_CLEAR: return 0;
    case IOB_R_MAGIC: return n->magic;
    case IOB_R_MAP_VERSION: return n->map_version;
    case IOB_R_HW_REV: return n->hw_rev;
    case IOB_R_FW_VERSION: return n->fw_version;
    case IOB_R_FW_BUILD: return 1;
    case IOB_R_N_OUT_A: return n->n_out_a;
    case IOB_R_N_OUT_B: return n->n_out_b;
    case IOB_R_N_RELAYS: return n->n_relays;
    case IOB_R_N_INPUTS: return n->n_inputs;
    default:
        if (r >= IOB_R_UID && r < IOB_R_UID + 6) return n->uid[r - IOB_R_UID];
        return 0;                         /* reserved */
    }
}

static bool writable(uint16_t r) { return r <= IOB_R_CMD_SEQ || (r >= IOB_R_HOLD_DS && r <= IOB_R_FAULT_CLEAR); }

static bool value_ok(uint16_t r, uint16_t v) {
    switch (r) {
    case IOB_R_RELAY_CMD: return v <= 3;
    case IOB_R_HOLD_DS: return v >= 1 && v <= 6000;
    case IOB_R_FILTER_MS: return v <= 100;
    case IOB_R_WB_ENABLE: case IOB_R_OL_ENABLE: return v <= 255;
    case IOB_R_FAULT_CLEAR: return v <= 1;
    default: return true;
    }
}

/* check a whole write first (exception code, 0 = fine), then apply it */
static uint8_t write_check(uint16_t start, uint16_t cnt, const uint8_t *data) {
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t r = (uint16_t)(start + i), v = (uint16_t)(data[2u * i] << 8 | data[2u * i + 1]);
        if (!writable(r)) return 2;
        if (!value_ok(r, v)) return 3;
    }
    return 0;
}

static void write_apply(iob_node_t *n, uint32_t now, uint16_t start, uint16_t cnt, const uint8_t *data) {
    bool w0 = false;
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t r = (uint16_t)(start + i), v = (uint16_t)(data[2u * i] << 8 | data[2u * i + 1]);
        switch (r) {
        case IOB_R_OUT_CMD: n->out_cmd = v; w0 = true; break;
        case IOB_R_RELAY_CMD: n->relay_cmd = v; break;
        case IOB_R_CMD_SEQ: n->seq = v; break;
        case IOB_R_HOLD_DS: n->hold_ds = v; n->config_needed = false; break;
        case IOB_R_FILTER_MS: n->filter_ms = v; n->config_needed = false; break;
        case IOB_R_WB_ENABLE: n->wb_en = v; n->config_needed = false; break;
        case IOB_R_OL_ENABLE: n->ol_en = v; n->config_needed = false; break;
        case IOB_R_FAULT_CLEAR: if (v) { n->timeout_latched = false; n->wdg_latched = false; } break;
        default: break;
        }
    }
    if (w0) { n->last_write_ms = now; n->gate = true; n->writes0++; }
    n->echo = n->seq;                     /* the write is applied */
}

static uint16_t finish(uint8_t *tx, uint16_t len) {
    uint16_t c = mbr_crc16(tx, len);
    tx[len] = (uint8_t)c; tx[len + 1] = (uint8_t)(c >> 8);
    return (uint16_t)(len + 2);
}

static uint16_t exception(uint8_t *tx, uint8_t addr, uint8_t fc, uint8_t code) {
    tx[0] = addr; tx[1] = (uint8_t)(fc | 0x80u); tx[2] = code;
    return finish(tx, 3);
}

static uint16_t read_regs(const iob_node_t *n, uint8_t *tx, uint8_t fc, uint16_t start, uint16_t cnt) {
    if (cnt == 0 || cnt > 125) return exception(tx, n->address, fc, 3);
    if ((uint32_t)start + cnt > REG_SPACE) return exception(tx, n->address, fc, 2);
    tx[0] = n->address; tx[1] = fc; tx[2] = (uint8_t)(2u * cnt);
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t v = iob_node_reg(n, (uint16_t)(start + i));
        tx[3 + 2u * i] = (uint8_t)(v >> 8); tx[4 + 2u * i] = (uint8_t)v;
    }
    return finish(tx, (uint16_t)(3u + 2u * cnt));
}

uint16_t iob_node_frame(iob_node_t *n, uint32_t now, const uint8_t *rx, uint16_t len, uint8_t *tx, uint16_t cap) {
    (void)cap;
    if (!n->powered) return 0;
    iob_node_tick(n, now);
    n->frames++;
    if (len < 4) { n->rx_errors++; return 0; }
    if (mbr_crc16(rx, len - 2u) != (uint16_t)(rx[len - 2] | rx[len - 1] << 8)) { n->rx_errors++; return 0; }
    if (rx[0] != n->address) return 0;          /* another node, or a broadcast (not used) */
    uint8_t fc = rx[1];
    uint16_t a = 0, c = 0;
    if (len >= 6) { a = (uint16_t)((unsigned)rx[2] << 8 | rx[3]); c = (uint16_t)((unsigned)rx[4] << 8 | rx[5]); }
    uint16_t out = 0;
    switch (fc) {
    case 3: case 4:
        out = len == 8 ? read_regs(n, tx, fc, a, c) : exception(tx, n->address, fc, 3);
        break;
    case 6: {
        if (len != 8) { out = exception(tx, n->address, fc, 3); break; }
        uint8_t e = write_check(a, 1, rx + 4);
        if (e) { out = exception(tx, n->address, fc, e); break; }
        write_apply(n, now, a, 1, rx + 4);
        memcpy(tx, rx, 6);
        out = finish(tx, 6);
        break;
    }
    case 16: {
        if (len < 9 || c == 0 || c > 123 || rx[6] != 2u * c || len != 9u + 2u * c) { out = exception(tx, n->address, fc, 3); break; }
        uint8_t e = (uint32_t)a + c > REG_SPACE ? 2 : write_check(a, c, rx + 7);
        if (e) { out = exception(tx, n->address, fc, e); break; }
        write_apply(n, now, a, c, rx + 7);
        memcpy(tx, rx, 6);
        out = finish(tx, 6);
        break;
    }
    case 23: {
        if (len < 13) { out = exception(tx, n->address, fc, 3); break; }
        uint16_t wa = (uint16_t)(rx[6] << 8 | rx[7]), wc = (uint16_t)(rx[8] << 8 | rx[9]);
        if (wc == 0 || wc > 121 || rx[10] != 2u * wc || len != 13u + 2u * wc || c == 0 || c > 125) {
            out = exception(tx, n->address, fc, 3); break;
        }
        uint8_t e = ((uint32_t)wa + wc > REG_SPACE || (uint32_t)a + c > REG_SPACE) ? 2 : write_check(wa, wc, rx + 11);
        if (e) { out = exception(tx, n->address, fc, e); break; }
        write_apply(n, now, wa, wc, rx + 11);
        out = read_regs(n, tx, fc, a, c);
        break;
    }
    default:
        out = exception(tx, n->address, fc, 1);
    }
    if (out) n->replies++;
    return out;
}

/* ---------------- the bus ---------------- */
static uint16_t bus_xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t timeout) {
    (void)timeout;
    iob_simbus_t *b = (iob_simbus_t *)p->ctx;
    b->requests++;
    uint32_t now = b->now ? b->now() : bsp_ms();
    uint16_t got = 0;
    for (unsigned i = 0; i < b->n; ++i) {
        uint16_t r = iob_node_frame(b->nodes[i], now, tx, n, rx, cap);
        if (r) got = r;                   /* only the addressed node answers */
    }
    if (b->drop_next) { b->drop_next--; return 0; }
    if (got && b->corrupt_next) { b->corrupt_next--; rx[got - 1] ^= 0x5Au; }
    return got;
}

void iob_simbus_init(iob_simbus_t *b) {
    memset(b, 0, sizeof *b);
    b->port.xfer = bus_xfer;
    b->port.ctx = b;
}

void iob_simbus_add(iob_simbus_t *b, iob_node_t *n) {
    if (b->n < 4) b->nodes[b->n++] = n;
}
