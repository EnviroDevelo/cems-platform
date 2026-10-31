/* node_io_stm32h5.c - the CORE PCB rev 0.2 in the I/O node role (CORE_PCB.md section 14):
 * node_port.h for the chip drivers in node/drivers, and the plat_node.h hooks (bsp_node_chips,
 * bsp_node_pass) that hand those drivers to the node core and run the output gate.
 *
 * CubeMX user labels (main.h); without CS_A this file builds to nothing and the node uses the
 * platform's pins (the Nucleo bench). All pins are still marked CHECK in the schematic.
 *   SPI4 (PE12/13/14), shared with the W5500 through spi4_bus.h; the chips run at 4 MHz.
 *   CS_A PE2 (bank A ISO2H823V2.5), CS_B PE3 (bank B ISO808A-1), CS_IN PE4 (inputs ISO1I813T),
 *   CS_T PE15 (ADS124S08), CS_AI PE0 (AD4112): outputs, initial high.
 *   A_ERR PD1, IN_ERR PC13, B_FAULT PD14 (active low), B_PGOOD PD15: inputs with pull-ups.
 *   GATE_EN PB8 (out, initial low), WDI PB9 (out), GATE_FB PD8 (in).
 *   PUMP: TIM1_CH1 PA8 PWM (TIM1 counting at 1 MHz: PSC set in CubeMX), TIM2_CH1 PA0 input
 *   capture of the tacho (TIM2 free running at 1 MHz, rising edge), PUMP_DIR PC6, PUMP_EN PC7.
 *   REL1_N PB4, REL2_N PB5: open drain, low = on.
 *
 * The gate (MAX6369, 30 ms window, AND GATE_EN): WDI is toggled on each pass in which the node
 * core says HEALTHY (FW-22), so it only moves while the core runs and its gate is open; the
 * iobus task passes every millisecond. GATE_EN follows the core's gate. A stuck task, or a core
 * that closed its gate, lets the hardware drop bank A, bank B, the pump and the relays.
 *
 * Not here yet: the AD4112 (4-20 mA, level, mass flow). The node map has no registers for it;
 * CS_AI is held high so it never drives the bus. */
#include "main.h"
#include "bsp.h"
#include "plat_node.h"

#if defined(CS_A_Pin) && defined(HAL_SPI_MODULE_ENABLED)
#include "node_port.h"
#include "drivers.h"
#include "spi4_bus.h"

#define CHIP_HZ 4000000u
#define SPI_TMO 5u
#ifndef NODE_SPI_HANDLE
#define NODE_SPI_HANDLE hspi4
#endif
extern SPI_HandleTypeDef NODE_SPI_HANDLE;

static inline void pin(GPIO_TypeDef *p, uint16_t n, bool v) { p->BSRR = v ? n : (uint32_t)n << 16; }
static inline bool in(const GPIO_TypeDef *p, uint16_t n) { return (p->IDR & n) != 0; }

/* ---------------- node_port.h ---------------- */
int port_spi_xfer(port_cs_t cs, uint8_t mode, const uint8_t *tx, uint8_t *rx, uint16_t n) {
    GPIO_TypeDef *port; uint16_t p;
    switch (cs) {
    case PORT_CS_BANK_A: port = CS_A_GPIO_Port;  p = CS_A_Pin;  break;
    case PORT_CS_BANK_B: port = CS_B_GPIO_Port;  p = CS_B_Pin;  break;
    case PORT_CS_INPUTS: port = CS_IN_GPIO_Port; p = CS_IN_Pin; break;
    case PORT_CS_TEMP:   port = CS_T_GPIO_Port;  p = CS_T_Pin;  break;
    default: return -1;
    }
    static uint8_t zero[32], sink[32];
    spi4_claim(mode, CHIP_HZ);
    pin(port, p, false);
    HAL_StatusTypeDef st = HAL_OK;
    for (uint16_t k = 0; k < n && st == HAL_OK; ) {           /* the drivers send a few bytes at a time */
        uint16_t m = (uint16_t)((unsigned)(n - k) > sizeof zero ? sizeof zero : (unsigned)(n - k));
        st = HAL_SPI_TransmitReceive(&NODE_SPI_HANDLE, (uint8_t *)(uintptr_t)(tx ? tx + k : zero), rx ? rx + k : sink, m, SPI_TMO);
        k = (uint16_t)(k + m);
    }
    pin(port, p, true);
    spi4_release();
    return st == HAL_OK ? 0 : -1;
}

