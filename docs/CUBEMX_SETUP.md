# Bringing the platform up on the STM32H573I-DK (STM32CubeIDE)

These steps set up the CubeIDE project that the platform sources drop into. The
platform needs four things from CubeMX: the HAL, a TIM timebase, RTC, IWDG, the GPIO
labels and FreeRTOS. Everything else stays in the platform files.

Check each value marked **(check)** against the DK user manual or schematic before
relying on it. Those are the ones I couldn't confirm from the files here.

## 1. Project

1. STM32CubeIDE → New STM32 Project → Board Selector → **STM32H573I-DK**.
2. When asked about TrustZone, choose **without TrustZone** (the platform uses the `ARM_CM33_NTZ` FreeRTOS port).
3. Don't initialise all peripherals to their default mode. Add only the ones below, so the project stays readable.

## 2. System core

| Peripheral | Setting |
|---|---|
| SYS | Timebase source: **TIM6** (FreeRTOS needs SysTick) |
| RCC | Clocks as the board default gives (250 MHz SYSCLK). Enable LSE for the RTC **(check that the DK fits a 32.768 kHz crystal)** |
| RTC | Activate clock source and calendar. Hour format 24, binary data format. Clock source LSE |
| IWDG | Prescaler 32, reload 999: about 1 s at the nominal 32 kHz LSI. The supervisor kicks it every 100 ms |
| CORTEX_M33 | MPU off for now. Instruction cache (ICACHE) on |

## 3. GPIO labels

Give each pin you want to use a **user label** in the pinout view: `DI_01`, `DI_02` …
for inputs, `DO_01`, `DO_02` … for outputs. The BSP picks up whatever labels exist in
`main.h`, and a missing label just means that channel is absent. On the DK:

- the user button as `DI_01` (GPIO input)
- the user LEDs as `DO_01` … `DO_04` (GPIO output, push-pull)

If a pin is active low (LEDs usually are on ST boards **(check the DK schematic)**),
add this in `main.h` between `/* USER CODE BEGIN Private defines */` and the matching END:

```c
#define DO_01_ACTIVE_LOW 1
```

Channel numbers line up with `10_IO_Map.md` (DIG_IN_01 = DI_01, and so on). On the
real board, all 22 inputs and 20 outputs come from the schematic.

## 4. FreeRTOS

1. Help → Manage embedded software packages → STMicroelectronics → install **X-CUBE-FREERTOS** (FreeRTOS V11.x).
2. Middleware → enable X-CUBE-FREERTOS (CMSIS-RTOS2 interface is not needed).
3. Set the values the platform requires (see `port/stm32h5/FreeRTOSConfig.h`, the REQUIRED lines):
   `configTICK_RATE_HZ 1000`, `configMAX_PRIORITIES 16`, `configSUPPORT_STATIC_ALLOCATION 1`,
   `configCHECK_FOR_STACK_OVERFLOW 2`, `configUSE_MALLOC_FAILED_HOOK 1`, `INCLUDE_xTaskDelayUntil 1`,
   `configTOTAL_HEAP_SIZE` 48 KB, `configTIMER_TASK_PRIORITY 11`. Also set `configASSERT` to call
   `plat_assert_failed(__FILE__, __LINE__)`.
4. Create **no** tasks in CubeMX: the platform creates its own.
5. NVIC → Code generation: make sure **SVC, PendSV and SysTick handlers are not generated** (the FreeRTOS port defines them). X-CUBE-FREERTOS normally unticks these itself; check after generating that `stm32h5xx_it.c` doesn't contain them.
6. If the generated code already defines `vApplicationGetIdleTaskMemory` / `vApplicationGetTimerTaskMemory`, keep them: the platform's versions are weak and give way.

## 5. Add the platform to the project

1. Copy (or link) `platform/`, `port/stm32h5/bsp_stm32h5.c`, and for a first test `app_demo/`, into the project.
2. Include paths: `platform/include`, `app_demo`.
3. Don't add `platform/bsp/bsp_host.c`, `host/`, `tests/` or `port/stm32h5/check/` to the target build.
4. Linker script (`STM32H573IIKXQ_FLASH.ld` or similar): paste the contents of `port/stm32h5/noinit_section.ld` just before the `._user_heap_stack` section. Without it, the fault record is cleared at every boot.
5. In `main.c`:

```c
/* USER CODE BEGIN Includes */
#include "plat_tasks.h"
#include "app_demo.h"
/* USER CODE END Includes */
...
  /* USER CODE BEGIN 2 */
  plat_start(&app_demo);          /* creates all tasks, starts FreeRTOS, never returns */
  /* USER CODE END 2 */
```

If CubeMX also generates an `osKernelInitialize()` / `MX_FREERTOS_Init()` /
`osKernelStart()` block, `plat_start()` must come before it (it never returns), or that
block can be disabled.

## 6. First run: what you should see

- Hold the user button (DI_01) for 2 s and LED DO_01 comes on; release it and the LED goes off. That is the demo TON running on the 10 ms scan.
- In the debugger: `plat_scan.stats.scans` rises by 100 per second, `plat_scan.stats.max_exec_us` stays tiny for the demo, and `plat_diag.boots` counts warm resets.
- Watchdog test: put a breakpoint-free `for(;;);` in `plat_ui_task_body` (define it in a test file) and the unit resets after about 5 s, with `plat_diag.fault_name` = "ui". The debugger can stop the IWDG; enable `DBGMCU` IWDG freeze only if you want it frozen while halted.

## 7. Networking (next module, decision D3)

CubeMX does not offer lwIP for the STM32H5 (ST recommends NetX Duo). lwIP 2.2.1 for
the H5, with a FreeRTOS sys_arch and examples for this DK, is in ST's GitHub package
`STMicroelectronics/stm32h5-classic-coremw-apps` (EWARM projects only). The Modbus/HTTP
module will bring lwIP in from there as source.
