/* lwip_hooks.h - lwIP hooks for the STM32H5 port (LWIP_HOOK_FILENAME in lwipopts.h). */
#ifndef LWIP_HOOKS_H
#define LWIP_HOOKS_H
#include "lwip/arch.h"
struct pbuf;
struct tcp_hdr;
struct tcp_pcb;
/* Counts TCP data segments and retransmissions for the automatic 10 Mbit (ethernetif.c). */
u32_t *ethernetif_tcp_out(struct pbuf *p, struct tcp_hdr *hdr, const struct tcp_pcb *pcb, u32_t *opts);
#define LWIP_HOOK_TCP_OUT_ADD_TCPOPTS(p, hdr, pcb, opts) ethernetif_tcp_out((p), (hdr), (pcb), (opts))
#endif
