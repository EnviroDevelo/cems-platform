# Bringing the platform up on the STM32H573I-DK (STM32CubeIDE)

These steps set up the CubeIDE project that the platform sources drop into. The
platform needs four things from CubeMX: the HAL, RTC, IWDG, the GPIO
labels and FreeRTOS. Everything else stays in the platform files.

Check each value marked **(check)** against the DK user manual or schematic before
relying on it. Those are the ones I couldn't confirm from the files here.

## 1. Project

1. **STM32CubeMX** (standalone: from STM32CubeIDE 2.0 the IDE no longer has CubeMX built in, and its New STM32 Project wizard makes an empty project) → New Project → Board Selector → **STM32H573I-DK**. In the Board Project Options dialog press **Unselect All** (no ST BSP drivers: the platform drives the LEDs and button by pin label) and leave "Generate demonstration code" off. Project Manager → Toolchain/IDE **STM32CubeIDE**; Generate Code, then in CubeIDE File → Import → Existing Projects into Workspace. (First bring-up, 28 Oct: project `CEMS_CORE`.)
2. When asked about TrustZone, choose **without TrustZone** (the platform uses the `ARM_CM33_NTZ` FreeRTOS port).
3. Don't initialise all peripherals to their default mode. Add only the ones below, so the project stays readable.
4. **Memory for stacks (Gary, 25 Oct: stack overflows on earlier DK prototypes; set these large from the start).** Project Manager → Project → Linker Settings: **Minimum Stack Size 0x2000** (8 KB, the main stack: interrupts, and start-up before the scheduler, which includes the HTTPS key generation on first boot) and **Minimum Heap Size 0x1000** (4 KB, the C library's heap). ST's defaults are 0x400 and 0x200, too small. These are minimums the linker checks; the build fails rather than run short.
   The task stacks are not set in CubeMX: the platform allocates them itself (`plat_config.h` `PLAT_STACK_*`, 85 KB in all, about twice the first sizes), as it does the FreeRTOS heap (64 KB, section 4) and the idle and timer task stacks (`FreeRTOSConfig.h`). Leave CubeMX's FreeRTOS task list empty. A task that still runs past its stack is stopped by the CPU's stack limit and the next boot names it ("stk scan" on the System screen; `PLATFORM.md` D4). Trim the sizes only after the 72-hour soak on hardware, from the margins shown under `health` on `/api/info`.

## 2. System core

| Peripheral | Setting |
|---|---|
| SYS | Timebase source: **SysTick** (the default). Not a TIM: on the DK a TIM6 timebase froze the start-up after a power-up (28 Oct). `bsp_stm32h5.c` supplies `HAL_InitTick`/`HAL_GetTick` (the DWT cycle counter before the scheduler, the FreeRTOS tick after), so CubeMX's SysTick setup is never used and FreeRTOS keeps SysTick |
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
`touch_config` in `eve_panel_rvt70h` (platform/ui/eve.c). The uxTouch variant uses the ILITEK
ILI2132A; Riverdi's riverdi-eve-lib (IPS_70) leaves REG_TOUCH_CONFIG at the BT817's default
(controller auto-detect), and so does `touch_config = 0` here (checked 25 Oct, along with the
timing, PCLK, GPIOX and REG_FREQUENCY values, which match the library). If the picture is shifted or flickers, the
timing values came from Riverdi's library for this module and are worth checking
against its datasheet.

## 3f. HTTPS (mbedTLS)

Add `third_party/mbedtls/library/*.c` and `platform/net/tls.c` to the build, the include
paths `third_party/mbedtls/include`, `third_party/mbedtls/library` and `platform/net`,
and the preprocessor define `MBEDTLS_CONFIG_FILE="tls_config.h"` (Project →
Properties → C/C++ Build → Settings → MCU GCC Compiler → Preprocessor). The RNG must be
enabled (3b). Browse to `https://192.168.1.50/` and accept the certificate once, after
comparing its fingerprint with the touchscreen's System screen.

## 3h. The I/O port: W5500 for the I/O boards (HARDWARE.md H5, 25 Oct)

The I/O boards are on their own network, on the I/O port: a WIZnet W5500 on its own SPI, which
runs TCP/IP itself, so this traffic never enters lwIP (`iob_link` = 2, the default;
`PLATFORM.md` 5k). Product board pins (PCB thread, `docs/CORE_PCB.md` 4):

| CubeMX | Setting |
|---|---|
| SPI4 (PE12 SCK, PE13 MISO, PE14 MOSI) | Full-Duplex Master, 8 bits, Motorola, MSB first, CPOL low, CPHA 1 edge (mode 0), NSS **software**. Prescaler for **at most 33 MHz** from SPI4's kernel clock **(check against the clock tree)**. No DMA, no interrupt |
| PE11 | GPIO output, label **W5500_CS**, initial level **high**, push-pull, speed high |
| PE10 | GPIO output, label **W5500_RST**, initial level **high** (running), push-pull |
| PE9 | GPIO input, label **W5500_INT**, pull-up (the chip's INTn; reserved for an interrupt-driven wait, not used yet) |

In `main.h` (USER CODE section), only if the handle is not `hspi4`:

```c
#define W5500_SPI_HANDLE hspi2      /* the handle CubeMX generated for the W5500's SPI */
```

Add `port/stm32h5/w5500_stm32h5.c`, `platform/core/w5500.c` and `platform/core/iob_w5500.c`.
Without the `W5500_CS` label there is no I/O port (`bsp_w5500()` returns NULL): with
`iob_link` = 2, every enabled board then reports "not responding" and `iob_port_status`
(parameter 105) reads 0.

Settings: `iob_port_ip` (103, default 192.168.1.60) and `iob_port_prefix` (104, default 24) are
the I/O port's own address. It is a separate network with its own stack, so it may use the same
numbers as the client network without any clash; nothing routes between the two. The port's MAC
is the main port's with bit 1 of the last byte flipped. `iob_port_status` shows the chip found
(1) and the cable in (2).

**On the DK** (no W5500 on the board): either wire a W5500 module (WIZ850io or any W5500
breakout, 3.3 V) to a free SPI with the three GPIO labels above, or set `iob_link` = 1 to run the
boards on the DK's own Ethernet port through lwIP (the same protocol; the boards then share the
client network).

## 3i. Supply sense: the 24 V supply on PC0 (CIP-07, CORE_PCB.md 12)

The core board divides the 24 V supply (after the reverse diode) by 13.20 onto PC0. The firmware
reads it every ms, averages 16 readings, raises alarm 20 SUPPLY outside 19-30 V and, below
16 V, stops starting FRAM and log-flash writes so the brown-out finds nothing half-written
(`PLATFORM.md` 5l).

| CubeMX | Setting |
|---|---|
| ADC1 | 12-bit, right aligned, **scan conversion mode enabled**, 2 regular conversions, software trigger, continuous **off**, DMA **off**, overrun: data overwritten |
| Rank 1 | Channel of **PC0** (ADC1_INP10), sampling time 47.5 cycles or more |
| Rank 2 | **Vrefint** channel, sampling time 47.5 cycles or more (it needs the long sample) |
| PC0 | label **SUPPLY_SENSE** (CubeMX sets it analog) |
| ADC clock | any, up to the datasheet maximum; one reading is two conversions, a few µs |

In `main.h` (USER CODE section), only if they differ:

```c
#define SUPPLY_ADC_HANDLE   hadc2   /* if not hadc1 */
#define SUPPLY_DIVIDER_X100 1320    /* the divider ratio x 100 */
```

Add `port/stm32h5/supply_stm32h5.c` and `platform/core/plat_power.c`. Without the `SUPPLY_SENSE`
label the board does not measure it: no SUPPLY alarm, no power-fail hold, parameter 305 reads 0.
**On the DK**, PC0 is not wired to a supply divider: leave the label off, or feed PC0 from a
potentiometer (0-2.4 V reads as 0-31.7 V) to try the alarm and the hold. Check the reading
against a meter on the first core board (the VREFINT calibration value is taken at 3.3 V; the
reading is about 0.4 V below the terminal voltage because of the diode).

## 3j. Board revision straps: PE7 and PE8 (CIP-01.3, CORE_PCB.md 12)

| CubeMX | Setting |
|---|---|
| PE7 | GPIO_Input, **no pull-up or pull-down** (the board has the resistors), label **HW_REV0** |
| PE8 | GPIO_Input, no pull, label **HW_REV1** |

The firmware reads them once at start (`bsp_hw_straps` in `supply_stm32h5.c`): revision =
PE8 x 2 + PE7, shown as parameter 319 `board_rev` and `/api/info` `board_rev`. A firmware image
whose header names other board revisions is refused (`FW_E_HARDWARE`), and alarm 22 HW_REV
(warning) says when the straps disagree with the factory `hw_rev`. Without the labels the board
has no straps: nothing is checked. **On the DK** leave them off (PE7/PE8 float there).

**Build ID.** Add `PLAT_BUILD_ID="<git short hash>"` to the compiler's defines (Project >
Properties > C/C++ Build > Settings > MCU GCC Compiler > Preprocessor), for example from a
pre-build step; it shows as `/api/info` `build` and goes in the image header with
`tools/mkfwimage.py --build-id`. Without it the build says "dev".

