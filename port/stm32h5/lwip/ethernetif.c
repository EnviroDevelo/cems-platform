/**
  ******************************************************************************
  * @file    ethernetif.c
  * @brief   Ethernet network interface driver for lwIP (STM32H5, zero-copy).
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component (see LICENSE_ST_SLA0044.md
  * next to this file). If no LICENSE file comes with this software, it is
  * provided AS-IS.
  *
  * Derived from STM32H5 LwIP_HTTP_Server_Socket_RTOS (stm32h5-classic-coremw-apps).
  * Modified for the CEMS platform:
  *  - native FreeRTOS API instead of CMSIS-RTOS2, with the FromISR variants in
  *    the interrupt callbacks;
  *  - the receive task runs below the control, scan and store tasks
  *    (PLAT_PRIO_ETH_RX), so network load can never delay control;
  *  - MAC address passed in by the platform (unique per unit), not a constant;
  *  - HAL_ETH_MspInit (board pins) left to CubeMX;
  *  - link monitoring moved to plat_net_lwip.c (ethernetif_link_poll).
  ******************************************************************************
  */
#include <stdbool.h>
#include <string.h>
#include "stm32h5xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "lwip/opt.h"
#include "lwip/timeouts.h"
#include "lwip/tcpip.h"
#include "lwip/netifapi.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip_hooks.h"
#include "ethernetif.h"
#include "lan8742.h"
#include "plat_config.h"
#include "plat_selftest.h"
#include "plat_net.h"

#define ETHIF_TX_TIMEOUT_MS   2000U
#define ETHIF_RX_POLL_MS      50U     /* receive also checked this often without an interrupt */
#define ETH_RX_BUFFER_SIZE    1000U       /* must be the same as heth.Init.RxBuffLen */
#define ETH_RX_BUFFER_CNT     12U         /* must be greater than ETH_RX_DESC_CNT */
#define IFNAME0 'e'
#define IFNAME1 'n'

typedef enum { RX_ALLOC_OK = 0, RX_ALLOC_ERROR = 1 } rx_alloc_t;

typedef struct {
    struct pbuf_custom pbuf_custom;
    uint8_t buff[(ETH_RX_BUFFER_SIZE + 31) & ~31] __ALIGNED(32);
} RxBuff_t;

static ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT];   /* contiguous Rx descriptors */
static ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT];   /* contiguous Tx descriptors */
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

static volatile rx_alloc_t RxAllocStatus;
static volatile uint32_t   rx_pool_empty, rx_missed, rx_overflow;   /* plat_eth_diag */
static volatile uint16_t   phy_scsr, phy_secr;                      /* read by the link poll only */
static volatile uint16_t   phy_anar, phy_anlpar, phy_aner;
static volatile uint32_t   link_drops, resyncs;
static int32_t             applied = LAN8742_STATUS_LINK_DOWN;      /* the PHY state the MAC is set to */
static uint8_t             link_mode = 0xFFu;                        /* offer in force: none written yet */
static uint8_t             link_want;                                /* net_link_mode as set */
static bool                fell_back;                                /* auto mode, 100 Mbit lost data: offering 10 */
static uint32_t            fallbacks;
static volatile uint32_t   tcp_segs, tcp_rexmits;                    /* data segments sent, of them sent again */
static SemaphoreHandle_t   RxPktSemaphore, TxPktSemaphore;
static StaticSemaphore_t   rx_sem_mem, tx_sem_mem;
static StaticTask_t        rx_tcb;
static StackType_t         rx_stack[PLAT_STACK_ETH_RX];
static uint8_t             mac[6];

ETH_HandleTypeDef EthHandle;

static int32_t ETH_PHY_IO_Init(void);
static int32_t ETH_PHY_IO_DeInit(void);
static int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
static int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
static int32_t ETH_PHY_IO_GetTick(void);

static lan8742_Object_t LAN8742;
static lan8742_IOCtx_t  LAN8742_IOCtx = { ETH_PHY_IO_Init, ETH_PHY_IO_DeInit, ETH_PHY_IO_WriteReg,
                                          ETH_PHY_IO_ReadReg, ETH_PHY_IO_GetTick };

void pbuf_free_custom(struct pbuf *p);

