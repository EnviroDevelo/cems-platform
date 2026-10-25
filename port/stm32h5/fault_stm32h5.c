/* fault_stm32h5.c - CPU fault handlers (decision D4).
 *
 * HardFault, MemManage, BusFault and UsageFault all end here: outputs to the safe
 * state, the faulting PC, LR and fault registers into plat_diag (which survives the
 * reset), then a system reset. The next boot shows the fault as "bus @0801A2C4" on the
 * System screen and the web page and in the event log's power-up entry; look the PC up
 * in the build's .map or with arm-none-eabi-addr2line -e cems.elf 0x0801A2C4.
 *
 * CubeMX must not generate these four handlers in stm32h5xx_it.c (NVIC → Code
 * generation, docs/CUBEMX_SETUP.md section 4). */
#include "stm32h5xx.h"
#include "bsp.h"
#include "plat_diag.h"
#include "FreeRTOS.h"
#include "task.h"

/* RAM the stacked frame may be in (SRAM1-3, 640 KB); anything else is not read */
#define FRAME_RAM_START 0x20000000u
#define FRAME_RAM_END   0x200A0000u
#define CFSR_STKOF      (1u << 20)

void plat_cpu_fault(const uint32_t *frame);

void plat_cpu_fault(const uint32_t *frame) {
    bsp_io_safe_state();                /* the main board's pins first; the I/O boards drop theirs after their hold time */
    uint32_t cfsr = SCB->CFSR, pc = 0, lr = 0;
    uintptr_t f = (uintptr_t)frame;
    /* a stack-limit fault leaves no usable frame; otherwise r0-r3, r12, lr, pc, xpsr */
    if (!(cfsr & CFSR_STKOF) && f >= FRAME_RAM_START && f + 32u <= FRAME_RAM_END && (f & 3u) == 0u) {
        lr = frame[5];
        pc = frame[6];
    }
    /* The CM33 port gives every task a stack limit (PSPLIM), so an overflow faults here
     * before it corrupts anything, and the task's control block is still sound to read. */
    const char *task = (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) ? pcTaskGetName(NULL) : NULL;
    plat_diag_cpu_fault(pc, lr, cfsr, SCB->HFSR, SCB->MMFAR, SCB->BFAR, task, bsp_ms());
    NVIC_SystemReset();
}

/* Pick the stack the CPU pushed the frame on (EXC_RETURN bit 2: 0 = MSP, 1 = PSP). */
__attribute__((naked)) void HardFault_Handler(void) {
    __asm volatile(
        "tst   lr, #4          \n"
        "ite   eq              \n"
        "mrseq r0, msp         \n"
        "mrsne r0, psp         \n"
        "b     plat_cpu_fault  \n");
}

void MemManage_Handler(void)  __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void)   __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));
