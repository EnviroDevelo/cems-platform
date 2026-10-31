/* drv_ad4112.c - the analog inputs, ADI AD4112 behind an ISOW7741 (CORE PCB rev 0.2, io_ana sheet),
 * on the node's SPI bus with its own chip select (PORT_CS_AI), SPI mode 3, at most 4 MHz.
 *
 * Channels (CORE_PCB.md 14):
 *   AD4112 IIN0-3        -> analog inputs 1-4, 4-20 mA on the chip's own shunts
 *   VIN0, VIN1           -> inputs 5-6, 4-20 mA on 250 ohm 0.1 % shunts (reported in uA)
 *   VIN2                 -> input 7, level (mV)
 *   VIN3                 -> input 8, mass flow (mV), corrected ratiometrically to a 10.000 V supply
 *   VIN4                 -> the 10 V sensor supply through a /2 divider (supply[0], mV)
 *   VIN5                 -> the 5 V level supply (supply[1], mV)
 * The sequencer cycles the ten channels continuously; each pass reads at most one finished
 * conversion (DATA with the status byte appended tells which channel it is).
 *
 * DATASHEET STATUS: NOT yet checked (drivers.h). The register addresses, the input codes, the
 * setup bits and the current-input scale below were written from the AD411x family layout and
 * are marked TO CHECK. Until AD4112_CONFIRMED is 1 this driver is built in its fail-safe form:
 * every input reports IOB_TS_ADC and 0, and nothing is written to the chip. */
#include <string.h>
#include "drivers.h"
#include "node_port.h"
#include "iobus.h"

#define AI_SPI_MODE 3u

static void fail_all(node_ai_t *a) {
    for (unsigned i = 0; i < 8u; ++i) { a->v[i] = 0; a->st[i] = IOB_TS_ADC; }
    a->supply[0] = a->supply[1] = 0;
}

#if AD4112_CONFIRMED
/* registers (TO CHECK against the AD4112 datasheet) */
#define R_STATUS   0x00u
#define R_ADCMODE  0x01u
#define R_IFMODE   0x02u
#define R_DATA     0x04u
#define R_ID       0x07u
#define R_CH0      0x10u
#define R_SETUP0   0x20u
#define R_FILT0    0x28u
#define RD         0x40u
#define ID_MASK    0xFFF0u
#define ID_AD4112  0x30E0u            /* TO CHECK */
#define ADCMODE_V  0x8000u            /* REF_EN, continuous conversion, internal clock (TO CHECK) */
#define IFMODE_V   0x0040u            /* DATA_STAT: status byte appended to DATA */
#define SETUP_V    0x1320u            /* bipolar, input buffers on, internal 2.5 V reference (TO CHECK) */
#define FILT_V     0x0508u            /* about 50 SPS per channel with rejection of 50/60 Hz (TO CHECK) */
#define VREF_UV    2500000.0
#define VIN_DIV    10.0               /* the chip's voltage-input divider */
#define IIN_OHMS   50.0               /* the chip's current-input shunt (TO CHECK) */
#define SHUNT_OHMS 250.0              /* inputs 5-6 on the board */
#define N_CH       10u
static const uint16_t in_code[N_CH] = {   /* TO CHECK: the chip's input-pair codes */
    0x1E8u, 0x1C9u, 0x1AAu, 0x18Bu,       /* IIN0..IIN3 */
    0x010u, 0x030u, 0x050u, 0x070u,       /* VIN0..VIN3 against VINCOM */
    0x090u, 0x0B0u };                     /* VIN4, VIN5 */

static bool cfg_done;
static uint32_t retry_at;
static double v10_mv = 10000.0;           /* last 10 V supply reading, for the ratiometric input */

static int rd(uint8_t reg, uint8_t *v, uint8_t n) {
    uint8_t tx[5] = { (uint8_t)(RD | reg), 0, 0, 0, 0 }, rx[5];
    if (port_spi_xfer(PORT_CS_AI, AI_SPI_MODE, tx, rx, (uint16_t)(n + 1u)) < 0) return -1;
    memcpy(v, rx + 1, n);
    return 0;
}
static int wr16(uint8_t reg, uint16_t v) {
    uint8_t tx[3] = { reg, (uint8_t)(v >> 8), (uint8_t)v };
    return port_spi_xfer(PORT_CS_AI, AI_SPI_MODE, tx, NULL, 3);
}

