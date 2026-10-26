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
#include "ethernetif.h"
#include "lan8742.h"
#include "plat_config.h"
#include "plat_selftest.h"

#define ETHIF_TX_TIMEOUT_MS   2000U
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
    return true;
}

/* Called every few hundred ms by the net task (outside the tcpip thread). */
void ethernetif_link_poll(struct netif *netif) {
    int32_t st = LAN8742_GetLinkState(&LAN8742);
    if (netif_is_link_up(netif) && st <= LAN8742_STATUS_LINK_DOWN) {
        HAL_ETH_Stop_IT(&EthHandle);
        netifapi_netif_set_down(netif);
        netifapi_netif_set_link_down(netif);
    } else if (!netif_is_link_up(netif) && st > LAN8742_STATUS_LINK_DOWN) {
        if (apply_phy_state(st)) {
            netifapi_netif_set_up(netif);
            netifapi_netif_set_link_up(netif);
        }
    }
}

/* ---------------- receive task ---------------- */
static struct pbuf *low_level_input(struct netif *netif) {
    (void)netif;
    struct pbuf *p = NULL;
    if (RxAllocStatus == RX_ALLOC_OK) HAL_ETH_ReadData(&EthHandle, (void **)&p);
    return p;
}

static void ethernetif_input(void *argument) {
    struct netif *netif = (struct netif *)argument;
    for (;;) {
        if (xSemaphoreTake(RxPktSemaphore, portMAX_DELAY) == pdTRUE) {
            struct pbuf *p;
            do {
                p = low_level_input(netif);
                if (p != NULL && netif->input(p, netif) != ERR_OK) pbuf_free(p);
            } while (p != NULL);
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
