/* lwipopts.h - lwIP 2.2.1 configuration for the CEMS platform on STM32H5 with
 * native FreeRTOS (sys_arch from lwip/contrib/ports/freertos).
 * Sized for the spec: one Ethernet port, Modbus TCP (2-4 clients) and the web
 * server (3-4 browsers) at the same time. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#include "plat_config.h"

/* ---- OS ---- */
#define NO_SYS                          0
#define SYS_LIGHTWEIGHT_PROT            1
#define LWIP_TCPIP_CORE_LOCKING         1
#define LWIP_FREERTOS_SYS_NOW_FROM_FREERTOS 1   /* sys_now() = FreeRTOS tick (1 kHz) */
#define TCPIP_THREAD_NAME               "tcpip"
#define TCPIP_THREAD_STACKSIZE          1024    /* words (FreeRTOS units) */
#define LWIP_FREERTOS_THREAD_STACKSIZE_IS_STACKWORDS 1
#define TCPIP_THREAD_PRIO               PLAT_PRIO_TCPIP
#define TCPIP_MBOX_SIZE                 16
#define DEFAULT_THREAD_STACKSIZE        512
#define DEFAULT_THREAD_PRIO             PLAT_PRIO_TCPIP
#define DEFAULT_TCP_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_ACCEPTMBOX_SIZE         4

/* ---- memory ---- */
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (16 * 1024)   /* TX data (zero-copy RX has its own pool) */
#define MEMP_NUM_PBUF                   16
#define PBUF_POOL_SIZE                  0             /* unused: received frames are RX_POOL custom pbufs (ethernetif.c),
                                                         and nothing else in lwIP or the platform allocates PBUF_POOL
                                                         (lwIP 2.2.1 core/api checked, 29 Oct). Was 8 = 12 KB idle */
#define PBUF_POOL_BUFSIZE               1536
#define LWIP_SUPPORT_CUSTOM_PBUF        1             /* zero-copy RX in ethernetif.c */
#define MEMP_NUM_TCP_PCB                16            /* 4 Modbus + 5 HTTP + 3 I/O boards + mail + spare */
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_NETCONN                20            /* + 3 I/O board connections and the discovery socket */
#define MEMP_NUM_TCP_SEG                TCP_SND_QUEUELEN
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)

/* ---- protocols ---- */
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ICMP                       1             /* ping */
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0
#define LWIP_ACD                        1   /* duplicate-address check for static addresses and DHCP leases */
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DNS                        0
#define LWIP_IGMP                       0
#define TCP_MSS                         1460
#define TCP_WND                         (4 * TCP_MSS)
#define TCP_SND_BUF                     (4 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define TCP_LISTEN_BACKLOG              1
/* first retransmit after 1 s, not lwIP's 3 s (RFC 6298's value): on a lossy link a lost SYN or a
 * lost first segment costs a second, not three, before round-trip samples take over */
#define LWIP_TCP_RTO_TIME               1000
#define LWIP_TCP_KEEPALIVE              1
/* a PLC that disappears without closing is noticed in about 50 s */
#define TCP_KEEPIDLE_DEFAULT            30000
#define TCP_KEEPINTVL_DEFAULT           5000
#define TCP_KEEPCNT_DEFAULT             4

/* ---- APIs: BSD sockets for the servers (platform/include/plat_net.h) ---- */
#define LWIP_NETCONN                    1
#define LWIP_SOCKET                     1
#define LWIP_COMPAT_SOCKETS             1             /* socket(), bind(), select() ... */
#define LWIP_POSIX_SOCKETS_IO_NAMES     0             /* keep close()/read() for files; servers use closesocket() */
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SOCKET_SELECT              1
#define SO_REUSE                        1
#define LWIP_NETIF_API                  1
#define LWIP_TIMEVAL_PRIVATE            0             /* struct timeval from newlib <sys/time.h> */

/* ---- interface ---- */
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define ETHARP_SUPPORT_STATIC_ENTRIES   0

/* ---- checksums: the H5 Ethernet MAC computes them (ethernetif.c sets TX insertion) ---- */
#define CHECKSUM_GEN_IP                 0
#define CHECKSUM_GEN_UDP                0
#define CHECKSUM_GEN_TCP                0
#define CHECKSUM_GEN_ICMP               1             /* not inserted by hardware for fragments */
#define CHECKSUM_CHECK_IP               0
#define CHECKSUM_CHECK_UDP              0
#define CHECKSUM_CHECK_TCP              0
#define CHECKSUM_CHECK_ICMP             0

/* ---- hooks ---- */
#define LWIP_HOOK_FILENAME              "lwip_hooks.h"   /* TCP retransmission count (ethernetif.c) */

/* ---- diagnostics ---- */
#define LWIP_STATS                      0
#define LWIP_DEBUG                      0

#endif
