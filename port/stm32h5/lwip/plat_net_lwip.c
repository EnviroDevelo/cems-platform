/* plat_net_lwip.c - network bring-up and upkeep on the STM32H5 (net task).
 *
 *  - starts lwIP (tcpip thread) and the Ethernet interface;
 *  - MAC address: locally administered (02:xx:..), derived from the chip's
 *    96-bit unique ID, so every unit differs and keeps its address (DECISION
 *    D10: a registered OUI or an EUI-48 chip for production);
 *  - addresses from the parameter table (net_dhcp, net_ip, net_mask, net_gw);
 *    a change made from the touchscreen or web is applied live;
 *  - polls the PHY link every 250 ms;
 *  - duplicate-address check (RFC 5227, lwIP ACD): a static address is probed with
 *    ARP before it is used. If another device answers, the address is NOT taken
 *    (that would break the device already using it, often the PLC), the
 *    IP CONFLICT alarm is raised (NETST_CONFLICT, event log) and the probe is repeated
 *    every 30 s and on cable reconnect. After that, a device that later claims our
 *    address is detected too: the address is defended, kept, and the alarm raised.
 *    DHCP leases get the same check inside lwIP's DHCP client (declined if in use). */
#include "plat_clock.h"
#include <math.h>
#include "lwip/tcpip.h"
#include "lwip/netifapi.h"
#include "lwip/dhcp.h"
#include "lwip/acd.h"
#include "lwip/prot/acd.h"
#include "stm32h5xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ethernetif.h"
#include "plat_tasks.h"
#include "param.h"
#include "nvm.h"
#include "bsp.h"
#include "plat_log.h"
#include "tls.h"

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

/* ---- duplicate-address check (runs in the tcpip thread) ---- */
static struct acd acd;
static ip4_addr_t want_ip, want_mask, want_gw;
static volatile bool probing, conflict, bound;
static volatile uint32_t conflicts;          /* counts every conflict report */

static void acd_cb(struct netif *n, acd_callback_enum_t st) {
    if (st == ACD_IP_OK) {
        if (probing) netif_set_addr(n, &want_ip, &want_mask, &want_gw);   /* free: take it */
        probing = false;
        conflict = false;
        bound = true;
    } else {
        conflict = true;                     /* in use by another device */
        conflicts++;
        if (probing) { probing = false; acd_stop(&acd); }   /* not taken; the net task retries */
        /* once bound, lwIP has defended the address; we keep it and raise the alarm */
    }
}

static void probe_start(void *arg) {         /* tcpip thread */
    (void)arg;
    ip4_addr_t any;
    ip4_addr_set_zero(&any);
    acd_stop(&acd);
    netif_set_addr(&nif, &any, &any, &any);  /* not used until it is known to be free */
    bound = false;
    probing = true;
    if (acd_start(&nif, &acd, want_ip) != ERR_OK) probing = false;
}

static void acd_setup(void *arg) { (void)arg; acd_add(&nif, &acd, acd_cb); }

static void apply_cfg(const net_cfg_t *c) {
    IP4_ADDR(&want_ip, c->ip >> 24, (c->ip >> 16) & 255, (c->ip >> 8) & 255, c->ip & 255);
    IP4_ADDR(&want_mask, c->mask >> 24, (c->mask >> 16) & 255, (c->mask >> 8) & 255, c->mask & 255);
    IP4_ADDR(&want_gw, c->gw >> 24, (c->gw >> 16) & 255, (c->gw >> 8) & 255, c->gw & 255);
    conflict = false;
    if (c->dhcp) {
        tcpip_callback(acd_setup, NULL);     /* (harmless if already added) */
        probing = false;
        bound = false;
        netifapi_dhcp_start(&nif);
    } else {
        netifapi_dhcp_release_and_stop(&nif);
        tcpip_callback(probe_start, NULL);
    }
}

static void publish_status(bool dhcp, uint32_t *was) {
    uint32_t st = 0;
    if (netif_is_link_up(&nif)) st |= NETST_LINK;
    if (conflict) st |= NETST_CONFLICT;
    if (probing) st |= NETST_PROBING;
    if (dhcp ? dhcp_supplied_address(&nif) : bound) st |= NETST_READY;
    plat_net_status_word = st;
    if (st & NETST_READY) tls_set_ip(lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&nif))));   /* certificate follows the address */
    if ((st ^ *was) & NETST_CONFLICT)
        plat_log_event(LOG_EV_IP_CONFLICT, PLAT_SRC_INTERNAL, 0, lwip_ntohl(ip4_addr_get_u32(&want_ip)),
                       NAN, (st & NETST_CONFLICT) ? 1.0 : 0.0, NULL);
    *was = st;
}

/* for the touchscreen's System screen (ui.h) */
bool ui_net_status(uint32_t *ip, bool *link_up, uint8_t mac[6]) {
    if (!ready) return false;
    *ip = lwip_ntohl(ip4_addr_get_u32(netif_ip4_addr(&nif)));
    *link_up = netif_is_link_up(&nif);
    plat_net_mac(mac);
    return true;
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
    tcpip_callback(acd_setup, NULL);
#if LWIP_NETIF_HOSTNAME
    nif.hostname = "cems-supervisor";
#endif
    apply_cfg(&cfg);
    ready = true;

    uint32_t was = 0, retry = 0, t = 0, seen = 0, seen_t = 0;
    bool link_was = false;
    for (;;) {
        plat_checkin(PLAT_TASK_NET);
        ethernetif_link_poll(&nif);
        net_cfg_t now = read_cfg();
        if (now.dhcp != cfg.dhcp || now.ip != cfg.ip || now.mask != cfg.mask || now.gw != cfg.gw) {
            cfg = now;
            apply_cfg(&cfg);                       /* checked, then used at once */
        }
        bool link = netif_is_link_up(&nif);
        t += 250u;
        if (!cfg.dhcp && link && ((link && !link_was) || (conflict && !bound && !probing && t - retry >= 30000u))) {
            retry = t;                             /* cable plugged in, or 30 s after a refused address: check again */
            tcpip_callback(probe_start, NULL);
        }
        if (conflicts != seen) { seen = conflicts; seen_t = t; }
        if (conflict && bound && t - seen_t >= 60000u) conflict = false;   /* in use and no claim for 60 s: clear */
        link_was = link;
        publish_status(cfg.dhcp, &was);
        if (link && (bound || cfg.dhcp)) sntp_service(bsp_ms());   /* site time server, if one is set */
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