## 3k. External PSRAM: APS6404L on OCTOSPI1 (CORE PCB only, Gary 29 Oct)

The CORE PCB carries an 8 MB QSPI PSRAM (U10, AP Memory APS6404L-3SQR). The DK has none: leave
all of this out of the DK project, and the firmware keeps every buffer on chip.

| CubeMX | Setting |
|---|---|
| OCTOSPI1 | Mode **Quad SPI**, Port 1, chip select NCS1. Pins **PB2** CLK (AF9), **PB10** NCS (AF9), **PB1** IO0 (AF6), **PB0** IO1 (AF6), **PC2** IO2 (AF9), **PA6** IO3 (AF6), speed Very High. Parameters: any (the driver sets them) |
| DCACHE1 | Activated (caches the PSRAM region; nothing else uses it) |
| Clock | OCTOSPI1 kernel clock = HCLK (250 MHz); the driver divides by 4 |
| main.h (USER CODE) | `#define PSRAM_XSPI_HANDLE hospi1` |
| Preprocessor | `PLAT_PSRAM=1` |

Linker script (`STM32H573VITX_FLASH.ld`): add to MEMORY
`PSRAM (rw) : ORIGIN = 0x90000000, LENGTH = 8M`, and paste `port/stm32h5/extram_section.ld`
just before `._user_heap_stack`, after the `.noinit` block (the compile check does the same).

