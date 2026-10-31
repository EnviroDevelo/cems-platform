/* inst_sim.c - see inst_sim.h */
#include <string.h>
#include "inst_sim.h"
#include "inst.h"

void inst_sim_init(inst_sim_t *s, uint32_t ip, uint8_t unit) {
    memset(s, 0, sizeof *s);
    s->ip = ip;
    s->port = 502;
    s->unit = unit;
}

static uint16_t exc(const uint8_t *req, uint8_t fc, uint8_t code, uint8_t *out) {
    memcpy(out, req, 7);
    out[4] = 0; out[5] = 3;
    out[7] = (uint8_t)(fc | 0x80u);
    out[8] = code;
    return 9;
}

uint16_t inst_sim_serve(inst_sim_t *s, const uint8_t *req, uint16_t n, uint8_t *out, uint16_t cap) {
    if (s->silent || n < 8u || cap < 260u) return 0;
    s->requests++;
    uint8_t unit = req[6], fc = req[7];
    if (s->unit && unit != s->unit) return s->silent_other_units ? 0 : exc(req, fc, 0x0B, out);
    if (s->force_exc) return exc(req, fc, s->force_exc, out);
    if (fc < 1u || fc > 4u) return exc(req, fc, 0x01, out);
    if (n < 12u) return exc(req, fc, 0x03, out);
    uint16_t addr = (uint16_t)(req[8] << 8 | req[9]), count = (uint16_t)(req[10] << 8 | req[11]);
    bool bits = fc <= 2u;
    if (count == 0 || count > (bits ? 2000u : 125u)) return exc(req, fc, 0x03, out);
    if ((uint32_t)addr + count > INST_SIM_REGS) return exc(req, fc, 0x02, out);
    uint8_t *d = out + 9;
    unsigned nb;
    if (bits) {
        const uint8_t *src = fc == 1u ? s->coils : s->discrete;
        nb = (count + 7u) / 8u;
        memset(d, 0, nb);
        for (unsigned i = 0; i < count; ++i) {
            unsigned a = addr + i;
            if ((src[a / 8u] >> (a % 8u)) & 1u) d[i / 8u] = (uint8_t)(d[i / 8u] | 1u << (i % 8u));
        }
    } else {
        const uint16_t *src = fc == 3u ? s->holding : s->input;
        nb = 2u * count;
        for (unsigned i = 0; i < count; ++i) { d[2 * i] = (uint8_t)(src[addr + i] >> 8); d[2 * i + 1] = (uint8_t)src[addr + i]; }
    }
    memcpy(out, req, 7);
    out[4] = (uint8_t)((3u + nb) >> 8); out[5] = (uint8_t)(3u + nb);
    out[7] = fc;
    out[8] = (uint8_t)nb;
    return (uint16_t)(9u + nb);
}

void inst_sim_put_u32(uint16_t *regs, uint16_t addr, uint32_t v, uint8_t order) {
    uint8_t m[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }, a, b, c, d;
    switch (order) {                    /* the wire bytes A B C D that make m in this order */
    case INST_O_CDAB: c = m[0]; d = m[1]; a = m[2]; b = m[3]; break;
    case INST_O_BADC: b = m[0]; a = m[1]; d = m[2]; c = m[3]; break;
    case INST_O_DCBA: d = m[0]; c = m[1]; b = m[2]; a = m[3]; break;
    default:          a = m[0]; b = m[1]; c = m[2]; d = m[3]; break;
    }
    regs[addr] = (uint16_t)(a << 8 | b);
    regs[addr + 1u] = (uint16_t)(c << 8 | d);
}

void inst_sim_put_f32(uint16_t *regs, uint16_t addr, float v, uint8_t order) {
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    inst_sim_put_u32(regs, addr, u, order);
}
