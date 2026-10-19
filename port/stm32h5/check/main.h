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
#ifdef CHECK_WITH_FRAM
#define FRAM_CS_Pin GPIO_PIN_4
#define FRAM_CS_GPIO_Port GPIOE
#endif
#endif
