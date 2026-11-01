/* inst_sim.h - a simulated third-party instrument (a gas analyser, or a Modbus TCP to RTU gateway
 * with instruments behind it), host only: a Modbus TCP server with coils, discrete inputs,
 * holding and input registers, for the instrument poller's tests (inst.h) and the PC simulation.
 *
 *  - functions 01-04 only (others: exception 01); an address range outside the tables: 02;
 *    a count of 0 or more than 125 registers / 2000 bits: 03
 *  - unit: 0 answers any unit id; otherwise another unit id gets exception 0B (a gateway whose
 *    instrument does not answer), or no reply at all with silent_other_units
 *  - silent: no reply at all; force_exc: every request gets this exception code */
#ifndef INST_SIM_H
#define INST_SIM_H
#include <stdbool.h>
#include <stdint.h>

#define INST_SIM_REGS 512u

typedef struct inst_sim {
    uint32_t ip;
    uint16_t port;
    uint8_t  unit;                       /* 0: any */
    bool     silent, silent_other_units, refuse;
    uint8_t  force_exc;
    uint16_t holding[INST_SIM_REGS], input[INST_SIM_REGS];
    uint8_t  coils[INST_SIM_REGS / 8], discrete[INST_SIM_REGS / 8];
    uint32_t requests;
} inst_sim_t;

void inst_sim_init(inst_sim_t *s, uint32_t ip, uint8_t unit);
/* One Modbus TCP request frame (whole) in, its reply out; returns the reply length (0: none). */
uint16_t inst_sim_serve(inst_sim_t *s, const uint8_t *req, uint16_t n, uint8_t *out, uint16_t cap);
/* Helpers to put values in: 32-bit values in the given word/byte order (INST_O_*). */
void inst_sim_put_f32(uint16_t *regs, uint16_t addr, float v, uint8_t order);
void inst_sim_put_u32(uint16_t *regs, uint16_t addr, uint32_t v, uint8_t order);

#endif
