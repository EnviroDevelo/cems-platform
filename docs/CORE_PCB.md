# Core PCB: design specification (draft)

Rev 0.1, 25 Oct 2026. The CEMS main ("core") board: the STM32H5, its memories, the two
Ethernet ports, and the RVT70 touch display plugged straight onto it by FFC (no separate
HMI board). The field I/O is on the separate I/O boards (`PLATFORM.md` 5k) and is not part
of this board. What the firmware needs from the board is in `HARDWARE.md` (H1-H11); this
document turns that into a schematic-ready plan. Items marked **(check)** must be confirmed
against the part's datasheet before layout.

## 1. Block diagram

```
 24 VDC in ─┬─ fuse, reverse-polarity FET, TVS, filter
            └─ buck 24→5 V (1.5 A) ─┬─ 5 V: display backlight (BLVDD, FFC)
                                    └─ buck 5→3.3 V (2 A) ─ MCU, PHY, W5500, memories, display logic

                    ┌──────────────── STM32H573VIT6 (LQFP100) ────────────────┐
 Client RJ45 ═ magnetics ═ LAN8742A ══ RMII ══ ETH MAC   │ lwIP: HTTPS, Modbus TCP, email, SNTP
 I/O RJ45    ═ magnetics ═ W5500 ═══ SPI4 ═══════════════│ I/O bus only (hardware sockets)
 RVT70 display (BT817) ═ 20-pin FFC ═ SPI3 ══════════════│ UI task
 W25Q128JV NOR (event + data log) ═ SPI2 ════════════════│ store task
 MB85RC1MT FRAM 128 KB (settings) ═ I2C1 ════════════════│ store task
 32.768 kHz LSE, CR2032 on VBAT (RTC) ═══════════════════│
 SWD + SWO, BOOT0, reset ════════════════════════════════│
 RS-485 (not fitted by default) ═ USART2 ════════════════│
                    └──────────────────────────────────────────────────────────┘
```

## 2. Two separate networks (decided, Gary 25 Oct)

| | Client port (RJ45 1) | I/O port (RJ45 2) |
|---|---|---|
| Purpose | The customer's plant network: PLC, SCADA, web browser, email, time server | Internal link to the I/O boards only |
| Silicon | MCU's own Ethernet MAC on RMII + LAN8742A PHY (as on the DK; firmware driver exists) | WIZnet W5500 (MAC, PHY and TCP/IP in hardware) on its own SPI |
| Stack | lwIP; every network service listens here | W5500 hardware sockets: 3 TCP (one per I/O board) + 1 UDP (discovery, port 50223); nothing listens |
| Addressing | Customer's subnet, DHCP or static | Fixed private subnet set at commissioning |
| MAC address | From the chip UID (D10) | From the chip UID with a different low byte |

No bridging or routing between the two is possible: the I/O traffic never enters lwIP, and
lwIP does not know the I/O port exists. The I/O boards' data reaches the client side only as
the core's own values (platform image → HTTPS and Modbus), never as forwarded packets.

**Hardware hardening of the client port** (security and industrial EMC):
- Magnetics (integrated MagJack or discrete) rated 1500 Vrms isolation, Bob Smith
  termination to chassis through 1 nF / 2 kV.
- Low-capacitance ESD array on the PHY side of the magnetics (e.g. TI TPD4E05U06) **(check)**.
- RJ45 shield to the chassis-ground plane, which ties to logic ground only through
  1 MΩ ∥ 4.7 nF / 2 kV near the power input. The two Ethernet ports have separate chassis
  areas at their connectors.
- Nothing else on that connector: no PoE, no debug, no strap that changes the PHY mode.
- The I/O port gets the same protection. Its RJ45 is labelled **I/O ONLY** on the silkscreen
  and the enclosure, and should be a different LED colour or housing so that site
  technicians don't swap the cables. If they do, nothing is exposed: the W5500 has no
  listening sockets and the boards simply don't answer.

