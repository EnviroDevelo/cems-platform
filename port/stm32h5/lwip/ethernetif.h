/* ethernetif.h - STM32H5 Ethernet driver for lwIP (see ethernetif.c). */
#ifndef ETHERNETIF_H
#define ETHERNETIF_H
#include <stdint.h>
#include "lwip/err.h"
#include "lwip/netif.h"
void  ethernetif_set_mac(const uint8_t mac[6]);   /* before netif_add */
err_t ethernetif_init(struct netif *netif);
void  ethernetif_link_poll(struct netif *netif);   /* from a task, every few hundred ms */
void  ethernetif_set_link_mode(unsigned mode);      /* the same task: net_link_mode (param.h) */
#endif
