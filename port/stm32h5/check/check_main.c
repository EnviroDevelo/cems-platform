/* check_main.c - COMPILE-CHECK STUB: the shape of main() after CubeMX init. */
#include "main.h"
#include "plat_tasks.h"
#include "app_demo.h"
RTC_HandleTypeDef hrtc;
IWDG_HandleTypeDef hiwdg;
int main(void) {
    HAL_Init();
    /* SystemClock_Config(); MX_GPIO_Init(); MX_RTC_Init(); MX_IWDG_Init(); ... */
    plat_start(&app_demo);
}