**Product lifecycle on the H573:** the production units are shipped with debug closed
(product state / RDP per ST's H5 security model) so SWD cannot read the firmware or keys
from a unit on site. The SWD header stays fitted for development units. **(check: which state, and
the recovery route, are a firmware-update decision; see `PLATFORM.md` 5b.)**

## 3. MCU

STM32H573VIT6 (2 MB dual-bank flash, 640 KB RAM, crypto, LQFP100); the STM32H563VIT6 is
pin-compatible if hardware crypto and secure boot aren't wanted (H8). The firmware is
485 KB flash and 438 KB RAM with the application (`MEMORY.md`), so an image fits in one
bank as the bank-swap update needs. LQFP100 gives 82 I/O; this board uses about 45, so there
are spares for later without a bigger package, and it can be hand-reworked on prototypes.

Clocks: HSE crystal 25 MHz on PH0/PH1 (**check** it matches the DK so the CubeMX clock tree
for 250 MHz SYSCLK carries over unchanged); LSE 32.768 kHz on PC14/PC15 with load
capacitors per ST AN2867 (H1). The LAN8742A and the W5500 each have their own 25 MHz crystal, separate from the MCU's HSE (nothing in the MCU clock tree depends on them, and the W5500 SPI clock comes from SPI4). W5500 SPI runs at up to about 30 MHz: keep its traces short (under 50 mm) with a 33 Ω series resistor on SCK and MOSI at the MCU.

## 4. Pin map

Alternate functions are taken from ST's pin data for the STM32H573VI (the same data
CubeMX uses). The SPI, I2C and UART instances are the firmware's defaults where it has one, so
`main.h` needs the fewest overrides.

| Function | Pins (AF) | Firmware name / note |
|---|---|---|
| ETH RMII REF_CLK | PA1 (AF11) | 50 MHz from the LAN8742A (REFCLKO mode) |
| ETH MDIO / MDC | PA2 / PC1 (AF11) | PHY address 0 (strap) |
| ETH CRS_DV | PA7 (AF11) | |
| ETH RXD0 / RXD1 | PC4 / PC5 (AF11) | |
| ETH TX_EN | PA5 (AF11) | |
| ETH TXD0 / TXD1 | PB12 / PB15 (AF11) | |
| PHY nRST | PD11 (GPIO out, initial low → high) | `ETH_PHY_RST` |
| Display SPI3 SCK / MISO / MOSI | PC10 / PC11 / PC12 (AF6) | `hspi3`, the EVE default |
| Display CS | PA15 (GPIO out, initial high) | `EVE_CS` |
| Display PD_N | PD2 (GPIO out, initial high) | `EVE_PD` |
| Display INT_N | PD0 (EXTI0) | `EVE_INT` (routed; the driver polls today). The module has its own 47 kΩ pull-up |
| W5500 SPI4 SCK / MISO / MOSI | PE12 / PE13 / PE14 (AF5) | own bus, not shared (I/O timing) |
| W5500 SCSn | PE11 (GPIO out, initial high) | `W5500_CS` |
| W5500 INTn | PE9 (EXTI9, pull-up) | `W5500_INT` (routed; the driver polls today) |
| W5500 RSTn | PE10 (GPIO out, initial low → high) | `W5500_RST` |
| NOR SPI2 SCK / MISO / MOSI | PB13 / PB14 / PC3 (AF5) | `hspi2`, the NOR default |
| NOR CS | PD10 (GPIO out, initial high) | `NOR_CS`; WP# and HOLD# pulled up |
| FRAM I2C1 SCL / SDA | PB6 / PB7 (AF4) | `hi2c1`, the FRAM default; 2.2 kΩ pull-ups |
| RS-485 USART2 TX / RX / DE | PD5 / PD6 / PD4 (AF7) | `huart2`; transceiver not fitted by default (H11) |
| Service UART USART1 TX / RX | PA9 / PA10 (AF7) | 3.3 V header for bench logging |
| SWD SWDIO / SWCLK / SWO | PA13 / PA14 / PB3 | 10-pin 1.27 mm Cortex debug header with NRST |
| BOOT0 | BOOT0 pin | 10 kΩ pull-down, test pad / jumper to 3.3 V (H9) |
| 24 V supply sense | PC0 (ADC1_INP10) | divider 100 k / 10 k; early power-fail warning, supply shown on the System screen |
| Status LEDs | PD12 (run), PD13 (fault) | `DO_01`, `DO_02` (labelled outputs, active high) |
| Hardware revision straps | PE7, PE8 | read once at boot |

