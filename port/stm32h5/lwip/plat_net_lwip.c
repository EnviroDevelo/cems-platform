/* plat_net_lwip.c - network bring-up and upkeep on the STM32H5 (net task).
 *
 *  - starts lwIP (tcpip thread) and the Ethernet interface;
 *  - MAC address: locally administered (02:xx:..), derived from the chip's
 *    96-bit unique ID, so every unit differs and keeps its address (DECISION
 *    D10: a registered OUI or an EUI-48 chip for production);
 *  - addresses from the parameter table (net_dhcp, net_ip, net_mask, net_gw);
 *    a change made from the touchscreen or web is applied live;
 *  - polls the PHY link every 250 ms. */
#include "lwip/tcpip.h"
#include "lwip/netifapi.h"
#include "lwip/dhcp.h"
#include "stm32h5xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ethernetif.h"
#include "plat_tasks.h"
#include "param.h"
#include "nvm.h"
#include "bsp.h"

static struct netif nif;
static volatile bool ready;
static volatile bool tcpip_up;

bool plat_net_ready(void) { return ready; }
uint32_t plat_lwip_rand(void) { uint32_t r; bsp_random(&r, sizeof r); return r; }

void plat_net_mac(uint8_t m[6]) {
    uint32_t a = plat_crc32(0, (const void *)UID_BASE, 12);
    uint32_t b = plat_crc32(a, "CEMS", 4);
    m[0] = 0x02;                                   /* locally administered, unicast */
    m[1] = (uint8_t)(a >> 24); m[2] = (uint8_t)(a >> 16); m[3] = (uint8_t)(a >> 8);
    m[4] = (uint8_t)a ^ (uint8_t)(b >> 24); m[5] = (uint8_t)b;
}

typedef struct { bool dhcp; uint32_t ip, mask, gw; } net_cfg_t;

static net_cfg_t read_cfg(void) {
    plat_val_t v; net_cfg_t c = { 0 };
    if (param_read(PARAM_ID_NET_DHCP, &v)) c.dhcp = v.b;
    if (param_read(PARAM_ID_NET_IP, &v))   c.ip = v.u;
    if (param_read(PARAM_ID_NET_MASK, &v)) c.mask = v.u;
    if (param_read(PARAM_ID_NET_GW, &v))   c.gw = v.u;
    return c;
}

static void apply_cfg(const net_cfg_t *c) {
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, c->ip >> 24, (c->ip >> 16) & 255, (c->ip >> 8) & 255, c->ip & 255);
    IP4_ADDR(&mask, c->mask >> 24, (c->mask >> 16) & 255, (c->mask >> 8) & 255, c->mask & 255);
    IP4_ADDR(&gw, c->gw >> 24, (c->gw >> 16) & 255, (c->gw >> 8) & 255, c->gw & 255);
    if (c->dhcp) {
        netifapi_dhcp_start(&nif);
    } else {
        netifapi_dhcp_release_and_stop(&nif);
        netifapi_netif_set_addr(&nif, &ip, &mask, &gw);
    }
}

static void tcpip_done(void *arg) { (void)arg; tcpip_up = true; }

void plat_net_task_body(void) {
    tcpip_init(tcpip_done, NULL);
    while (!tcpip_up) { plat_checkin(PLAT_TASK_NET); vTaskDelay(pdMS_TO_TICKS(10)); }

    uint8_t mac[6];
    plat_net_mac(mac);
    ethernetif_set_mac(mac);
    net_cfg_t cfg = read_cfg();
    ip4_addr_t any;
    ip4_addr_set_zero(&any);
    plat_checkin(PLAT_TASK_NET);
    netifapi_netif_add(&nif, &any, &any, &any, NULL, ethernetif_init, tcpip_input);   /* PHY init: up to ~2 s */
    netifapi_netif_set_default(&nif);
#if LWIP_NETIF_HOSTNAME
    nif.hostname = "cems-supervisor";
#endif
    apply_cfg(&cfg);
    ready = true;

    for (;;) {
        plat_checkin(PLAT_TASK_NET);
        ethernetif_link_poll(&nif);
        net_cfg_t now = read_cfg();
        if (now.dhcp != cfg.dhcp || now.ip != cfg.ip || now.mask != cfg.mask || now.gw != cfg.gw) {
            cfg = now;
            apply_cfg(&cfg);                       /* the new address takes effect at once */
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
