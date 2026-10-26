/* iob_node_sim.c - see iob_node_sim.h */
#include <string.h>
#include "iob_node_sim.h"
#include "mb_rtu.h"
#include "bsp.h"

#define REG_SPACE(n) ((n)->map_version >= 2u ? 107u : 80u)
#define COMMS_TIMEOUT_MS 1000u


void iob_node_init(iob_node_t *n, uint8_t address) {
    memset(n, 0, sizeof *n);
    n->address = address;
    n->powered = true;
    n->magic = IOB_MAGIC; n->map_version = IOB_MAP_VERSION;
    n->n_out_a = 8; n->n_out_b = 8; n->n_relays = 2; n->n_inputs = 8;
    n->n_temp_pwm = IOB_TEMPS | 1u << 8;
    for (unsigned i = 0; i < IOB_TEMPS; ++i) { n->temp[i] = IOB_TEMP_INVALID; n->temp_st[i] = IOB_TS_OFF; }
    n->cj = IOB_TEMP_INVALID;
    n->fw_version = 0x0100; n->hw_rev = 1;
    for (unsigned i = 0; i < 6; ++i) n->uid[i] = (uint16_t)(0x1000u * address + i);
    n->field_a = n->field_b = true;
    if (address >= 1 && address <= 3) { n->ip = 0xC0A8013Cu + address; n->mask = 0xFFFFFF00u; }
    iob_mac_from_uid(n->uid, n->mac);
    iob_node_restart(n, false, 0);
    n->resets = 0;
}

void iob_node_restart(iob_node_t *n, bool wdg, uint32_t now) {
    n->out_cmd = n->relay_cmd = n->seq = n->echo = 0;
    n->hold_ds = 3000; n->filter_ms = 8; n->wb_en = 0; n->ol_en = 0xFF;
    n->pwm_cmd = n->pwm_dir = 0; n->pwm_hz = 1000; n->tach_ppr = 2;
    for (unsigned k = 0; k < IOB_TEMP_CONNS; ++k) { n->temp_type[k] = IOB_TT_NONE; n->ntc_r25[k] = 1000; n->ntc_beta[k] = 3435; }
    n->config_needed = true;
    n->timeout_latched = false;
    n->wdg_latched = wdg;
    n->gate = false;                   /* closed until the first valid output write */
    n->last_write_ms = now;
    n->comms_seen = n->comms_lost = false;
    n->in_rep = n->in_pend = 0;
    n->resets++;
}

void iob_node_tick(iob_node_t *n, uint32_t now) {
    if (n->gate && (uint32_t)(now - n->last_write_ms) >= (uint32_t)n->hold_ds * 100u) {
        n->gate = false;
        n->timeout_latched = true;
    }
    if (n->comms_seen && (uint32_t)(now - n->last_comms_ms) >= COMMS_TIMEOUT_MS) n->comms_lost = true;
    if (n->field_a) n->in_pend |= (uint8_t)(n->inputs ^ n->in_rep);
    else n->in_pend = n->in_rep;      /* no field supply: 0 at once, whatever was pending */
}

static bool has_temp(const iob_node_t *n) { return n->map_version >= 2u && (n->n_temp_pwm & 0xFFu) != 0; }
static bool has_pwm(const iob_node_t *n) { return n->map_version >= 2u && (n->n_temp_pwm >> 8) != 0; }

uint16_t iob_node_reg(const iob_node_t *n, uint16_t r) {
    uint16_t st = 0;
    if (n->map_version >= 2u) {
        if (r >= IOB_R_TEMP && r < IOB_R_TEMP + IOB_TEMPS) return (uint16_t)(has_temp(n) ? n->temp[r - IOB_R_TEMP] : IOB_TEMP_INVALID);
        if (r >= IOB_R_TEMP_STATE && r < IOB_R_TEMP_STATE + IOB_TEMPS) return has_temp(n) ? n->temp_st[r - IOB_R_TEMP_STATE] : IOB_TS_OFF;
        if (r >= IOB_R_TEMP_TYPE && r < IOB_R_TEMP_TYPE + IOB_TEMP_CONNS) return n->temp_type[r - IOB_R_TEMP_TYPE];
        if (r >= IOB_R_NTC_R25 && r < IOB_R_NTC_R25 + IOB_TEMP_CONNS) return n->ntc_r25[r - IOB_R_NTC_R25];
        if (r >= IOB_R_NTC_BETA && r < IOB_R_NTC_BETA + IOB_TEMP_CONNS) return n->ntc_beta[r - IOB_R_NTC_BETA];
        switch (r) {
        case IOB_R_PWM_CMD: return n->pwm_cmd;
        case IOB_R_PWM_DIR: return n->pwm_dir;
        case IOB_R_PWM_APPLIED: return n->gate ? n->pwm_cmd : 0u;
        case IOB_R_TACH_RPM: return has_pwm(n) ? n->tach_rpm : 0u;
        case IOB_R_TEMP_FLAGS: return has_temp(n) ? n->temp_flags : 0u;
        case IOB_R_TEMP_COUNT: return has_temp(n) ? n->temp_count : 0u;
        case IOB_R_CJ_TEMP: return (uint16_t)(has_temp(n) ? n->cj : IOB_TEMP_INVALID);
        case IOB_R_PWM_FREQ: return n->pwm_hz;
        case IOB_R_TACH_PPR: return n->tach_ppr;
        case IOB_R_N_TEMP_PWM: return n->n_temp_pwm;
        default: break;
        }
    }
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
        if (n->comms_lost) st |= IOB_ST_COMMS_LOST;
        return st;
    case IOB_R_INPUTS: return (uint8_t)(n->in_rep ^ n->in_pend);
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
    case IOB_R_NODE_ADDR: return n->address;
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
        if (r >= IOB_R_SERIAL && r < IOB_R_SERIAL + IOB_SERIAL_REGS)
            return (uint16_t)(n->serial[2u * (r - IOB_R_SERIAL)] << 8 | n->serial[2u * (r - IOB_R_SERIAL) + 1u]);
        return 0;                         /* reserved */
    }
}