bool port_bank_b_fault(void) { return !in(B_FAULT_GPIO_Port, B_FAULT_Pin); }
bool port_bank_b_pgood(void) { return in(B_PGOOD_GPIO_Port, B_PGOOD_Pin); }

void port_relays(uint8_t r) {
    pin(REL1_N_GPIO_Port, REL1_N_Pin, !(r & 1u));
    pin(REL2_N_GPIO_Port, REL2_N_Pin, !(r & 2u));
}

void port_delay_us(uint32_t us) {                              /* the DWT counter (started by bsp_init) */
    uint32_t t0 = DWT->CYCCNT, n = us * (SystemCoreClock / 1000000u);
    while ((uint32_t)(DWT->CYCCNT - t0) < n) {}
}

/* rev 0.2 has no switch for the temperature island's supply: it is on with the board */
bool port_temp_supply_ok(void) { return true; }
void port_temp_enable(bool on) { (void)on; }

void port_pwm(uint16_t duty, uint16_t freq_hz, bool dir) {
    pin(PUMP_DIR_GPIO_Port, PUMP_DIR_Pin, dir);
    if (duty == 0u || freq_hz == 0u) {
        TIM1->CCR1 = 0;
        pin(PUMP_EN_GPIO_Port, PUMP_EN_Pin, false);
        return;
    }
    if (duty > 1000u) duty = 1000u;
    if (freq_hz > 20000u) freq_hz = 20000u;
    uint32_t arr = 1000000u / freq_hz;                          /* 1 MHz counter */
    TIM1->ARR = arr - 1u;
    TIM1->CCR1 = (uint32_t)((uint64_t)arr * duty / 1000u);
    TIM1->CCER |= TIM_CCER_CC1E;
    TIM1->BDTR |= TIM_BDTR_MOE;
    TIM1->CR1 |= TIM_CR1_CEN;
    pin(PUMP_EN_GPIO_Port, PUMP_EN_Pin, true);
}

/* polled every pass (1 ms): fine for the tacho rates of the pumps in use (well under 500 Hz);
 * two edges between polls set the overcapture flag and that period is dropped */
uint32_t port_tach_period_us(void) {
    static uint32_t last_cap, last_ms, period;
    static bool have;
    uint32_t now = bsp_ms();
    if (!(TIM2->CR1 & TIM_CR1_CEN)) { TIM2->CCER |= TIM_CCER_CC1E; TIM2->CR1 |= TIM_CR1_CEN; }
    uint32_t sr = TIM2->SR;
    if (sr & TIM_SR_CC1IF) {
        uint32_t c = TIM2->CCR1;                                /* clears CC1IF */
        if (sr & TIM_SR_CC1OF) { TIM2->SR = ~TIM_SR_CC1OF; have = false; }
        else if (have) period = c - last_cap;
        last_cap = c; last_ms = now; have = true;
    }
    if ((uint32_t)(now - last_ms) > 2000u) { period = 0; have = false; }
    return period;
}

/* the node's address and serial number live in the backup words (plat_node.c); the drivers'
 * F-RAM store (nvcfg) is not used on this board */
int port_fram_read(uint8_t offset, uint8_t *buf, uint8_t n) { (void)offset; (void)buf; (void)n; return -1; }
int port_fram_write(uint8_t offset, const uint8_t *buf, uint8_t n) { (void)offset; (void)buf; (void)n; return -1; }

/* ---------------- plat_node.h hooks ---------------- */
static uint32_t gate_fb_bad;            /* passes in a row with GATE_EN set but GATE_FB low */

const node_hw_t *bsp_node_chips(void) {
    pin(CS_AI_GPIO_Port, CS_AI_Pin, true);                     /* the AD4112 stays off the bus */
    pin(GATE_EN_GPIO_Port, GATE_EN_Pin, false);
    port_relays(0);
    port_pwm(0, 0, false);
    return drivers_hw();
}

void bsp_node_pass(bool healthy, bool gate) {
    pin(GATE_EN_GPIO_Port, GATE_EN_Pin, gate);
    if (healthy) WDI_GPIO_Port->ODR ^= WDI_Pin;
    gate_fb_bad = (gate && !in(GATE_FB_GPIO_Port, GATE_FB_Pin)) ? gate_fb_bad + 1u : 0u;
}

/* the gate asked open but its read-back low for more than 50 passes (about 50 ms): the watchdog
 * or the AND gate has dropped the outputs (for the self-test and the service screen) */
bool bsp_node_gate_fault(void) { return gate_fb_bad > 50u; }
#endif