Free for later: PA0, PA3, PA4, PA6, PA8, PA11, PA12, PB0-PB2, PB4, PB5, PB8-PB11, PC2,
PC6-PC9, PC13, PD1, PD3, PD7-PD9, PD14, PD15, PE0, PE2-PE6, PE15.

## 5. Power

| Rail | Source | Loads | Budget |
|---|---|---|---|
| 24 VDC in (18-32 V) | Pluggable terminal (+, 0 V, FE) | | about 0.3 A at 24 V |
| Input protection | Fuse 1 A slow (or PTC), P-channel FET reverse-polarity, TVS SMBJ33A, LC filter | | |
| 5 V | Buck, 60 V rated input (e.g. TI LMR36015, 1.5 A) | Display backlight BLVDD (FFC pins 17-18): 400 mA at full brightness; the 3.3 V buck about 450 mA | about 0.9 A |
| 3.3 V | Buck from 5 V (e.g. TI TLV62569, 2 A) | MCU about 120 mA, display logic 240 mA max (235 typ; datasheet), W5500 130 mA, LAN8742A 50 mA, NOR, FRAM | about 0.6 A |
| VBAT | CR2032 in a holder (H1) | RTC, backup registers | µA |
| VDDA / VREF+ | 3.3 V through a ferrite and 1 µF + 100 nF | ADC (supply sense) | |

The 60 V input rating is there because the TVS clamps at about 53 V during a surge; a 30 V
buck would not survive one. Display figures are from the RVT70HSBNWC00 V1.1A datasheet Rev 1.1 (27 Jul 2026):
VDD 3.0-3.6 V, 235 mA typ / 240 mA max (audio on at full volume); BLVDD 4.5-5.5 V (5.0 typ;
it still lights from 2.7 V but not at full brightness), 10 / 310 / 400 mA at minimum / 50 % /
100 % backlight. A 5 % 5 V rail is within range; keep the FFC's 5 V drop under 0.25 V. Brown-out: the MCU's BOR at the highest level
plus the supply-sense pin (H10). The core board drives no field outputs (H6 and H7 are on the I/O boards).

## 6. Connectors

| Ref | Connector | Notes |
|---|---|---|
| J1 | Power in, 3-way pluggable terminal 5.08 mm | +24 V, 0 V, functional earth to chassis |
| J2 | Client RJ45, shielded, with LEDs | LAN8742A, see section 2 |
| J3 | I/O RJ45, shielded, with LEDs | W5500, labelled I/O ONLY |
| J4 | Display FFC, RiBUS, ZIF 20-pin, 0.5 mm pitch | See section 6a |
| J5 | SWD, 10-pin 1.27 mm Cortex debug | SWDIO, SWCLK, SWO, NRST, 3.3 V, GND |
| J6 | Service UART, 6-pin 2.54 mm (FTDI-cable order) | 3.3 V levels |
| J7 | RS-485, 4-way terminal (not fitted) | A, B, 0 V, shield; isolated transceiver, TVS, switchable 120 Ω (H11) |
| BT1 | CR2032 holder | |
| SW1 | Reset button | |

## 6a. Display FFC (J4), from the RVT70 datasheet

ZIF 20-pin, 0.5 mm pitch. The display's connector is down-side contact; the core board's
connector contact side has to suit the cable the enclosure uses (same-side or opposite-side
FFC) **(check at mechanical layout)**.

