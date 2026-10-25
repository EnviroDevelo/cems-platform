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
4. **Memory for stacks (Gary, 25 Oct: stack overflows on earlier DK prototypes; set these large from the start).** Project Manager → Project → Linker Settings: **Minimum Stack Size 0x2000** (8 KB, the main stack: interrupts, and start-up before the scheduler, which includes the HTTPS key generation on first boot) and **Minimum Heap Size 0x1000** (4 KB, the C library's heap). ST's defaults are 0x400 and 0x200, too small. These are minimums the linker checks; the build fails rather than run short.
   The task stacks are not set in CubeMX: the platform allocates them itself (`plat_config.h` `PLAT_STACK_*`, 85 KB in all, about twice the first sizes), as it does the FreeRTOS heap (64 KB, section 4) and the idle and timer task stacks (`FreeRTOSConfig.h`). Leave CubeMX's FreeRTOS task list empty. A task that still runs past its stack is stopped by the CPU's stack limit and the next boot names it ("stk scan" on the System screen; `PLATFORM.md` D4). Trim the sizes only after the 72-hour soak on hardware, from the margins shown under `health` on `/api/info`.

## 2. System core

| Peripheral | Setting |
|---|---|
| SYS | Timebase source: **TIM6** (FreeRTOS needs SysTick) |
| RCC | Clocks as the board default gives (250 MHz SYSCLK). Enable LSE for the RTC **(check that the DK fits a 32.768 kHz crystal)** |
| RTC | Activate clock source and calendar. Hour format 24, binary data format. Clock source LSE. The product board has a VBAT supply (coin cell or supercap; `docs/HARDWARE.md` H1) so the clock survives a power cut; without one every power-up starts "clock not set" until someone, the PLC or the time server sets it |
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

## 3a. Parameter FRAM

Development part: **Adafruit 1895** (Digi-Key 1528-1035-ND), an MB85RC256V I2C FRAM,
32 KB, address 0x50 with A0..A2 left open, WP pulled down on the board (writes enabled),
3.3 V or 5 V, up to 1 MHz. The platform's default memory map is now the product's 128 KB
part (D6), so **with this breakout add `PLAT_FRAM_SIZE=32768u` to the project's preprocessor
symbols** (Project → Properties → C/C++ General → Paths and Symbols). Without it the driver
reads the part's ID, sees 32 KB, and refuses it (the unit runs on defaults with the
calibration alarm) rather than writing past its end. Wire VCC to 3.3 V, GND, and SCL/SDA to a free I2C on the DK
**(check which I2C the DK brings out, and whether that bus already has pull-ups)**.

| CubeMX | Setting |
|---|---|
| I2Cx | I2C mode, Fast mode Plus 1 MHz (or Fast mode 400 kHz), 7-bit addressing |
| I2Cx NVIC | Event and error interrupts enabled, priority 5 or numerically higher |
| GPDMA | One channel for I2Cx_TX and one for I2Cx_RX, linked to the I2C handle (the default DMA mode needs them) |

In `main.h` (USER CODE section):

```c
#define FRAM_I2C_HANDLE hi2c1     /* the handle CubeMX generated; selects the I2C driver */
/* optional: #define FRAM_I2C_MODE 1   interrupt mode if you'd rather not set up DMA */
```

Add `nvm_fram_i2c.c` to the build. Without any FRAM configured, the platform uses a
RAM stand-in and reports `PSTAT_NVM_VOLATILE`. With `FRAM_I2C_HANDLE` defined but no
chip answering, it runs on defaults and raises the calibration-invalid alarm (it never
quietly falls back to RAM).

Note: the driver defines the HAL's global I2C completion callbacks. If another I2C
device is added later, those move to one shared file that dispatches by handle.

The SPI FRAM driver (`nvm_fram_spi.c`, `FRAM_CS` label) stays available if the
product board uses an SPI part.

## 3c. Log flash (optional on the DK)

The logger uses a JEDEC serial NOR flash on plain SPI (16 MB W25Q128JV or similar). The
DK's own flash sits on OCTOSPI and is not used. Without a NOR chip the event log runs
from a 32 KB RAM stand-in until power-off, which is enough for bench work.

| CubeMX | Setting |
|---|---|
| SPIx | Full-duplex master, 8-bit, mode 0 (CPOL low, CPHA 1 edge), NSS software, up to 50 MHz. It may share the FRAM's SPI bus |
| GPIO | One output labelled `NOR_CS`, push-pull, initial level high |

