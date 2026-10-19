/* plat_port.h - the few CPU/OS primitives the portable core needs.
 * Target (default): FreeRTOS critical sections and CMSIS barriers.
 * Host (-DPLAT_HOST): single-threaded tests, compiler fences only. */
#ifndef PLAT_PORT_H
#define PLAT_PORT_H

#if defined(PLAT_HOST) && defined(PLAT_HOST_THREADS)
  /* threaded PC simulation: one global lock stands in for "interrupts off" */
  void plat_host_crit_enter(void);
  void plat_host_crit_exit(void);
  #define PLAT_CRIT_ENTER()  plat_host_crit_enter()
  #define PLAT_CRIT_EXIT()   plat_host_crit_exit()
  #define PLAT_DMB()         __atomic_thread_fence(__ATOMIC_SEQ_CST)
#elif defined(PLAT_HOST)
  #define PLAT_CRIT_ENTER()  do { } while (0)
  #define PLAT_CRIT_EXIT()   do { } while (0)
  #define PLAT_DMB()         __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
  #include "FreeRTOS.h"
  #include "task.h"
  #define PLAT_CRIT_ENTER()  taskENTER_CRITICAL()
  #define PLAT_CRIT_EXIT()   taskEXIT_CRITICAL()
  #if defined(__ARM_ARCH)
    #define PLAT_DMB()       __asm volatile ("dmb" ::: "memory")
  #else                              /* FreeRTOS POSIX port on a PC */
    #define PLAT_DMB()       __atomic_thread_fence(__ATOMIC_SEQ_CST)
  #endif
#endif

#endif