static void give_from_isr(SemaphoreHandle_t s) {
    BaseType_t woken = pdFALSE;
    if (s) xSemaphoreGiveFromISR(s, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---------------- link: PHY state -> MAC config + netif ---------------- */
static bool apply_phy_state(int32_t st) {
    uint32_t duplex, speed;
    switch (st) {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX: duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_100M; break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX: duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_100M; break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:  duplex = ETH_FULLDUPLEX_MODE; speed = ETH_SPEED_10M;  break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:  duplex = ETH_HALFDUPLEX_MODE; speed = ETH_SPEED_10M;  break;
    default: return false;
    }
    ETH_MACConfigTypeDef MACConf = { 0 };
    HAL_ETH_GetMACConfig(&EthHandle, &MACConf);
    MACConf.DuplexMode = duplex;
    MACConf.Speed = speed;
    HAL_ETH_SetMACConfig(&EthHandle, &MACConf);
    HAL_ETH_Start_IT(&EthHandle);
    applied = st;
    return true;
}

/* Called every few hundred ms by the net task (outside the tcpip thread). It is the only MDIO
 * user after start-up, so it also reads the PHY's state and error counter for plat_eth_diag, and
 * collects the MAC's clear-on-read drop counters. */
void ethernetif_link_poll(struct netif *netif) {
    int32_t st = LAN8742_GetLinkState(&LAN8742);
    uint32_t v;
    if (HAL_ETH_ReadPHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_PHYSCSR, &v) == HAL_OK) phy_scsr = (uint16_t)v;
    if (HAL_ETH_ReadPHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_SECR, &v) == HAL_OK) phy_secr = (uint16_t)v;
    if (HAL_ETH_ReadPHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_ANAR, &v) == HAL_OK) phy_anar = (uint16_t)v;
    if (HAL_ETH_ReadPHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_ANLPAR, &v) == HAL_OK) phy_anlpar = (uint16_t)v;
    if (HAL_ETH_ReadPHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_ANER, &v) == HAL_OK) phy_aner = (uint16_t)v;
    rx_missed += EthHandle.Instance->DMACMFCR & ETH_DMACMFCR_MFC;
    rx_overflow += EthHandle.Instance->MTLRQMPOCR & ETH_MTLRQMPOCR_OVFPKTCNT;
    if (netif_is_link_up(netif) && st <= LAN8742_STATUS_LINK_DOWN) {
        HAL_ETH_Stop_IT(&EthHandle);
        netifapi_netif_set_down(netif);
        netifapi_netif_set_link_down(netif);
        applied = LAN8742_STATUS_LINK_DOWN;
        link_drops++;
    } else if (!netif_is_link_up(netif) && st > LAN8742_STATUS_LINK_DOWN) {
        if (apply_phy_state(st)) {
            netifapi_netif_set_up(netif);
            netifapi_netif_set_link_up(netif);
        }
    } else if (netif_is_link_up(netif) && st != applied
               && st >= LAN8742_STATUS_100MBITS_FULLDUPLEX && st <= LAN8742_STATUS_10MBITS_HALFDUPLEX) {
        /* The switch renegotiated between two polls without the link looking down: a MAC left on
         * the old speed or duplex loses frames silently, so set it again. */
        HAL_ETH_Stop_IT(&EthHandle);
        if (apply_phy_state(st)) resyncs++;
    }
}

/* What the port offers the switch (net_link_mode). Autonegotiation stays on in every mode, only
 * the offer shrinks: forcing a mode would make an autonegotiating switch fall back to half duplex
 * against our full duplex, which loses frames without an error on either side. Written at the
 * first call (the PHY's straps may offer less than everything) and on a change; when the offer
 * really changes, the PHY renegotiates and the link drops for a moment. */
static void write_offer(unsigned mode) {
    static const uint16_t offer[] = {
        LAN8742_ANAR_100BASE_TX_FD | LAN8742_ANAR_100BASE_TX | LAN8742_ANAR_10BASE_T_FD | LAN8742_ANAR_10BASE_T,
        LAN8742_ANAR_100BASE_TX_FD | LAN8742_ANAR_100BASE_TX,
        LAN8742_ANAR_10BASE_T_FD | LAN8742_ANAR_10BASE_T,
    };
    const uint32_t all = offer[0];
    uint32_t anar, bcr;
    if (mode == link_mode) return;
    if (HAL_ETH_ReadPHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_ANAR, &anar) != HAL_OK) return;
    if (HAL_ETH_ReadPHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_BCR, &bcr) != HAL_OK) return;
    uint32_t want = (anar & ~all) | offer[mode];
    if (want != anar || !(bcr & LAN8742_BCR_AUTONEGO_EN)) {
        if (HAL_ETH_WritePHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_ANAR, want) != HAL_OK) return;
        bcr &= ~(uint32_t)(LAN8742_BCR_POWER_DOWN | LAN8742_BCR_ISOLATE | LAN8742_BCR_LOOPBACK | LAN8742_BCR_SOFT_RESET);
        if (HAL_ETH_WritePHYRegister(&EthHandle, LAN8742.DevAddr, LAN8742_BCR,
                                     bcr | LAN8742_BCR_AUTONEGO_EN | LAN8742_BCR_RESTART_AUTONEGO) != HAL_OK) return;
    }
    link_mode = (uint8_t)mode;
}