In `main.h` (USER CODE section), if the handle isn't `hspi2`:

```c
#define NOR_SPI_HANDLE hspi1
```

Add `port/stm32h5/nor_spi.c` to the build. The size is read from the chip's ID.

## 3b. Ethernet and RNG

| CubeMX | Setting |
|---|---|
| ETH | RMII mode. Pins as the DK schematic routes them to its LAN8742 PHY **(check: CubeMX's board preset normally sets these)**. Tx and Rx descriptor counts 4 |
| ETH NVIC | Global interrupt enabled, priority 7 (any value 5 or higher works with FreeRTOS) |
| RNG | Activated (handle `hrng`). Session tokens and password salts need it |

Do **not** enable LwIP or NetX Duo middleware in CubeMX (the H5 doesn't offer lwIP).
The platform brings its own lwIP configuration. Add to the project:

- lwIP 2.2.1 sources: `src/core/*.c`, `src/core/ipv4/*.c`, `src/api/*.c`,
  `src/netif/ethernet.c`, and `contrib/ports/freertos/sys_arch.c`, from
  https://github.com/lwip-tcpip/lwip (tag `STABLE-2_2_1_RELEASE`);
- the PHY driver `lan8742.c/.h` from https://github.com/STMicroelectronics/stm32-lan8742;
- `port/stm32h5/lwip/` (lwipopts.h, arch/cc.h, ethernetif.c, plat_net_lwip.c) and
  `platform/net/`, `platform/os/plat_net_tasks.c`;
- include paths: `lwip/src/include`, `lwip/contrib/ports/freertos/include`,
  `port/stm32h5/lwip`, the lan8742 folder.

CubeMX generates `HAL_ETH_MspInit` (the pins). The platform's `ethernetif.c` defines
`ETH_IRQHandler`, so untick its generation in NVIC → Code generation.

On first power-up the unit answers on 192.168.1.50 (web: http://192.168.1.50/, Modbus
TCP port 502). Change the address from the web page (admin) or, later, the touchscreen.

## 3e. Display (Riverdi RVT70HSBNWC00-B through a RiBUS breakout)

| CubeMX | Setting |
|---|---|
| SPIx | Full-duplex master, 8-bit, mode 0, NSS software. Its own SPI if possible (the UI task owns it) |
| GPIO | `EVE_CS` output, initial high; `EVE_PD` output, initial high (the BT817's PD_N) |

In `main.h` (USER CODE section), if the handle isn't `hspi3`, and the prescalers for your
SPI kernel clock (11 MHz or less at start-up, 30 MHz or less after):

```c
#define EVE_SPI_HANDLE hspi1
#define EVE_SPI_SLOW SPI_BAUDRATEPRESCALER_32
#define EVE_SPI_FAST SPI_BAUDRATEPRESCALER_8
```

Add `platform/ui/`, `platform/os/plat_ui_task.c` and `port/stm32h5/eve_bus_stm32h5.c`.
Bring-up order (spec): the chip ID (REG_ID = 0x7C) answers, then the first frame
shows. If the touch doesn't respond, the module's touch controller may need
`touch_config` in `eve_panel_rvt70h` (platform/ui/eve.c) **(check with Riverdi which
controller the uxTouch variant uses)**. If the picture is shifted or flickers, the
timing values came from Riverdi's library for this module and are worth checking
against its datasheet.

## 3f. HTTPS (mbedTLS)

Add `third_party/mbedtls/library/*.c` and `platform/net/tls.c` to the build, the include
paths `third_party/mbedtls/include`, `third_party/mbedtls/library` and `platform/net`,
and the preprocessor define `MBEDTLS_CONFIG_FILE="tls_config.h"` (Project →
Properties → C/C++ Build → Settings → MCU GCC Compiler → Preprocessor). The RNG must be
enabled (3b). Browse to `https://192.168.1.50/` and accept the certificate once, after
comparing its fingerprint with the touchscreen's System screen.

## 3g. RS-485 to the I/O boards (D27, optional since 24 Oct)

The I/O boards are on Ethernet by default (`iob_link`, `PLATFORM.md` 5k): nothing to set up in
CubeMX for that, the boards share the Ethernet port and lwIP above. lwIP's pools are sized for
the three board connections and the discovery socket (`port/stm32h5/lwip/lwipopts.h`). Only
for the RS-485 setting:

| CubeMX | Setting |
|---|---|
| USARTx (any free one; on the DK one brought out to the Arduino or STMod+ header) | Mode **Asynchronous**, Hardware Flow Control (RS485) **enabled**: the USART drives the transceiver's DE pin itself. 115200 baud, 8 bits, no parity, 1 stop bit, oversampling 16. Driver Enable polarity high; assertion and deassertion time 8 (sample times) |
| NVIC | USARTx global interrupt **enabled**, preemption priority 5 or higher number (FreeRTOS calls from the callback) |
| DMA | none needed |

In `main.h` (USER CODE section):

```c
#define BSP_RS485_HUART huart2      /* the handle CubeMX generated for that USART */
```

Add `port/stm32h5/rs485_stm32h5.c`, `platform/core/iobus.c`, `platform/core/mb_rtu.c` and
`platform/os/plat_iobus_task.c`. The driver defines `HAL_UART_TxCpltCallback`,
`HAL_UARTEx_RxEventCallback` and `HAL_UART_ErrorCallback`; if `main.c` defines any of them
too, move the body into one function that checks the handle. Without `BSP_RS485_HUART`
the build has no port and an enabled board reports "not responding".

On the DK, a 3.3 V RS-485 module (MAX3485 class: DI = TX, RO = RX, DE and /RE together on
the DE pin) is enough to talk to an I/O board or to a PC with a USB-RS485 adapter running
a Modbus slave simulator. To try it: Settings → `iob1_enable` = 1, then System → I/O boards.

## 3d. Firmware update

Nothing to configure in CubeMX beyond the HAL modules: FLASH and ICACHE (on by default).
Add `port/stm32h5/fw_flash_stm32h5.c` to the build. To produce update files:

1. Project → Properties → C/C++ Build → Settings → MCU Post build outputs → tick
   "Convert to binary file (-O binary)".
2. After a build: `python3 tools/mkfwimage.py Debug/<project>.bin cems-0.4.0.cfw --version 0.4.0`
   (keep `--version` the same as the version string in `plat_app_t.ident.revision`).
3. On the web page (factory/tech login): Administration → Firmware → choose the .cfw → Upload → "Restart on 0.4.0".

Signed images (D14), once, for the product, on the technicians' build machine:

    python3 tools/fwkey.py new ~/cems-keys/cems-fw-key.pem      # private key: never in the repository
    python3 tools/fwkey.py pubkey ~/cems-keys/cems-fw-key.pem > platform/core/fw_pubkey.c

Commit the new `fw_pubkey.c` (the public key only). From then on every build refuses
unsigned images, so sign each one: add `--key ~/cems-keys/cems-fw-key.pem` to step 2.
Check a file with `python3 tools/mkfwimage.py cems-0.4.0.cfw --check --pub $(python3 tools/fwkey.py hex ~/cems-keys/cems-fw-key.pem)`.
Back the key up somewhere safe: lose it and units in the field can't be updated except
with the debugger; leak it and anyone with a tech login can install their own firmware.

Serial numbers (D25): a second, separate key, kept by the developer only (not on the
technicians' machine):

    python3 tools/fwkey.py new ~/cems-dev/cems-dev-key.pem
    python3 tools/fwkey.py devpubkey ~/cems-dev/cems-dev-key.pem > platform/core/dev_pubkey.c

Commit `dev_pubkey.c`. Until then no build can change a serial number. To give a unit its
serial: web page (factory login) → Administration → Serial number → enter it → "Show text
to sign" gives the command line; run it on the developer's PC
(`python3 tools/serial_token.py --key ~/cems-dev/cems-dev-key.pem --uid <chip ID> --old <now> --new <serial>`)
and paste the token back. The token only works on that chip, once.

The first update on the DK should be done with the debugger attached (see
`docs/PLATFORM.md` 5b). The option byte SWAP_BANK stays as the update leaves it; to
return to a plain setup, clear it in STM32CubeProgrammer (Option bytes → User
configuration → SWAP_BANK).

## 4. FreeRTOS

1. Help → Manage embedded software packages → STMicroelectronics → install **X-CUBE-FREERTOS** (FreeRTOS V11.x).
2. Middleware → enable X-CUBE-FREERTOS (CMSIS-RTOS2 interface is not needed).
3. Set the values the platform requires (see `port/stm32h5/FreeRTOSConfig.h`, the REQUIRED lines):
   `configTICK_RATE_HZ 1000`, `configMAX_PRIORITIES 16`, `configSUPPORT_STATIC_ALLOCATION 1`,
   `configCHECK_FOR_STACK_OVERFLOW 2`, `configUSE_MALLOC_FAILED_HOOK 1`, `INCLUDE_xTaskDelayUntil 1`,
   `configTOTAL_HEAP_SIZE` 64 KB, `configMINIMAL_STACK_SIZE 512`, `configTIMER_TASK_STACK_DEPTH 1024`, `configTIMER_TASK_PRIORITY 11`. Also set `configASSERT` to call
   `plat_assert_failed(__FILE__, __LINE__)`.
4. Create **no** tasks in CubeMX: the platform creates its own.
5. NVIC → Code generation: make sure **SVC, PendSV and SysTick handlers are not generated** (the FreeRTOS port defines them). X-CUBE-FREERTOS normally unticks these itself; check after generating that `stm32h5xx_it.c` doesn't contain them.
   Untick the same "Generate IRQ handler" box for **Hard fault, Memory management fault, Prefetch fault / memory access fault (BusFault) and Undefined instruction / illegal state (UsageFault)**: `port/stm32h5/fault_stm32h5.c` defines these four (D4). Left ticked, the link fails with multiple definitions; with ST's empty loops instead, a fault would hang until the watchdog and leave no record.
6. If the generated code already defines `vApplicationGetIdleTaskMemory` / `vApplicationGetTimerTaskMemory`, keep them: the platform's versions are weak and give way.

## 5. Add the platform to the project

1. Copy (or link) `platform/` (except `platform/bsp/bsp_host.c` and `platform/bsp/fw_flash_sim.c`), `port/stm32h5/bsp_stm32h5.c`, `port/stm32h5/nvm_fram_i2c.c` (and `nvm_fram_spi.c` if an SPI part is used), `port/stm32h5/nor_spi.c`, `port/stm32h5/fw_flash_stm32h5.c`, `port/stm32h5/fault_stm32h5.c`, and for a first test `app_demo/`, into the project.
2. Include paths: `platform/include`, `platform/bsp`, `app_demo`.
3. Don't add `platform/bsp/bsp_host.c`, `host/`, `tests/` or `port/stm32h5/check/` to the target build. (`platform/bsp/nvm_ram.c` and `nor_sim.c` *are* needed: they are the RAM stand-ins.)
4. Linker script (`STM32H573IIKXQ_FLASH.ld` or similar): paste the contents of `port/stm32h5/noinit_section.ld` just before the `._user_heap_stack` section. Without it, the fault record is cleared at every boot.
5. In `main.c`:

```c
/* USER CODE BEGIN Includes */
#include "plat_tasks.h"
#include "app_demo.h"
/* USER CODE END Includes */
...
  /* USER CODE BEGIN 2 */
  plat_start(&app_demo_plat);     /* loads FRAM, creates all tasks, starts FreeRTOS, never returns */
  /* USER CODE END 2 */
```

If CubeMX also generates an `osKernelInitialize()` / `MX_FREERTOS_Init()` /
`osKernelStart()` block, `plat_start()` must come before it (it never returns), or that
block can be disabled.

## 6. First run: what you should see

- Hold the user button (DI_01) for 2 s and LED DO_01 comes on; release it and the LED goes off. That is the demo TON running on the 10 ms scan.
- In the debugger: `plat_scan.stats.scans` rises by 100 per second, `plat_scan.stats.max_exec_us` stays tiny for the demo, and `plat_diag.boots` counts warm resets.
- `param_store_status()` reads 0x001C on a bare DK (calibration invalid, user defaulted, RAM stand-in). With an FRAM fitted it reads 0x000C on the first boot and 0x0006 after a power cycle (user valid, calibration still invalid until a factory value is set).
- Watchdog test: put a breakpoint-free `for(;;);` in `plat_ui_task_body` (define it in a test file) and the unit resets after about 5 s, with `plat_diag.fault_name` = "ui". The debugger can stop the IWDG; enable `DBGMCU` IWDG freeze only if you want it frozen while halted.

## 7. Networking (next module, decision D3)

CubeMX does not offer lwIP for the STM32H5 (ST recommends NetX Duo). lwIP 2.2.1 for
the H5, with a FreeRTOS sys_arch and examples for this DK, is in ST's GitHub package
`STMicroelectronics/stm32h5-classic-coremw-apps` (EWARM projects only). The Modbus/HTTP
module will bring lwIP in from there as source.
