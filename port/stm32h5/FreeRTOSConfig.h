/* FreeRTOSConfig.h - reference configuration for the CEMS platform on STM32H5
 * (FreeRTOS Kernel V11.2.0, port GCC/ARM_CM33_NTZ, TrustZone disabled).
 *
 * If X-CUBE-FREERTOS generates its own FreeRTOSConfig.h, make the values below
 * match it (docs/CUBEMX_SETUP.md lists them as CubeMX settings). The platform
 * depends on the ones marked REQUIRED. */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>
extern uint32_t SystemCoreClock;
void plat_assert_failed(const char *file, int line);

/* ---- core / port ---- */
#define configENABLE_TRUSTZONE                  0
#define configRUN_FREERTOS_SECURE_ONLY          0
#define configENABLE_FPU                        1
#define configENABLE_MPU                        0
#define configENABLE_MVE                        0

#define configCPU_CLOCK_HZ                      (SystemCoreClock)
#define configTICK_RATE_HZ                      1000      /* REQUIRED: 1 ms tick (ctrl task, pdMS_TO_TICKS) */
#define configUSE_PREEMPTION                    1         /* REQUIRED */
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
#define configMAX_PRIORITIES                    16        /* REQUIRED: plat_config.h uses up to 14 */
#define configMINIMAL_STACK_SIZE                256
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   2
#define configQUEUE_REGISTRY_SIZE               8
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_NEWLIB_REENTRANT              0
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

/* ---- memory ---- */
#define configSUPPORT_STATIC_ALLOCATION         1         /* REQUIRED: platform tasks are static */
#define configSUPPORT_DYNAMIC_ALLOCATION        1         /* lwIP sys_arch creates its own objects */
#define configTOTAL_HEAP_SIZE                   (48 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0

/* ---- hooks and checks ---- */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2         /* REQUIRED: plat_tasks hooks record the task */
#define configUSE_MALLOC_FAILED_HOOK            1         /* REQUIRED */
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
#define configUSE_TRACE_FACILITY                1
#define configGENERATE_RUN_TIME_STATS           0
#define configRECORD_STACK_HIGH_ADDRESS         1
#define configASSERT(x) do { if ((x) == 0) plat_assert_failed(__FILE__, __LINE__); } while (0)

/* ---- software timers ---- */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               11        /* below the supervisor, above store */
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            512

/* ---- interrupts (STM32H5: 4 priority bits) ----
 * ISRs that call FreeRTOS API must have priority number >= 5 (numerically).
 * Priorities 0-4 are never masked by the kernel: use them only for code that
 * makes no RTOS call (e.g. a hardware control-loop timer). */
#define configPRIO_BITS                         4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY         (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* ---- API inclusion ---- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1         /* REQUIRED */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTimerPendFunctionCall          1

/* The ARM_CM33_NTZ port defines SVC_Handler, PendSV_Handler and SysTick_Handler
 * itself: CubeMX must NOT generate them in stm32h5xx_it.c, and the HAL timebase
 * must be a TIM (e.g. TIM6), not SysTick. */

#endif
