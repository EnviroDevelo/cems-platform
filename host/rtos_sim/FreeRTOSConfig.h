/* FreeRTOSConfig.h - FreeRTOS POSIX port, for running plat_tasks_freertos.c on a PC.
 * Same task-relevant settings as port/stm32h5/FreeRTOSConfig.h. */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H
void plat_assert_failed(const char *file, int line);
#define configTICK_RATE_HZ                      1000
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configMAX_PRIORITIES                    16
#define configMINIMAL_STACK_SIZE                2048
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configUSE_MUTEXES                       1
#define configUSE_COUNTING_SEMAPHORES           1
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (256 * 1024)
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          0   /* not supported meaningfully on POSIX */
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_TRACE_FACILITY                1
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               11
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            configMINIMAL_STACK_SIZE
#define configASSERT(x) do { if ((x) == 0) plat_assert_failed(__FILE__, __LINE__); } while (0)
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_vTaskSuspend                    1
#include <limits.h>
#endif
