# The NUCLEO-H563ZI as an I/O node (STM32CubeIDE)

The Nucleo runs the **same firmware source** as the DK, in the NODE role (`platform/include/plat_role.h`).
It has no role jumper, so the build fixes the role with a symbol. At power-up it starts the node
runtime (`platform/include/plat_node.h`) instead of the display, web and plant Modbus: the node core
(`node/node.c`, formerly the io-node repository) answers the CORE's Modbus TCP on port 502 and its
discovery on UDP 50223, exactly as the I/O board will.

This page lists only what differs from the DK's `docs/CUBEMX_SETUP.md`. Follow that page for
everything else (stacks, SysTick, FreeRTOS, the platform sources, the linker script, -O2 for mbedTLS).

## 1. Project

STM32CubeMX → New Project → Board Selector → **NUCLEO-H563ZI** → **without TrustZone**, then
**Unselect All** in the Board Project Options (no BSP drivers). Project name **`CEMS_NODE_NUCLEO`**, next to
`CEMS_CORE`. Toolchain STM32CubeIDE. Linker settings as the DK: Minimum Stack 0x2000, Heap 0x1000.

## 2. System core

As the DK (SysTick time base, board default clocks, 250 MHz). RTC on LSE: the Nucleo fits the
32.768 kHz crystal (X2) **(check that the board preset enabled LSE)**. IWDG prescaler 32, reload 999.
ICACHE on. MPU: keep the board preset's region for the OTP/UID area; if the preset has none, add
region 0 over 0x08FFF000-0x08FFFFFF, not cacheable, read-only (the UID read in `bsp_chip_uid`).

## 3. Pins

| Label (user label in the pinout) | Pin | What | Mode |
|---|---|---|---|
| `DO_01` | PB0 | LD1 green = node output 1 | GPIO output, push-pull |
| `DO_02` | PF4 | LD2 yellow = node output 2 | GPIO output, push-pull |
| `DO_03` | PG4 | LD3 red = node output 3 | GPIO output, push-pull |
| `DI_01` | PC13 | blue button B1 = node input 1 | GPIO input, no pull (the board has one) |

The LEDs and the button are active high on this board, so no `_ACTIVE_LOW` defines. Node outputs
4-16 and the relays (node outputs 17-18 = `DO_17`, `DO_18`) and inputs 2-8 (`DI_02`..`DI_08`) can be
added on any free header pin with the same labels; unlabelled ones simply aren't there.

No `ROLE_A`/`ROLE_B` labels (the role is fixed below), no FRAM, no display, no NOR, no W5500, no
RS-485, no supply sense.

## 3b. Ethernet and RNG

| CubeMX | Setting |
|---|---|
| ETH | RMII, the board preset's pins to the on-board LAN8742A: PA1 REF_CLK, PA2 MDIO, PC1 MDC, PA7 CRS_DV, PC4 RXD0, PC5 RXD1, PG11 TX_EN, PG13 TXD0, PB15 TXD1 **(check against the preset)**. Tx and Rx descriptors 4. NVIC: global interrupt on, priority 7, IRQ handler **not** generated (`ethernetif.c` has it). `MX_ETH_Init` is not called |
| RNG | Activated (`hrng`) |

lwIP, lan8742 and the platform sources exactly as the DK (`port/stm32h5/check.mk` `SRCS`, which now
include `platform/core/plat_role.c`, `platform/net/plat_node.c`, `node/node.c` and `node/node_net.c`), plus
the include path **`node`**.

## 4. Symbols (the only real difference)

Project → Properties → C/C++ General → Paths and Symbols → Symbols, for all configurations:

| Symbol | Value | Why |
|---|---|---|
| `PLAT_ROLE_FIXED` | `2` | the NODE role (no jumper on this board) |
| `PLAT_NODE_DEFAULT_IP` | `0xC0A8013Du` | 192.168.1.61/24 with nothing saved: board 1's default IP on the CORE, so it answers straight away |
| `PLAT_FRAM_SIZE` | `32768u` | no FRAM: the RAM stand-in, as on the bare DK |

The chip define (`STM32H563xx`) comes from CubeMX. `main.c` is the same as the DK's:
`plat_start(&app_demo_plat);` in USER CODE 2. The demo application's tables are loaded, but in the
NODE role its logic never runs.

## 5. What you should see

- After a power-up: no display, no web page. `ping 192.168.1.61` answers.
- Nothing is lit until the CORE talks to it: the node core keeps every output off until the CORE's
  first valid output write, and turns them all off again if the CORE goes quiet for the hold time
  (5 minutes by default, the CORE sets it).
- On the DK (CORE, admin): iob_link (102) = 1, iob1_ip (146) = 192.168.1.61 (the default),
  iob1_enable (110) = 1 → I/O boards tab: board 1 online, firmware 0.1, hardware 90 (a bench board).
- The blue button is node input 1 (the DK's demo start input with the default channel mapping);
  set iob1 first output channel (112) = 1 and after the start delay LD1 follows.
- Address, serial number and IP settings given by the CORE ("Find boards", set IP) are kept in the
  backup registers: over a reset yes, over a power-off no (then the defaults above again).

## 6. Tests on the PC

`make build/netsim build/nodesim && python3 tests/integration/node_role_test.py` runs the CORE
(netsim) against the node runtime (nodesim, the same `plat_node.c` and node core) over sockets.
