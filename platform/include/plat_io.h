/* plat_io.h - the platform's physical I/O image.
 *
 * The scan engine reads the hardware into a plat_io_in_t at the start of every
 * scan and writes a plat_io_out_t at the end (rule E11: inputs are frozen for the
 * whole scan, so behaviour is repeatable). The application maps these generic
 * channels onto its own tags (TA30_DI[], UID_DI[], TA30_AI[] ...) in its
 * io_in / io_out hooks; the platform never touches application tags.
 *
 * Sizes are upper bounds that cover 10_IO_Map.md (22 DI, 20 DO, 2 AI, 2 RTD in use)
 * with room to spare. Channels come from the main board's own pins (bsp_io_read) and
 * from the I/O boards on RS-485 (iobus.h), each board on its configured channel range. The real channel count is fixed by the H5 I/O list
 * (design spec "Decisions before schematic"); only the BSP needs to change. */
#ifndef PLAT_IO_H
#define PLAT_IO_H
#include <stdint.h>
#include <stdbool.h>

#define PLAT_DI_MAX   64            /* main-board inputs plus up to 3 I/O boards (iobus.h) */
#define PLAT_DO_MAX   64
#define PLAT_AI_MAX   8
#define PLAT_RTD_MAX  24           /* main board's own, plus 6 per I/O board (iobus.h, map 2) */
#define PLAT_AO_MAX   8            /* main board's own, plus each I/O board's pump PWM */

/* Analog channel status, per channel */
typedef enum {
    PLAT_AIN_OK = 0,
    PLAT_AIN_UNDER,       /* below range (e.g. < 3.6 mA on 4-20 mA) */
    PLAT_AIN_OVER,        /* above range / open RTD */
    PLAT_AIN_FAULT,       /* converter or wiring fault */
    PLAT_AIN_NOT_FITTED   /* channel not present on this hardware */
} plat_ain_status_t;

typedef struct {
    uint64_t di;                     /* bit n = digital input n (1 = active)      */
    uint64_t di_fault;               /* bit n = input n unreliable: wire break, I/O board lost (iobus.h) */
    uint64_t do_fault;               /* bit n = output n fault: open circuit, short, overheating, no field supply */
    int32_t  ai_raw[PLAT_AI_MAX];    /* raw converter counts, scaling in the app  */
    uint8_t  ai_status[PLAT_AI_MAX]; /* plat_ain_status_t                         */
    int32_t  rtd_cdeg[PLAT_RTD_MAX]; /* temperature in 0.01 degC                  */
    uint8_t  rtd_status[PLAT_RTD_MAX];
} plat_io_in_t;

typedef struct {
    uint64_t do_bits;                /* bit n = digital output n (1 = energised) */
    int32_t  ao_raw[PLAT_AO_MAX];    /* an I/O board's pump PWM: 0.1 %, sign = DIR (iobus.h) */
} plat_io_out_t;

static inline bool plat_di(const plat_io_in_t *in, unsigned n) {
    return n < PLAT_DI_MAX && ((in->di >> n) & 1u);
}
static inline void plat_do_set(plat_io_out_t *out, unsigned n, bool v) {
    if (n >= PLAT_DO_MAX) return;
    if (v) out->do_bits |= ((uint64_t)1 << n); else out->do_bits &= ~((uint64_t)1 << n);
}
static inline bool plat_di_fault(const plat_io_in_t *in, unsigned n) {
    return n < PLAT_DI_MAX && ((in->di_fault >> n) & 1u);
}
static inline bool plat_do_fault(const plat_io_in_t *in, unsigned n) {
    return n < PLAT_DO_MAX && ((in->do_fault >> n) & 1u);
}

#endif