void ethernetif_set_link_mode(unsigned mode) {
    if (mode > 2u) mode = 0;
    if (mode != link_want) fell_back = false;          /* a new setting starts again at what it asks */
    link_want = (uint8_t)mode;
    write_offer(mode == 0u && fell_back ? 2u : mode);
}

/* lwip_hooks.h: every TCP segment on its way out (tcpip thread). One that starts below snd_nxt
 * was sent before: a retransmission, after a timeout or three duplicate ACKs. */
u32_t *ethernetif_tcp_out(struct pbuf *p, struct tcp_hdr *hdr, const struct tcp_pcb *pcb, u32_t *opts) {
    if (pcb != NULL && p->tot_len > TCPH_HDRLEN_BYTES(hdr)) {
        tcp_segs++;
        if (TCP_SEQ_LT(lwip_ntohl(hdr->seqno), pcb->snd_nxt)) tcp_rexmits++;
    }
    return opts;
}

/* Automatic 10 Mbit (net_link_mode 0 only). Some switch and cable combinations lose 100BASE-TX
 * frames without an error at either end (the DK on Gary's switch: 14-45 %, none at 10 Mbit), so
 * TCP retransmissions are the only sign. Two 10 s windows in a row with real traffic and 3 % or
 * more sent again (a healthy LAN resends next to nothing; on the bench 2-3 % already cost the I/O
 * board link a 1 s outage): offer 10 Mbit only until a power cycle or a net_link_mode change. Little
 * traffic leaves the count as it was, so one vanished browser can't do it on its own. */
#define WATCH_POLLS   40u                /* 10 s of 250 ms polls */
#define WATCH_SEGS    40u
#define WATCH_REXMITS 5u
#define WATCH_PCT     3u
#define WATCH_BAD     2u
unsigned ethernetif_link_watch(struct netif *netif) {
    static uint32_t polls, segs0, rex0;
    static unsigned bad;
    if (++polls < WATCH_POLLS) return 0;
    polls = 0;
    uint32_t segs = tcp_segs - segs0, rex = tcp_rexmits - rex0;
    segs0 += segs; rex0 += rex;
    bool at100 = applied == LAN8742_STATUS_100MBITS_FULLDUPLEX || applied == LAN8742_STATUS_100MBITS_HALFDUPLEX;
    if (link_want != 0u || fell_back || !netif_is_link_up(netif) || !at100) { bad = 0; return 0; }
    if (segs < WATCH_SEGS) return 0;
    if (rex < WATCH_REXMITS || rex * 100u < segs * WATCH_PCT) { bad = 0; return 0; }
    if (++bad < WATCH_BAD) return 0;
    bad = 0;
    fell_back = true;
    fallbacks++;
    write_offer(2u);
    return (unsigned)(rex * 100u / segs);
}

/* ---------------- receive task ---------------- */
static struct pbuf *low_level_input(struct netif *netif) {
    (void)netif;
    struct pbuf *p = NULL;
    if (RxAllocStatus == RX_ALLOC_OK) HAL_ETH_ReadData(&EthHandle, (void **)&p);
    return p;
}

/* Receive does not rely on the interrupt alone. After the core was halted (a debugger
 * attach) the RX interrupt could stay silent and the DMA sit suspended with every
 * descriptor full (RBU), so the port went deaf until a power cycle (DK bench, 29 Oct).
 * Every ETHIF_RX_POLL_MS the task drains the ring anyway and, if the DMA reports
 * buffers unavailable, clears the flag and rewrites the tail pointer to resume it. */
uint32_t ethernetif_rx_kicks;          /* DMA receive restarts (diagnostics) */

