/* main.h - COMPILE-CHECK STUB ONLY. Stands in for the CubeMX-generated main.h so
 * the target sources can be compiled outside CubeIDE. The pin assignments here
 * are placeholders, not the DK's or the product's real pins. */
#ifndef MAIN_H
#define MAIN_H
#include "stm32h5xx_hal.h"
#define DI_01_Pin GPIO_PIN_13
#define DI_01_GPIO_Port GPIOC
#define DO_01_Pin GPIO_PIN_0
#define DO_01_GPIO_Port GPIOA
#define DO_01_ACTIVE_LOW 1
#define DO_02_Pin GPIO_PIN_1
#define DO_02_GPIO_Port GPIOA
/* compile-check both FRAM paths: define CHECK_WITH_FRAM to build the SPI driver */
#ifdef CHECK_WITH_I2C_FRAM
#define FRAM_I2C_HANDLE hi2c1
#endif
#ifdef CHECK_WITH_EVE
#define EVE_CS_Pin GPIO_PIN_6
#define EVE_CS_GPIO_Port GPIOE
#define EVE_PD_Pin GPIO_PIN_7
#define EVE_PD_GPIO_Port GPIOE
#endif
#ifdef CHECK_WITH_NOR
#define NOR_CS_Pin GPIO_PIN_5
#define NOR_CS_GPIO_Port GPIOE
#endif
#ifdef CHECK_WITH_RS485
#define BSP_RS485_HUART huart2
#endif
#ifdef CHECK_WITH_W5500
#define W5500_CS_Pin GPIO_PIN_11
#define W5500_CS_GPIO_Port GPIOE
#define W5500_RST_Pin GPIO_PIN_10
#define W5500_RST_GPIO_Port GPIOE
#define CS_EXP_Pin GPIO_PIN_3              /* the SPI expansion header (CORE_PCB.md 16): held high */
#define CS_EXP_GPIO_Port GPIOD
#define EXP_INT_Pin GPIO_PIN_7
#define EXP_INT_GPIO_Port GPIOD
#endif
#ifdef CHECK_WITH_NODE_IO             /* the CORE PCB rev 0.2 I/O node pins (CORE_PCB.md 14) */
#define CS_A_Pin GPIO_PIN_2
#define CS_A_GPIO_Port GPIOE
#define CS_B_Pin GPIO_PIN_3
#define CS_B_GPIO_Port GPIOE
#define CS_IN_Pin GPIO_PIN_4
#define CS_IN_GPIO_Port GPIOE
#define CS_T_Pin GPIO_PIN_15
#define CS_T_GPIO_Port GPIOE
#define CS_AI_Pin GPIO_PIN_0
#define CS_AI_GPIO_Port GPIOE
#define B_FAULT_Pin GPIO_PIN_14
#define B_FAULT_GPIO_Port GPIOD
#define B_PGOOD_Pin GPIO_PIN_15
#define B_PGOOD_GPIO_Port GPIOD
#define GATE_EN_Pin GPIO_PIN_8
#define GATE_EN_GPIO_Port GPIOB
#define WDI_Pin GPIO_PIN_9
#define WDI_GPIO_Port GPIOB
#define GATE_FB_Pin GPIO_PIN_8
#define GATE_FB_GPIO_Port GPIOD
#define PUMP_DIR_Pin GPIO_PIN_6
#define PUMP_DIR_GPIO_Port GPIOC
#define PUMP_EN_Pin GPIO_PIN_7
#define PUMP_EN_GPIO_Port GPIOC
#define REL1_N_Pin GPIO_PIN_4
#define REL1_N_GPIO_Port GPIOB
#define REL2_N_Pin GPIO_PIN_5
#define REL2_N_GPIO_Port GPIOB
#endif
#ifdef CHECK_WITH_SUPPLY
#define SUPPLY_SENSE_Pin GPIO_PIN_0
#define SUPPLY_SENSE_GPIO_Port GPIOC
#endif
#if defined(PLAT_PSRAM) && PLAT_PSRAM
#define PSRAM_XSPI_HANDLE hospi1
#endif
#ifdef CHECK_WITH_FRAM
#define FRAM_CS_Pin GPIO_PIN_4
#define FRAM_CS_GPIO_Port GPIOE
#endif
#endif