What happens: `bsp_init` maps the chip (`psram_stm32h5.c`), reads its ID, tests every address and
data line, and zeroes the `.extram` section. The TLS pool (80 KB) and the settings store's
staging block (45 KB) are placed there (`PLAT_EXTRAM`), so the MCU keeps about 300 KB free. If the
check fails, the unit stops at start-up with outputs safe and fault "psram" in the fault record.
**Before the PCB:** the same chip on a SOIC-8 breakout wired to a NUCLEO-H563ZI on the same pins
(the DK's OCTOSPI1 belongs to its own flash) runs this code unchanged (H563 and H573 share the
OCTOSPI). On flying leads add `PSRAM_CLK_PRESCALER=24u` (10 MHz) to the defines, and open the
Nucleo's solder bridge to the LED on PB0. Bench checks on the first board: an HTTPS handshake time against the DK's (the TLS pool is in the
PSRAM; `TLS_POOL_PLACE=` empty puts it back on chip), and a backup/restore.

## 3l. The I/O node's field chips (CORE PCB rev 0.2 in the node role, CORE_PCB.md 14)

One image serves both roles (J7). On the CORE PCB the node role drives the real field chips through
`node/drivers` on SPI4, shared with the W5500. Leave these out for the DK and the Nucleo; without
the `CS_A` label the node falls back to the platform's pins (the Nucleo bench).

- **Sources:** add `node/drivers/*.c`, `port/stm32h5/node_io_stm32h5.c` and `port/stm32h5/spi4_bus.c`;
  include paths `node/drivers` and `port/node_stm32g0` (for `node_port.h`).
