/* cc.h - lwIP compiler/platform glue for arm-none-eabi-gcc + newlib. */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#define LWIP_ERRNO_STDINCLUDE   1          /* use newlib's errno.h */
#define LWIP_NO_UNISTD_H        0
#define BYTE_ORDER              LITTLE_ENDIAN

void plat_assert_failed(const char *file, int line);
#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) plat_assert_failed(__FILE__, __LINE__)

/* random numbers for TCP initial sequence numbers, DHCP transaction IDs, ports */
uint32_t plat_lwip_rand(void);
#define LWIP_RAND()             plat_lwip_rand()

#endif