static bool configure(void) {
    uint8_t ff[8]; memset(ff, 0xFF, sizeof ff);
    if (port_spi_xfer(PORT_CS_AI, AI_SPI_MODE, ff, NULL, sizeof ff) < 0) return false;   /* serial reset: 64 ones */
    port_delay_us(500);
    uint8_t id[2];
    if (rd(R_ID, id, 2) < 0 || (((uint16_t)id[0] << 8 | id[1]) & ID_MASK) != ID_AD4112) return false;
    if (wr16(R_SETUP0, SETUP_V) < 0 || wr16(R_FILT0, FILT_V) < 0 || wr16(R_IFMODE, IFMODE_V) < 0) return false;
    for (unsigned c = 0; c < N_CH; ++c)
        if (wr16((uint8_t)(R_CH0 + c), (uint16_t)(0x8000u | in_code[c])) < 0) return false;   /* enabled, setup 0 */
    return wr16(R_ADCMODE, ADCMODE_V) >= 0;
}

static uint8_t current_state(double ua) {
    return ua < 1000.0 ? IOB_TS_OPEN : ua < 3600.0 ? IOB_TS_UNDER : ua > 21000.0 ? IOB_TS_OVER : IOB_TS_OK;
}
static uint16_t clamp16(double x) { return x <= 0.0 ? 0u : x >= 65535.0 ? 0xFFFFu : (uint16_t)(x + 0.5); }

void ad4112_poll(void *ctx, uint32_t now, node_ai_t *a) {
    (void)ctx;
    if (!cfg_done) {
        if ((int32_t)(now - retry_at) < 0) return;
        retry_at = now + 1000u;
        if (!configure()) { fail_all(a); return; }
        cfg_done = true;
        for (unsigned i = 0; i < 8u; ++i) a->st[i] = IOB_TS_WAIT;
    }
    uint8_t st;
    if (rd(R_STATUS, &st, 1) < 0) { cfg_done = false; fail_all(a); return; }
    if (st & 0x80u) return;                                   /* RDY high: nothing new */
    uint8_t d[4];
    if (rd(R_DATA, d, 4) < 0) { cfg_done = false; fail_all(a); return; }
    unsigned ch = d[3] & 0x0Fu;
    if (ch >= N_CH) return;
    int32_t code = (int32_t)((uint32_t)d[0] << 16 | (uint32_t)d[1] << 8 | d[2]) - 0x800000;
    double uv = (double)code / 8388608.0 * VREF_UV;           /* at the modulator */
    bool fs = code >= 0x7FFFF0 || code <= -0x7FFFF0;
    if (ch < 4u) {                                           /* IIN: uA */
        double ua = uv / IIN_OHMS;
        a->v[ch] = clamp16(ua); a->st[ch] = fs ? IOB_TS_OVER : current_state(ua);
    } else {
        double mv = uv * VIN_DIV / 1000.0;
        if (ch < 6u) {                                        /* 250 ohm shunts: uA */
            double ua = mv * 1000.0 / SHUNT_OHMS;
            a->v[ch] = clamp16(ua); a->st[ch] = fs ? IOB_TS_OVER : current_state(ua);
        } else if (ch == 6u) {
            a->v[6] = clamp16(mv); a->st[6] = fs ? IOB_TS_OVER : IOB_TS_OK;
        } else if (ch == 7u) {
            double corr = v10_mv > 1000.0 ? mv * 10000.0 / v10_mv : mv;
            a->v[7] = clamp16(corr); a->st[7] = fs ? IOB_TS_OVER : v10_mv > 1000.0 ? IOB_TS_OK : IOB_TS_ADC;
        } else if (ch == 8u) {
            v10_mv = mv * 2.0; a->supply[0] = clamp16(v10_mv);
        } else {
            a->supply[1] = clamp16(mv);
        }
    }
}
void ad4112_reset_state(void) { cfg_done = false; retry_at = 0; v10_mv = 10000.0; }
#else
void ad4112_poll(void *ctx, uint32_t now, node_ai_t *a) { (void)ctx; (void)now; fail_all(a); }
void ad4112_reset_state(void) {}
#endif
