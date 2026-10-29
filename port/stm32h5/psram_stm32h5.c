/* psram_stm32h5.c - the CORE PCB's external PSRAM: AP Memory APS6404L-3SQR (64 Mbit, 8 MB,
 * QSPI, 3.3 V) on OCTOSPI1, memory-mapped at 0x90000000 (docs/CORE_PCB.md, docs/MEMORY.md).
 *
 * Built in only with PLAT_PSRAM=1 (plat_config.h), for boards that have the chip: the DK has
 * none, so its builds keep every buffer in the MCU's RAM. Uses the CubeMX handle named by
 * PSRAM_XSPI_HANDLE in main.h (hospi1: OCTOSPI1, pins PB2 CLK, PB10 NCS, PB1/PB0/PC2/PA6 IO0-3;
 * docs/CUBEMX_SETUP.md); the settings below replace whatever the .ioc set.
 *
 * What lives there: buffers marked PLAT_EXTRAM (section .extram, NOLOAD): the TLS pool and the
 * settings store's staging block. Never stacks, never anything an interrupt or DMA touches, and
 * nothing used before bsp_init. The chip has no ECC, so bsp_psram_ok() is checked at start-up
 * (address lines and data lines) and a failure stops the unit with outputs safe ("psram").
 *
 * Timing (APS6404L datasheet): SPI mode after power-up; 0xEB quad read (6 wait cycles), 0x38
 * quad write; CE# low at most 8 us (tCEM), so the controller releases it every 400 clocks; a
 * burst may not cross a 1 KB page above 84 MHz, so the chip select is also cut at each 1 KB.
 * Clock 250 MHz / 4 = 62.5 MHz. DCACHE1, when the .ioc enables it, caches this region. */
#include "main.h"
#include "bsp.h"
#include "plat_config.h"
#include <string.h>

#if PLAT_PSRAM && !defined(PLAT_HOST)
#ifndef PSRAM_XSPI_HANDLE
#error "PLAT_PSRAM=1 needs PSRAM_XSPI_HANDLE (the OCTOSPI1 handle, hospi1) in main.h"
#endif
extern XSPI_HandleTypeDef PSRAM_XSPI_HANDLE;
#ifdef HAL_DCACHE_MODULE_ENABLED
extern DCACHE_HandleTypeDef hdcache1;
#endif

#define PSRAM_BASE   0x90000000u
#define PSRAM_SIZE   (8u * 1024u * 1024u)
#define CMD_RST_EN   0x66u
#define CMD_RST      0x99u
#define CMD_READ_ID  0x9Fu
#define CMD_QREAD    0xEBu
#define CMD_QWRITE   0x38u
#define KGD_PASS     0x5Du

extern uint8_t _sextram[], _eextram[];   /* linker: the .extram section */

static bool psram_ok;

