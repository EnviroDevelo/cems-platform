/* plat_net.h - the socket API the servers use: POSIX on the PC, lwIP on the H5.
 * lwIP must be built with LWIP_SOCKET=1, LWIP_COMPAT_SOCKETS=1,
 * LWIP_SO_RCVTIMEO=1, LWIP_SO_SNDTIMEO=1 and SO_REUSE=1 (port/stm32h5/lwipopts.h). */
#ifndef PLAT_NET_H
#define PLAT_NET_H

#if defined(PLAT_HOST) || defined(PLAT_POSIX_SOCKETS)   /* PC builds, incl. the FreeRTOS POSIX run */
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  #include <fcntl.h>
  #define closesocket(s) close(s)
#else
  #include "lwip/sockets.h"
  #include "lwip/inet.h"
#endif

#endif