- **SPI4** stays as in 3h (mode 0, the W5500's clock). The firmware switches the mode and divider per
  transaction: 4 MHz for the field chips, mode 1 for the ADS124S08 (`spi4_bus.h`).
- **GPIO outputs, initial high:** `CS_A` PE2, `CS_B` PE3, `CS_IN` PE4, `CS_T` PE15, `CS_AI` PE0.
- **GPIO outputs, initial low:** `GATE_EN` PB8, `WDI` PB9, `PUMP_DIR` PC6, `PUMP_EN` PC7.
- **Open-drain outputs, initial high (relays off):** `REL1_N` PB4, `REL2_N` PB5.
- **Inputs, pull-up:** `A_ERR` PD1, `IN_ERR` PC13, `B_FAULT` PD14, `B_PGOOD` PD15, `GATE_FB` PD8.
- **TIM1** CH1 PWM on PA8 (AF1), prescaler set so the counter runs at **1 MHz**; leave it stopped (the
  firmware sets ARR, CCR1 and MOE). **TIM2** CH1 input capture on PA0 (AF1), rising edge, 1 MHz, 32-bit
  free running, no interrupt.
- **The gate:** WDI toggles on every node pass in which the node core is healthy and its gate open,
  and GATE_EN follows that gate, so the MAX6369 (30 ms) drops every output if the iobus task stalls.
  To check on the first board: the MAX6369 start-up timeout against the time to the first command
  (the gate must open once kicks start), and GATE_FB against GATE_EN (`bsp_node_gate_fault`).
- **Not yet:** the AD4112 analog inputs (no node registers for them yet; `CS_AI` is held high).

## 3g. RS-485 to the I/O boards (D27, optional since 24 Oct)

The I/O boards are on Ethernet by default, on the I/O port (3h); `iob_link` = 1 puts them on the
main port through lwIP instead, with nothing more to set up in CubeMX (lwIP's pools are sized for
the three board connections and the discovery socket, `port/stm32h5/lwip/lwipopts.h`). Only
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

Behind the first-stage bootloader (docs/BOOTLOADER.md 4) the application is linked at
0x08008000 with `VECT_TAB_OFFSET 0x00008000U`; nothing else changes here.

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

1. Link into the project exactly the sources that `port/stm32h5/check.mk` lists in `SRCS` (platform core, os, net, ui, the stm32h5 port, lwIP, mbedTLS, lan8742, FreeRTOS with `portable/MemMang/heap_4.c`), except `port/stm32h5/check/check_main.c`, and its include paths except `port/stm32h5/check`. That list is the one the compile and link checks keep current; the HAL and CMSIS come from the CubeMX project. In the .ioc enable at least ETH (RMII; `MX_ETH_Init` is not called, `ethernetif.c` initialises it), RNG, RTC, IWDG and ICACHE. Keep the board preset's MPU region that makes the OTP/UID area non-cacheable.
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

**Debug builds:** set `third_party/mbedtls` and `platform/net/tls.c` to -O2 even in the Debug configuration (Properties on the folder/file → C/C++ Build → Settings → Optimization). At -O0 each HTTPS handshake took 0.8-2.8 s on the DK, and a browser's six parallel connections then timed out ("Connection lost", logins reported as wrong password); at -O2 it is 0.24 s (28 Oct).

**After flashing:** power-cycle the board (unplug the USB) before testing the network. After an ST-LINK reset or a flash the DK's peripheral interrupts stayed off until a real power-up (Ethernet answered a few frames, then went deaf), and the debugger's view of the NVIC on this part reads as zero even when the CPU has the interrupt enabled, so don't diagnose interrupts from SWD reads alone.

## 6. First run: what you should see

- Hold the user button (DI_01) for 2 s and LED DO_01 comes on; release it and the LED goes off. That is the demo TON running on the 10 ms scan.
- In the debugger: `plat_scan.stats.scans` rises by 100 per second, `plat_scan.stats.max_exec_us` stays tiny for the demo, and `plat_diag.boots` counts warm resets.
- `param_store_status()` reads 0x001C on a bare DK (calibration invalid, user defaulted, RAM stand-in). With an FRAM fitted it reads 0x000C on the first boot and 0x0006 after a power cycle (user valid, calibration still invalid until a factory value is set). `/api/info` "fram" shows the bytes and chips found: the I2C driver scans 0x50-0x57 and joins every MB85RC it finds, so keep chips at the same addresses once settings are stored.
- Watchdog test: put a breakpoint-free `for(;;);` in `plat_ui_task_body` (define it in a test file) and the unit resets after about 5 s, with `plat_diag.fault_name` = "ui". The debugger can stop the IWDG; enable `DBGMCU` IWDG freeze only if you want it frozen while halted.

## 7. Networking (next module, decision D3)

CubeMX does not offer lwIP for the STM32H5 (ST recommends NetX Duo). lwIP 2.2.1 for
the H5, with a FreeRTOS sys_arch and examples for this DK, is in ST's GitHub package
`STMicroelectronics/stm32h5-classic-coremw-apps` (EWARM projects only). The Modbus/HTTP
module will bring lwIP in from there as source.