static HAL_StatusTypeDef cmd(uint8_t instr, bool addr, uint32_t data_len, uint8_t *rx) {
    XSPI_RegularCmdTypeDef c = { 0 };
    c.OperationType = HAL_XSPI_OPTYPE_COMMON_CFG;
    c.IOSelect = HAL_XSPI_SELECT_IO_3_0;
    c.Instruction = instr;
    c.InstructionMode = HAL_XSPI_INSTRUCTION_1_LINE;
    c.InstructionWidth = HAL_XSPI_INSTRUCTION_8_BITS;
    c.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
    c.AddressMode = addr ? HAL_XSPI_ADDRESS_1_LINE : HAL_XSPI_ADDRESS_NONE;
    c.AddressWidth = HAL_XSPI_ADDRESS_24_BITS;
    c.AddressDTRMode = HAL_XSPI_ADDRESS_DTR_DISABLE;
    c.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
    c.DataMode = data_len ? HAL_XSPI_DATA_1_LINE : HAL_XSPI_DATA_NONE;
    c.DataLength = data_len;
    c.DataDTRMode = HAL_XSPI_DATA_DTR_DISABLE;
    c.DQSMode = HAL_XSPI_DQS_DISABLE;
    HAL_StatusTypeDef r = HAL_XSPI_Command(&PSRAM_XSPI_HANDLE, &c, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
    if (r == HAL_OK && data_len) r = HAL_XSPI_Receive(&PSRAM_XSPI_HANDLE, rx, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
    return r;
}

static HAL_StatusTypeDef mapped_cfg(uint32_t optype, uint8_t instr, uint32_t dummy) {
    XSPI_RegularCmdTypeDef c = { 0 };
    c.OperationType = optype;
    c.IOSelect = HAL_XSPI_SELECT_IO_3_0;
    c.Instruction = instr;
    c.InstructionMode = HAL_XSPI_INSTRUCTION_1_LINE;
    c.InstructionWidth = HAL_XSPI_INSTRUCTION_8_BITS;
    c.InstructionDTRMode = HAL_XSPI_INSTRUCTION_DTR_DISABLE;
    c.AddressMode = HAL_XSPI_ADDRESS_4_LINES;
    c.AddressWidth = HAL_XSPI_ADDRESS_24_BITS;
    c.AddressDTRMode = HAL_XSPI_ADDRESS_DTR_DISABLE;
    c.AlternateBytesMode = HAL_XSPI_ALT_BYTES_NONE;
    c.DataMode = HAL_XSPI_DATA_4_LINES;
    c.DataDTRMode = HAL_XSPI_DATA_DTR_DISABLE;
    c.DummyCycles = dummy;
    c.DQSMode = HAL_XSPI_DQS_DISABLE;
    return HAL_XSPI_Command(&PSRAM_XSPI_HANDLE, &c, HAL_XSPI_TIMEOUT_DEFAULT_VALUE);
}

/* Data lines (walking ones at the base) and address lines (a distinct word at each power of
 * two), every word read back after all are written, then the complement: a stuck, shorted or
 * open line fails. The chip's contents are not needed: nothing is placed there yet. */
static bool mem_test(void) {
    volatile uint32_t *m = (volatile uint32_t *)PSRAM_BASE;
    for (uint32_t b = 0; b < 32u; ++b) {
        m[0] = 1u << b;
        __DSB();
        if (m[0] != (1u << b)) return false;
    }
    for (uint32_t pass = 0; pass < 2u; ++pass) {
        uint32_t inv = pass ? 0xFFFFFFFFu : 0u, k = 0;
        m[0] = 0xA5A50000u ^ inv;
        for (uint32_t off = 4u; off < PSRAM_SIZE; off <<= 1, ++k) m[off / 4u] = (0x5A5A0000u + k) ^ inv;
        __DSB();
        if (m[0] != (0xA5A50000u ^ inv)) return false;
        k = 0;
        for (uint32_t off = 4u; off < PSRAM_SIZE; off <<= 1, ++k) if (m[off / 4u] != ((0x5A5A0000u + k) ^ inv)) return false;
    }
    return true;
}

static bool psram_start(void) {
    XSPI_HandleTypeDef *h = &PSRAM_XSPI_HANDLE;
    (void)HAL_XSPI_DeInit(h);
    h->Init.FifoThresholdByte = 4;
    h->Init.MemoryMode = HAL_XSPI_SINGLE_MEM;
    h->Init.MemoryType = HAL_XSPI_MEMTYPE_MICRON;          /* plain SPI command set */
    h->Init.MemorySize = HAL_XSPI_SIZE_64MB;                /* 64 Mbit */
    h->Init.ChipSelectHighTimeCycle = 2;                    /* tCPH 18 ns at 62.5 MHz */
    h->Init.FreeRunningClock = HAL_XSPI_FREERUNCLK_DISABLE;
    h->Init.ClockMode = HAL_XSPI_CLOCK_MODE_0;
    h->Init.WrapSize = HAL_XSPI_WRAP_NOT_SUPPORTED;
    h->Init.ClockPrescaler = 3;                             /* 250 MHz / 4 */
    h->Init.SampleShifting = HAL_XSPI_SAMPLE_SHIFT_NONE;
    h->Init.DelayHoldQuarterCycle = HAL_XSPI_DHQC_DISABLE;
    h->Init.ChipSelectBoundary = HAL_XSPI_BONDARYOF_8KB;    /* 8 Kbit = the 1 KB page */
    h->Init.DelayBlockBypass = HAL_XSPI_DELAY_BLOCK_BYPASS;
    h->Init.Refresh = 400;                                  /* CE# low under 6.4 us (tCEM 8 us) */
    if (HAL_XSPI_Init(h) != HAL_OK) return false;
    if (cmd(CMD_RST_EN, false, 0, NULL) != HAL_OK || cmd(CMD_RST, false, 0, NULL) != HAL_OK) return false;
    for (volatile uint32_t d = 0; d < 1000u; ++d) { }      /* tRST 50 ns, generously */
    uint8_t id[2] = { 0, 0 };
    if (cmd(CMD_READ_ID, true, 2, id) != HAL_OK || id[1] != KGD_PASS) return false;
    if (mapped_cfg(HAL_XSPI_OPTYPE_READ_CFG, CMD_QREAD, 6) != HAL_OK ||
        mapped_cfg(HAL_XSPI_OPTYPE_WRITE_CFG, CMD_QWRITE, 0) != HAL_OK) return false;
    XSPI_MemoryMappedTypeDef mm = { .TimeOutActivation = HAL_XSPI_TIMEOUT_COUNTER_ENABLE, .TimeoutPeriodClock = 0x20 };
    if (HAL_XSPI_MemoryMapped(h, &mm) != HAL_OK) return false;
#ifdef HAL_DCACHE_MODULE_ENABLED
    if (hdcache1.Instance) (void)HAL_DCACHE_Enable(&hdcache1);
#endif
    if (!mem_test()) return false;
    memset(_sextram, 0, (size_t)(_eextram - _sextram));     /* like .bss: the startup code skips NOLOAD */
    return true;
}

void bsp_psram_init(void) { psram_ok = psram_start(); }
bool bsp_psram_ok(void)   { return psram_ok; }
#else
void bsp_psram_init(void) { }
bool bsp_psram_ok(void)   { return true; }     /* no PSRAM in this build: nothing placed there */
#endif