static void ethernetif_input(void *argument) {
    struct netif *netif = (struct netif *)argument;
    for (;;) {
        bool irq = xSemaphoreTake(RxPktSemaphore, pdMS_TO_TICKS(ETHIF_RX_POLL_MS)) == pdTRUE;
        struct pbuf *p;
        do {
            p = low_level_input(netif);
            if (p != NULL && netif->input(p, netif) != ERR_OK) pbuf_free(p);
        } while (p != NULL);
        if (!irq && (EthHandle.Instance->DMACSR & ETH_DMACSR_RBU)) {
            EthHandle.Instance->DMACSR = ETH_DMACSR_RBU;                    /* write 1 to clear */
            EthHandle.Instance->DMACRDTPR = EthHandle.Instance->DMACRDTPR;  /* poll demand: resume */
            ethernetif_rx_kicks++;
        }
    }
}

/* ---------------- init ---------------- */
static void low_level_init(struct netif *netif) {
    EthHandle.Instance = ETH;
    EthHandle.Init.MACAddr = mac;
    EthHandle.Init.MediaInterface = HAL_ETH_RMII_MODE;
    EthHandle.Init.RxDesc = DMARxDscrTab;
    EthHandle.Init.TxDesc = DMATxDscrTab;
    EthHandle.Init.RxBuffLen = ETH_RX_BUFFER_SIZE;
    HAL_ETH_Init(&EthHandle);                    /* pins and clocks: HAL_ETH_MspInit from CubeMX */

    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, mac, 6);
    netif->mtu = ETH_MAX_PAYLOAD;
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;

    LWIP_MEMPOOL_INIT(RX_POOL);
    RxPktSemaphore = xSemaphoreCreateBinaryStatic(&rx_sem_mem);
    TxPktSemaphore = xSemaphoreCreateBinaryStatic(&tx_sem_mem);
    xTaskCreateStatic(ethernetif_input, "eth_rx", PLAT_STACK_ETH_RX, netif, PLAT_PRIO_ETH_RX, rx_stack, &rx_tcb);

    LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);
    bool phy_ok = LAN8742_Init(&LAN8742) == LAN8742_STATUS_OK;
    plat_selftest_report(PLAT_STEST_NET_PHY, phy_ok);   /* CIP-11.1 */
    if (!phy_ok) {
        netif_set_link_down(netif);
        netif_set_down(netif);
        return;
    }
    if (apply_phy_state(LAN8742_GetLinkState(&LAN8742))) {
        netif_set_up(netif);
        netif_set_link_up(netif);
    } else {
        netif_set_link_down(netif);
        netif_set_down(netif);
    }
}

/* ---------------- transmit ---------------- */
static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    uint32_t i = 0U;
    err_t errval = ERR_OK;
    ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT];
    ETH_TxPacketConfigTypeDef TxConfig;

    memset(Txbuffer, 0, sizeof Txbuffer);
    memset(&TxConfig, 0, sizeof TxConfig);
    TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
    TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
    TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (i >= ETH_TX_DESC_CNT) return ERR_IF;
        Txbuffer[i].buffer = q->payload;
        Txbuffer[i].len = q->len;
        if (i > 0) Txbuffer[i - 1].next = &Txbuffer[i];
        if (q->next == NULL) Txbuffer[i].next = NULL;
        i++;
    }
    TxConfig.Length = p->tot_len;
    TxConfig.TxBuffer = Txbuffer;
    TxConfig.pData = p;

    pbuf_ref(p);
    HAL_ETH_ReleaseTxPacket(&EthHandle);         /* free sent descriptors first: works without the TX interrupt too */
    do {
        if (HAL_ETH_Transmit_IT(&EthHandle, &TxConfig) == HAL_OK) {
            errval = ERR_OK;
        } else if (HAL_ETH_GetError(&EthHandle) & HAL_ETH_ERROR_BUSY) {
            xSemaphoreTake(TxPktSemaphore, pdMS_TO_TICKS(ETHIF_TX_TIMEOUT_MS));   /* wait for descriptors */
            HAL_ETH_ReleaseTxPacket(&EthHandle);
            errval = ERR_BUF;
        } else {
            pbuf_free(p);
            errval = ERR_IF;
        }
    } while (errval == ERR_BUF);
    return errval;
}

void ethernetif_set_mac(const uint8_t m[6]) { memcpy(mac, m, 6); }

err_t ethernetif_init(struct netif *netif) {
    LWIP_ASSERT("netif != NULL", (netif != NULL));
    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;
    netif->output = etharp_output;
    netif->linkoutput = low_level_output;
    low_level_init(netif);
    return ERR_OK;
}