static bool writable(const iob_node_t *n, uint16_t r) {
    return r <= IOB_R_CMD_SEQ || (r >= IOB_R_HOLD_DS && r <= IOB_R_FAULT_CLEAR) ||
           (has_pwm(n) && (r == IOB_R_PWM_CMD || r == IOB_R_PWM_DIR || r == IOB_R_PWM_FREQ || r == IOB_R_TACH_PPR)) ||
           (has_temp(n) && r >= IOB_R_TEMP_TYPE && r < IOB_R_NTC_BETA + IOB_TEMP_CONNS);
}

static bool value_ok(uint16_t r, uint16_t v) {
    if (r >= IOB_R_TEMP_TYPE && r < IOB_R_TEMP_TYPE + IOB_TEMP_CONNS)
        return v == IOB_TT_NONE || (v >= IOB_TT_TC_K && v <= IOB_TT_TC_S) || (v >= IOB_TT_PT100_2W && v <= IOB_TT_PT1000_4W) ||
               v == IOB_TT_NTC;
    if (r >= IOB_R_NTC_R25 && r < IOB_R_NTC_R25 + IOB_TEMP_CONNS) return v >= 10;
    if (r >= IOB_R_NTC_BETA && r < IOB_R_NTC_BETA + IOB_TEMP_CONNS) return v >= 1000 && v <= 10000;
    switch (r) {
    case IOB_R_PWM_CMD: return v <= 1000;
    case IOB_R_PWM_DIR: return v <= 1;
    case IOB_R_PWM_FREQ: return v >= 10 && v <= 25000;
    case IOB_R_TACH_PPR: return v >= 1 && v <= 12;
    case IOB_R_RELAY_CMD: return v <= 3;
    case IOB_R_HOLD_DS: return v >= 1 && v <= 6000;
    case IOB_R_FILTER_MS: return v <= 100;
    case IOB_R_WB_ENABLE: case IOB_R_OL_ENABLE: return v <= 255;
    case IOB_R_FAULT_CLEAR: return v <= 3;
    default: return true;
    }
}

/* check a whole write first (exception code, 0 = fine), then apply it */
static uint8_t write_check(const iob_node_t *n, uint16_t start, uint16_t cnt, const uint8_t *data) {
    if (n->address == 0) return 4;        /* commissioning: nothing switches */
    for (uint16_t i = 0; i < cnt; ++i) {
        uint16_t r = (uint16_t)(start + i), v = (uint16_t)(data[2u * i] << 8 | data[2u * i + 1]);
        if (!writable(n, r)) return 2;
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
        case IOB_R_PWM_CMD: n->pwm_cmd = v; break;
        case IOB_R_PWM_DIR: n->pwm_dir = v; break;
        case IOB_R_PWM_FREQ: n->pwm_hz = v; break;
        case IOB_R_TACH_PPR: n->tach_ppr = v; break;
        case IOB_R_FAULT_CLEAR:
            if (v & IOB_FC_EVENTS) { n->timeout_latched = false; n->wdg_latched = false; }
            if (v & IOB_FC_COMMS) n->comms_lost = false;
            break;
        default:
            if (r >= IOB_R_TEMP_TYPE && r < IOB_R_TEMP_TYPE + IOB_TEMP_CONNS) n->temp_type[r - IOB_R_TEMP_TYPE] = v;
            else if (r >= IOB_R_NTC_R25 && r < IOB_R_NTC_R25 + IOB_TEMP_CONNS) n->ntc_r25[r - IOB_R_NTC_R25] = v;
            else if (r >= IOB_R_NTC_BETA && r < IOB_R_NTC_BETA + IOB_TEMP_CONNS) n->ntc_beta[r - IOB_R_NTC_BETA] = v;
            break;
        }
    }
    if (w0) { n->last_write_ms = now; n->gate = true; n->writes0++; }
    n->echo = n->seq;                     /* the write is applied */
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }

