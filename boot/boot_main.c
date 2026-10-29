/* boot_main.c - the first-stage bootloader on the STM32H573 (boot.h, docs/BOOTLOADER.md).
 *
 * Runs from the first PLAT_BOOT_SIZE bytes of whichever bank is mapped at 0x08000000, on
 * the reset clock (HSI, 32 MHz after reset; nothing else is set up). It reads the trial
 * register, lets boot_decide() choose, and then starts the application, swaps banks, or
 * stops. Its RAM is the top of SRAM (boot.ld), clear of the application's .noinit block.
 *
 * Kept small on purpose: no RTOS, no clock tree, only the HAL flash (option bytes) and PKA
 * (signature) modules. Everything it decides is in boot_core.c and host-tested. */
#include <string.h>
#include "stm32h5xx_hal.h"
#include "boot.h"
#include "fw_update.h"

#define OTHER_BASE (FLASH_BASE + FLASH_BANK_SIZE)

/* ---------------- tick for the HAL's timeouts ---------------- */
void SysTick_Handler(void) { HAL_IncTick(); }

/* A double ECC error while reading flash (a write cut off half way) raises an NMI. Note
 * it and carry on: the read that hit it then fails, and the image counts as damaged.
 * **(check on the DK: corrupt a quad-word in the free bank and watch it swap/halt)** */
static volatile bool ecc_fault;
void NMI_Handler(void) {
    if (FLASH->ECCDETR & FLASH_ECCR_ECCD) { FLASH->ECCDETR = FLASH_ECCR_ECCD; ecc_fault = true; return; }
    for (;;) { }
}

static int rd(boot_io_t *io, bool mapped, uint32_t off, void *p, uint32_t n) {
    if (off > io->bank_size || n > io->bank_size - off) return -1;
    ecc_fault = false;
    memcpy(p, (const void *)((mapped ? FLASH_BASE : OTHER_BASE) + off), n);
    return ecc_fault ? -1 : 0;
}