/* ---------------- zero-copy RX buffers ---------------- */
void pbuf_free_custom(struct pbuf *p) {
    struct pbuf_custom *custom_pbuf = (struct pbuf_custom *)p;
    LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);
    if (RxAllocStatus == RX_ALLOC_ERROR) {
        RxAllocStatus = RX_ALLOC_OK;
        xSemaphoreGive(RxPktSemaphore);
    }
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff) {
    struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
    if (p) {
        *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
        p->custom_free_function = pbuf_free_custom;
        pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUFFER_SIZE);
    } else {
        rx_pool_empty++;
        RxAllocStatus = RX_ALLOC_ERROR;
        *buff = NULL;
    }
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length) {
    struct pbuf **ppStart = (struct pbuf **)pStart, **ppEnd = (struct pbuf **)pEnd;
    struct pbuf *p = (struct pbuf *)(void *)(buff - offsetof(RxBuff_t, buff));
    p->next = NULL;
    p->tot_len = 0;
    p->len = Length;
    if (!*ppStart) *ppStart = p; else (*ppEnd)->next = p;
    *ppEnd = p;
    for (p = *ppStart; p != NULL; p = p->next) p->tot_len = (u16_t)(p->tot_len + Length);
}

bool plat_eth_diag(plat_eth_diag_t *d) {
    ETH_TypeDef *e = EthHandle.Instance;
    if (e == NULL) return false;
    uint16_t hcd = phy_scsr & LAN8742_PHYSCSR_HCDSPEEDMASK;
    memset(d, 0, sizeof *d);
    d->link_up = netif_is_link_up(netif_default) != 0;
    d->phy_mbps = (phy_scsr & LAN8742_PHYSCSR_AUTONEGO_DONE) == 0 ? 0u
                : (hcd == LAN8742_PHYSCSR_100BTX_FD || hcd == LAN8742_PHYSCSR_100BTX_HD) ? 100u : 10u;
    d->phy_full = hcd == LAN8742_PHYSCSR_100BTX_FD || hcd == LAN8742_PHYSCSR_10BT_FD;
    d->mac_mbps = (e->MACCR & ETH_MACCR_FES) ? 100u : 10u;
    d->mac_full = (e->MACCR & ETH_MACCR_DM) != 0;
    d->phy_symbol_errors = phy_secr;
    d->rx_good_unicast = e->MMCRUPGR;
    d->tx_good = e->MMCTPCGR;
    d->rx_crc_errors = e->MMCRCRCEPR;
    d->rx_align_errors = e->MMCRAEPR;
    d->rx_missed = rx_missed;
    d->rx_overflow = rx_overflow;
    d->rx_pool_empty = rx_pool_empty;
    d->rx_kicks = ethernetif_rx_kicks;
    d->link_mode = link_want;
    d->fallback = fell_back;
    d->fallbacks = fallbacks;
    d->tcp_segs = tcp_segs;
    d->tcp_rexmits = tcp_rexmits;
    d->phy_anar = phy_anar;
    d->phy_anlpar = phy_anlpar;
    d->phy_aner = phy_aner;
    d->link_drops = link_drops;
    d->resyncs = resyncs;
    return true;
}

void HAL_ETH_TxFreeCallback(uint32_t *buff) { pbuf_free((struct pbuf *)buff); }

/* ---------------- interrupt callbacks (ETH_IRQn, priority >= 5) ---------------- */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth) { (void)heth; give_from_isr(RxPktSemaphore); }
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth) { (void)heth; give_from_isr(TxPktSemaphore); }
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth) {
    if ((HAL_ETH_GetDMAError(heth) & ETH_DMACSR_RBU) == ETH_DMACSR_RBU) give_from_isr(RxPktSemaphore);
    if ((HAL_ETH_GetDMAError(heth) & ETH_DMACSR_TBU) == ETH_DMACSR_TBU) give_from_isr(TxPktSemaphore);
}
void ETH_IRQHandler(void) { HAL_ETH_IRQHandler(&EthHandle); }

/* ---------------- PHY access over MDIO ---------------- */
static int32_t ETH_PHY_IO_Init(void) { HAL_ETH_SetMDIOClockRange(&EthHandle); return 0; }
static int32_t ETH_PHY_IO_DeInit(void) { return 0; }
static int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal) {
    return HAL_ETH_ReadPHYRegister(&EthHandle, DevAddr, RegAddr, pRegVal) == HAL_OK ? 0 : -1;
}
static int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal) {
    return HAL_ETH_WritePHYRegister(&EthHandle, DevAddr, RegAddr, RegVal) == HAL_OK ? 0 : -1;
}
static int32_t ETH_PHY_IO_GetTick(void) { return (int32_t)HAL_GetTick(); }