/* the address change (iobus.h, FW-61): key, address, address XOR 0xFFFF; 0 or an exception code */
static uint8_t addr_set(const uint8_t *d, uint8_t *addr) {
    uint16_t k = get16(d), a = get16(d + 2), inv = get16(d + 4);
    if (k != IOB_ADDR_SET_KEY || inv != (uint16_t)(a ^ 0xFFFFu) || a == 0 || a > IOB_ADDR_MAX) return 3;
    *addr = (uint8_t)a;
    return 0;
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

static uint16_t read_regs(const iob_node_t *n, uint8_t *tx, uint8_t ra, uint8_t fc, uint16_t start, uint16_t cnt) {
    if (cnt == 0 || cnt > 125) return exception(tx, ra, fc, 3);
    if ((uint32_t)start + cnt > REG_SPACE(n)) return exception(tx, ra, fc, 2);
    tx[0] = ra; tx[1] = fc; tx[2] = (uint8_t)(2u * cnt);
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
    bool commissioning = n->address == 0;
    if (rx[0] != (commissioning ? IOB_COMMISSION_ADDR : n->address)) return 0;   /* another node, or a broadcast */
    if (!commissioning) { n->comms_seen = true; n->last_comms_ms = now; }
    uint8_t ra = rx[0];
    uint8_t fc = rx[1];
    uint16_t a = 0, c = 0;
    if (len >= 6) { a = (uint16_t)((unsigned)rx[2] << 8 | rx[3]); c = (uint16_t)((unsigned)rx[4] << 8 | rx[5]); }
    uint16_t out = 0;
    if (fc == 16 && len >= 9 && ((a == IOB_R_ADDR_SET && c == 3) || (a == IOB_R_SERIAL && c == IOB_SERIAL_REGS))) {
        uint8_t e, new_addr = 0;
        if (rx[6] != 2u * c || len != 9u + 2u * c) e = 3;
        else if (a == IOB_R_ADDR_SET) e = addr_set(rx + 7, &new_addr);
        else e = 2;                       /* the serial number is written over USB only */
        if (e) out = exception(tx, ra, fc, e);
        else { memcpy(tx, rx, 6); out = finish(tx, 6); }
        if (new_addr) n->address = new_addr;
        n->replies++;
        return out;
    }
    switch (fc) {
    case 3: case 4:
        out = len == 8 ? read_regs(n, tx, ra, fc, a, c) : exception(tx, ra, fc, 3);
        break;
    case 6: {
        if (len != 8) { out = exception(tx, ra, fc, 3); break; }
        uint8_t e = write_check(n, a, 1, rx + 4);
        if (e) { out = exception(tx, ra, fc, e); break; }
        write_apply(n, now, a, 1, rx + 4);
        memcpy(tx, rx, 6);
        out = finish(tx, 6);
        break;
    }
    case 16: {
        if (len < 9 || c == 0 || c > 123 || rx[6] != 2u * c || len != 9u + 2u * c) { out = exception(tx, ra, fc, 3); break; }
        uint8_t e = (uint32_t)a + c > REG_SPACE(n) ? 2 : write_check(n, a, c, rx + 7);
        if (e) { out = exception(tx, ra, fc, e); break; }
        write_apply(n, now, a, c, rx + 7);
        memcpy(tx, rx, 6);
        out = finish(tx, 6);
        break;
    }
    case 23: {
        if (len < 13) { out = exception(tx, ra, fc, 3); break; }
        uint16_t wa = (uint16_t)(rx[6] << 8 | rx[7]), wc = (uint16_t)(rx[8] << 8 | rx[9]);
        if (wc == 0 || wc > 121 || rx[10] != 2u * wc || len != 13u + 2u * wc || c == 0 || c > 125) {
            out = exception(tx, ra, fc, 3); break;
        }
        uint8_t e = ((uint32_t)wa + wc > REG_SPACE(n) || (uint32_t)a + c > REG_SPACE(n)) ? 2 : write_check(n, wa, wc, rx + 11);
        if (e) { out = exception(tx, ra, fc, e); break; }
        write_apply(n, now, wa, wc, rx + 11);
        out = read_regs(n, tx, ra, fc, a, c);
        break;
    }
    default:
        out = exception(tx, ra, fc, 1);
    }
    if (out > 5 && (fc == 3 || fc == 4 || fc == 23) && !(tx[1] & 0x80u) && a <= IOB_R_INPUTS &&
        (uint32_t)a + c > IOB_R_INPUTS) {       /* the master has seen these changes */
        n->in_rep = (uint8_t)(n->in_rep ^ n->in_pend);
        n->in_pend = 0;
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

/* ---------------- the simulated Ethernet ---------------- */
static uint16_t net_xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t timeout) {
    (void)timeout;
    iob_simnet_t *s = (iob_simnet_t *)p->ctx;
    s->requests++;
    uint32_t now = s->now ? s->now() : bsp_ms();
    uint8_t mbap[IOB_MBAP_MAX], req[IOB_MBAP_MAX], rep[IOB_MBAP_MAX];
    size_t m = iob_mbap_from_rtu(tx, n, (uint16_t)s->requests, mbap);
    if (!m) return 0;
    for (unsigned i = 0; i < s->n; ++i) {
        iob_node_t *nd = s->nodes[i];
        if (!nd->powered || nd->ip == 0 || nd->ip != s->target_ip) continue;
        if (nd->master_ip && nd->master_ip != s->master_ip) continue;      /* another client: refused */
        uint16_t tid;
        size_t rl = iob_rtu_from_mbap(mbap, m, &tid, req);
        uint16_t r = rl ? iob_node_frame(nd, now, req, (uint16_t)rl, rep, sizeof rep) : 0;
        if (!r || s->drop_next) { if (s->drop_next) s->drop_next--; return 0; }
        size_t back = iob_mbap_from_rtu(rep, r, tid, mbap);              /* the board's TCP reply */
        uint8_t out[IOB_MBAP_MAX];
        size_t ol = back ? iob_rtu_from_mbap(mbap, back, NULL, out) : 0;  /* and the master's side */
        if (!ol || ol > cap) return 0;
        memcpy(rx, out, ol);
        return (uint16_t)ol;
    }
    return 0;
}

static void net_target(iob_port_t *p, uint32_t ip) { ((iob_simnet_t *)p->ctx)->target_ip = ip; }

static void net_here(iob_simnet_t *s, const iob_node_t *nd, uint16_t seq) {
    if (s->qn >= 8) return;
    iob_disc_t h; memset(&h, 0, sizeof h);
    h.op = IOB_DISC_HERE; h.seq = seq;
    memcpy(h.uid, nd->uid, sizeof h.uid);
    memcpy(h.mac, nd->mac, 6);
    for (unsigned k = 0; k < 16 && nd->serial[k]; ++k) h.serial[k] = (char)nd->serial[k];
    h.fw_version = nd->fw_version; h.hw_rev = nd->hw_rev; h.fw_build = 1;
    h.flags = (uint16_t)((nd->ip ? 0u : IOB_DISC_F_NEW) | (nd->comms_seen && !nd->comms_lost ? IOB_DISC_F_CONTROLLED : 0u));
    h.ip = nd->ip; h.mask = nd->mask; h.gw = nd->gw; h.master_ip = nd->master_ip; h.address = nd->address;
    (void)iob_disc_encode(&h, s->q[s->qn++]);
}

static bool net_send(iob_dgram_t *d, const uint8_t *buf, uint16_t len) {
    iob_simnet_t *s = (iob_simnet_t *)d->ctx;
    iob_disc_t m;
    s->datagrams++;
    if (!iob_disc_decode(buf, len, &m)) return true;
    s->qn = s->qi = 0;
    for (unsigned i = 0; i < s->n; ++i) {
        iob_node_t *nd = s->nodes[i];
        if (!nd->powered) continue;
        if (m.op == IOB_DISC_FIND) net_here(s, nd, m.seq);
        else if (m.op == IOB_DISC_SET && iob_disc_set_accept(&m, nd->uid, nd->master_ip, s->master_ip)) {
            nd->ip = m.ip; nd->mask = m.mask; nd->gw = m.gw; nd->master_ip = m.master_ip; nd->address = m.address;
            net_here(s, nd, m.seq);
        }
    }
    return true;
}

static uint16_t net_recv(iob_dgram_t *d, uint8_t *buf, uint16_t cap, uint32_t timeout) {
    (void)timeout;
    iob_simnet_t *s = (iob_simnet_t *)d->ctx;
    if (s->qi >= s->qn || cap < IOB_DISC_LEN) return 0;
    memcpy(buf, s->q[s->qi++], IOB_DISC_LEN);
    return IOB_DISC_LEN;
}

void iob_simnet_init(iob_simnet_t *s, uint32_t master_ip) {
    memset(s, 0, sizeof *s);
    s->port.xfer = net_xfer;
    s->port.target = net_target;
    s->port.ctx = s;
    s->disc.send = net_send;
    s->disc.recv = net_recv;
    s->disc.ctx = s;
    s->master_ip = master_ip;
}

void iob_simnet_add(iob_simnet_t *s, iob_node_t *n) {
    if (s->n < 4) s->nodes[s->n++] = n;
}