| Pin | Display signal | Core board |
|---|---|---|
| 1 | VDD 3.3 V | 3.3 V rail, 10 µF + 100 nF at the connector |
| 2 | GND | GND |
| 3 | SPI_SCLK | PC10 (SPI3_SCK), 33 Ω series at the MCU |
| 4 | MISO / IO.1 | PC11 (SPI3_MISO) |
| 5 | MOSI / IO.0 | PC12 (SPI3_MOSI), 33 Ω series |
| 6 | CS | PA15 (`EVE_CS`) |
| 7 | INT (active low, 47 kΩ pull-up in the module) | PD0 (`EVE_INT`, EXTI0) |
| 8 | RST/PD (active low, 47 kΩ pull-up in the module) | PD2 (`EVE_PD`) |
| 9 | GPIO.0 | test pad, not connected |
| 10 | DISP_AUDIO | test pad, not connected (the module's own 1.5 W amplifier drives a speaker on its P2) |
| 11, 12 | GPIO.1/IO.2, GPIO.2/IO.3 (QSPI data 2, 3) | test pads; single SPI is enough (D24) |
| 13-16 | NC | not connected |
| 17, 18 | BLVDD | 5 V rail, 10 µF + 100 nF at the connector |
| 19, 20 | BLGND | GND |

The display's operating range is -20 to 70 °C (storage -30 to 80 °C): it, not the core
board's -40 to 85 °C parts, sets the instrument's rating.

## 7. Layout notes

- 4 layers: signal / solid ground / power / signal. RMII at 50 MHz and two 100 Ω Ethernet
  pairs need the unbroken ground under them.
- RMII traces short and length-matched (± 5 mm is plenty at 50 MHz), the REF_CLK
  point-to-point with a series resistor at the PHY. Keep RMII away from the SPI buses.
- Under each MagJack: no copper on the line side except chassis; the two ports' chassis
  areas don't touch.
- The W5500's SPI and the display's SPI are on opposite sides of the MCU (PE and PC/PD pins),
  which keeps the high-speed buses apart naturally.
- **Mounting (Gary, 25 Oct): DIN rail, behind the display.** The board sits in a DIN-rail
  enclosure on the rail inside the panel, and the display (179.96 × 119.00 × 13.72 mm,
  without the mounting metal frame) is in the panel door or front, linked by the RiBUS FFC.
  The outline follows the chosen DIN enclosure; the FFC connector goes on the board edge
  that faces the display, with a slot in the enclosure for the cable.
- FFC length: keep it as short as the panel allows, 150 mm or less. RiBUS has only three
  ground pins (2, 19, 20) and no ground between the SPI lines, so a long cable limits the SPI
  clock. The firmware's 30 MHz fast rate is to be proved on the real cable; it can drop to
  15 MHz with no visible effect on the screens. Two BLVDD conductors carry the 400 mA with
  well under 0.1 V drop at that length. If the door has to be further away, a small
  buffer/adapter board at the display is the fallback (not planned).
- Decoupling per ST's hardware getting-started application note for the STM32H5 **(check)**: 100 nF per VDD pin,
  4.7 µF bulk, and the VCAP capacitors at the values the datasheet gives.

## 8. Decisions and open questions

1. **Supply: 24 VDC (Gary, 25 Oct)**, 18-32 V range, the same as the I/O boards' field supply.
2. **Mounting: DIN rail, behind the display (Gary, 25 Oct)**; see section 7.
3. **No system-OK relay (Gary, 25 Oct):** the PLC's Modbus communication watchdog is
   enough to detect a core board that has stopped.
4. **RS-485 footprint:** open, explained to Gary.
5. **Ethernet magnetics:** open. Gary answered "None"; being confirmed. Magnetics (or a
   MagJack) can't be left out on a port that goes off the board: they give the 1500 Vrms
   isolation and the common-mode rejection the client network needs.

## 9. BOM summary (main parts)

| Part | Function | Qty |
|---|---|---|
| STM32H573VIT6 (or H563VIT6) | MCU | 1 |
| LAN8742A-CZ-TR (-40 to 85 °C) | Client Ethernet PHY | 1 |
| W5500 (-40 to 85 °C) | I/O Ethernet controller | 1 |
| Shielded RJ45 MagJack with LEDs, 1500 Vrms | J2, J3 | 2 |
| MB85RC1MT | 128 KB FRAM, settings | 1 |
| W25Q128JV | 16 MB NOR, event and data log | 1 |
| 25 MHz crystals | MCU HSE, PHY, W5500 | 3 |
| 32.768 kHz crystal | RTC | 1 |
| LMR36015 (or equal, 60 V in) | 24 → 5 V buck | 1 |
| TLV62569 (or equal) | 5 → 3.3 V buck | 1 |
| SMBJ33A, P-FET, fuse | Input protection | 1 each |
| TPD4E05U06 (or equal) | Ethernet ESD | 2 |
| 20-pin FFC connector (RiBUS) | Display | 1 |
| CR2032 holder | VBAT | 1 |