/* ---------------- signature: the PKA, so mbedTLS stays out of the 32 KB ---------------- */
static const uint8_t p256_p[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
static const uint8_t p256_n[32] = {
    0xFF,0xFF,0xFF,0xFF,0x00,0x00,0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xBC,0xE6,0xFA,0xAD,0xA7,0x17,0x9E,0x84,0xF3,0xB9,0xCA,0xC2,0xFC,0x63,0x25,0x51 };
static const uint8_t p256_gx[32] = {
    0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,
    0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96 };
static const uint8_t p256_gy[32] = {
    0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,
    0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5 };
static const uint8_t p256_a_abs[32] = { [31] = 3 };   /* a = -3 */

void HAL_PKA_MspInit(PKA_HandleTypeDef *h)   { (void)h; }    /* clocks: below */
void HAL_PKA_MspDeInit(PKA_HandleTypeDef *h) { (void)h; }

static bool pka_verify(const uint8_t hash[32], const uint8_t sig[64]) {
    static PKA_HandleTypeDef pka;
    RCC->CR |= RCC_CR_HSI48ON;                           /* the RNG's clock: the PKA needs the RNG running */
    while (!(RCC->CR & RCC_CR_HSI48RDY)) { }
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN | RCC_AHB2ENR_PKAEN;
    (void)RCC->AHB2ENR;
    RNG->CR |= RNG_CR_RNGEN;
    pka.Instance = PKA;
    if (HAL_PKA_Init(&pka) != HAL_OK) return false;
    PKA_ECDSAVerifInTypeDef in = {
        .primeOrderSize = 32, .modulusSize = 32, .coefSign = 1, .coef = p256_a_abs,
        .modulus = p256_p, .basePointX = p256_gx, .basePointY = p256_gy,
        .pPubKeyCurvePtX = fw_pubkey + 1, .pPubKeyCurvePtY = fw_pubkey + 33,
        .RSign = sig, .SSign = sig + 32, .hash = hash, .primeOrder = p256_n };
    bool ok = HAL_PKA_ECDSAVerif(&pka, &in, 1000) == HAL_OK && HAL_PKA_ECDSAVerif_IsValidSignature(&pka);
    (void)HAL_PKA_DeInit(&pka);
    return ok;
}

/* ---------------- the three outcomes ---------------- */
static void quiesce(void) {                              /* leave the chip as reset left it */
    SysTick->CTRL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    RCC->AHB2RSTR |= RCC_AHB2RSTR_RNGRST | RCC_AHB2RSTR_PKARST;
    RCC->AHB2RSTR &= ~(RCC_AHB2RSTR_RNGRST | RCC_AHB2RSTR_PKARST);
    RCC->AHB2ENR &= ~(RCC_AHB2ENR_RNGEN | RCC_AHB2ENR_PKAEN);
    RCC->CR &= ~RCC_CR_HSI48ON;
    ICACHE->CR &= ~ICACHE_CR_EN;                         /* the application sets the cache up itself */
    while (ICACHE->SR & ICACHE_SR_BUSYF) { }
}

static void __attribute__((noreturn)) start_app(void) {
    const uint32_t *v = (const uint32_t *)(FLASH_BASE + PLAT_BOOT_SIZE);
    quiesce();
    __disable_irq();
    SCB->VTOR = (uint32_t)(uintptr_t)v;                  /* (its SystemInit sets it again: VECT_TAB_OFFSET) */
    __set_MSPLIM(0);
    __DSB(); __ISB();
    __enable_irq();                                      /* as after a reset */
    __asm volatile("msr msp, %0\n bx %1" : : "r"(v[0]), "r"(v[1]));
    for (;;) { }
}

static void __attribute__((noreturn)) swap_banks(void) {   /* as fw_flash_stm32h5.c fl_swap() */
    bool swapped = (FLASH->OPTSR_CUR & FLASH_OPTSR_SWAP_BANK) != 0u;
    FLASH_OBProgramInitTypeDef ob = { 0 };
    ob.OptionType = OPTIONBYTE_USER;
    ob.USERType = OB_USER_SWAP_BANK;
    ob.USERConfig = swapped ? OB_SWAP_BANK_DISABLE : OB_SWAP_BANK_ENABLE;
    HAL_FLASH_Unlock();
    HAL_FLASH_OB_Unlock();
    if (HAL_FLASHEx_OBProgram(&ob) == HAL_OK) (void)HAL_FLASH_OB_Launch();
    NVIC_SystemReset();
}

static void __attribute__((noreturn)) halt(void) {
    /* nothing to start. Recovery: SWD, or the ST ROM bootloader (BOOT0 high). */
    quiesce();
    for (;;) __WFI();
}

int main(void) {
    SystemCoreClockUpdate();                             /* HSI / 2 after reset */
    SysTick_Config(SystemCoreClock / 1000u);
    ICACHE->CR |= ICACHE_CR_EN;                          /* about 3x faster hashing from flash */
    RCC->APB3ENR |= RCC_APB3ENR_RTCAPBEN;                /* the TAMP backup registers */
    (void)RCC->APB3ENR;
    PWR->DBPCR |= PWR_DBPCR_DBP;

    boot_io_t io = { FLASH_BANK_SIZE, FLASH_SECTOR_SIZE, PLAT_BOOT_SIZE, SRAM1_BASE, SRAM1_BASE + 0xA0000u,
                     rd, fw_pubkey_present ? pka_verify : NULL, NULL };
    uint32_t trial = BOOT_TRIAL_REG;
    boot_report_t rep;
    boot_action_t a = boot_decide(&io, &trial, &rep);
    BOOT_TRIAL_REG = trial;
    if (a == BOOT_JUMP) start_app();
    if (a == BOOT_SWAP) swap_banks();
    halt();
}
