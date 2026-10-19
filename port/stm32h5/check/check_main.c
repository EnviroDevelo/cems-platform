/* check_main.c - COMPILE-CHECK STUB: the shape of main() after CubeMX init. */
#include "main.h"
#include "plat_tasks.h"
#include "app_demo.h"
RTC_HandleTypeDef hrtc;
IWDG_HandleTypeDef hiwdg;
SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;
I2C_HandleTypeDef hi2c1;
RNG_HandleTypeDef hrng;
int main(void) {
    HAL_Init();
    /* SystemClock_Config(); MX_GPIO_Init(); MX_RTC_Init(); MX_IWDG_Init(); ... */
    plat_start(&app_demo_plat);
}
