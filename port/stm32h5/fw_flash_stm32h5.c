/* fw_flash_stm32h5.c - the two flash banks of the STM32H573 for fw_update.
 *
 * The running image is always mapped at FLASH_BASE (0x08000000) and the other bank
 * at FLASH_BASE + FLASH_BANK_SIZE, whichever physical bank each one is: the
 * SWAP_BANK option bit exchanges the mapping at reset. So the free bank is always
 * written at the upper address, and its physical number (for the sector-erase
 * register) is bank 2 when not swapped, bank 1 when swapped, as in ST's
 * FLASH_SwapBank example. **(check on the DK with the debugger attached before the
 * first field update)**
 *
 * Program unit: 16 bytes (quad-word, with ECC: once per erase). Erase unit: 8 KB.
 * Writing the free bank does not stall execution from the running bank (read-while-
 * write across banks). The instruction cache also caches data reads from flash, so
 * it is invalidated after every erase and write.
 *
 * Behind the bootloader (boot.h) the image starts at image_off, taken from where this
 * build's vector table was linked (0 = no bootloader, the image at FLASH_BASE), and the
 * trial mark goes into the backup register the bootloader reads (BOOT_TRIAL_REG). The
 * RTC set-up has already enabled writes to the backup domain. */
#include <string.h>
#include "main.h"
#include "fw_update.h"
#include "boot.h"

#if defined(HAL_FLASH_MODULE_ENABLED)

#define FREE_BASE (FLASH_BASE + FLASH_BANK_SIZE)   /* (FLASH_BANK_SIZE reads the flash-size register) */

static bool swapped(void) { return (FLASH->OPTSR_CUR & FLASH_OPTSR_SWAP_BANK) != 0u; }

static void cache_invalidate(void) {
#if defined(HAL_ICACHE_MODULE_ENABLED)
    (void)HAL_ICACHE_Invalidate();
#endif
}

static int fl_erase(fw_flash_t *f, uint32_t off) {
    if (off >= f->bank_size || off % f->sector) return -1;
    FLASH_EraseInitTypeDef e = { 0 };
    e.TypeErase = FLASH_TYPEERASE_SECTORS;
    e.Banks = swapped() ? FLASH_BANK_1 : FLASH_BANK_2;
    e.Sector = off / f->sector;
    e.NbSectors = 1;
    uint32_t bad = 0;
    HAL_FLASH_Unlock();
    HAL_StatusTypeDef r = HAL_FLASHEx_Erase(&e, &bad);
    HAL_FLASH_Lock();
    cache_invalidate();
    if (r != HAL_OK) return -1;
    const uint32_t *p = (const uint32_t *)(FREE_BASE + off);     /* the right bank was erased? */
    for (uint32_t k = 0; k < f->sector / 4u; k += 64u) if (p[k] != 0xFFFFFFFFu) return -1;
    return 0;
}

static int fl_write(fw_flash_t *f, uint32_t off, const void *src, uint32_t n) {
    if (off > f->bank_size || n > f->bank_size - off || off % 16u || n % 16u) return -1;
    int rc = 0;
    HAL_FLASH_Unlock();
    for (uint32_t k = 0; k < n && rc == 0; k += 16u) {
        uint32_t q[4];
        memcpy(q, (const uint8_t *)src + k, 16);            /* aligned copy for the HAL */
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD, FREE_BASE + off + k, (uint32_t)(uintptr_t)q) != HAL_OK) rc = -1;
    }
    HAL_FLASH_Lock();
    cache_invalidate();
    return rc;
}

static int fl_read(fw_flash_t *f, bool active, uint32_t off, void *dst, uint32_t n) {
    if (off > f->bank_size || n > f->bank_size - off) return -1;
    memcpy(dst, (const void *)((active ? FLASH_BASE : FREE_BASE) + off), n);
    return 0;
}

static void fl_swap(fw_flash_t *f) {
    (void)f;
    FLASH_OBProgramInitTypeDef ob = { 0 };
    ob.OptionType = OPTIONBYTE_USER;
    ob.USERType = OB_USER_SWAP_BANK;
    ob.USERConfig = swapped() ? OB_SWAP_BANK_DISABLE : OB_SWAP_BANK_ENABLE;
    __disable_irq();
    HAL_FLASH_Unlock();
    HAL_FLASH_OB_Unlock();
    if (HAL_FLASHEx_OBProgram(&ob) == HAL_OK) (void)HAL_FLASH_OB_Launch();
    NVIC_SystemReset();                                   /* the new mapping applies from reset */
}

static void fl_trial(fw_flash_t *f, bool on) {
    (void)f;
    BOOT_TRIAL_REG = on ? BOOT_TRIAL_MAGIC : 0u;
}

extern const uint32_t g_pfnVectors[];     /* the startup file's vector table */

fw_flash_t *fw_flash_stm32h5(void) {
    static fw_flash_t f = { .sector = FLASH_SECTOR_SIZE, .prog_unit = 16u,
                            .erase = fl_erase, .write = fl_write, .read = fl_read, .swap_and_reset = fl_swap,
                            .trial_mark = fl_trial };
    f.bank_size = FLASH_BANK_SIZE;        /* from the flash-size register: 1 MB on the H573 */
    f.image_off = (uint32_t)(uintptr_t)g_pfnVectors - FLASH_BASE;   /* PLAT_BOOT_SIZE, or 0 */
    if (f.image_off) {                    /* behind the bootloader: what it found at this start */
        f.boot_report = BOOT_REPORT_REG;
        BOOT_REPORT_REG = 0u;             /* (a stale one must not be shown after a debugger load) */
    }
    return &f;
}

#else
fw_flash_t *fw_flash_stm32h5(void) { return NULL; }       /* HAL flash module not enabled */
#endif
