# Core PCB: design specification (draft)

Rev 0.2, 31 Oct 2026 (rev 0.1, 25 Oct). The CEMS main ("core") board: the STM32H5, its
memories, the two Ethernet ports, and the RVT70 touch display plugged straight onto it by
FFC (no separate HMI board). Since rev 0.2 the same board also carries the I/O node section
(Gary, 30 Oct: one PCB for CORE and I/O node, role set by jumper J7, section 12); the field
I/O is described in section 14. What the firmware needs from the board is in `HARDWARE.md` (H1-H11); this
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
 APS6404L PSRAM 8 MB (working RAM) ═ OCTOSPI1 (quad) ════│ memory-mapped buffers
 32.768 kHz LSE, CR2032 on VBAT (RTC) ═══════════════════│
 SWD + SWO, BOOT0, reset ════════════════════════════════│
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
- RJ45 with integrated magnetics (MagJack, Gary 25 Oct) rated 1500 Vrms isolation, Bob Smith
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

**Product lifecycle on the H573:** production units ship with debug closed, so SWD cannot
read the firmware or keys from a unit on site. The state and the recovery route are in
section 13.

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
| SPI4 SCK / MISO / MOSI | PE12 / PE13 / PE14 (AF5) | shared by the W5500 and the five I/O chips (section 14); I/O chips at 4 MHz, the firmware sets the prescaler per device |
| W5500 SCSn | PE11 (GPIO out, initial high) | `W5500_CS` |
| W5500 INTn | PE9 (EXTI9, pull-up) | `W5500_INT` (routed; the driver polls today) |
| W5500 RSTn | PE10 (GPIO out, initial low → high) | `W5500_RST` |
| NOR SPI2 SCK / MISO / MOSI | PB13 / PB14 / PC3 (AF5) | `hspi2`, the NOR default |
| NOR CS | PD10 (GPIO out, initial high) | `NOR_CS`; WP# and HOLD# pulled up |
| FRAM I2C1 SCL / SDA | PB6 / PB7 (AF4) | `hi2c1`, the FRAM default; 2.2 kΩ pull-ups |
| PSRAM OCTOSPI1 CLK / NCS | PB2 (AF9) / PB10 (AF9) | 33 Ω series on CLK at the MCU; 10 kΩ pull-up on NCS (deselected through reset) |
| PSRAM OCTOSPI1 IO0 / IO1 / IO2 / IO3 | PB1 (AF6) / PB0 (AF6) / PC2 (AF9) / PA6 (AF6) | quad mode, memory-mapped; the RAM thread owns the driver |
| Service UART USART1 TX / RX | PA9 / PA10 (AF7) | 3.3 V header for bench logging |
| SWD SWDIO / SWCLK / SWO | PA13 / PA14 / PB3 | 10-pin 1.27 mm Cortex debug header with NRST |
| BOOT0 | BOOT0 pin | 10 kΩ pull-down, test pad / jumper to 3.3 V (H9) |
| 24 V supply sense | PC0 (ADC1_INP10) | divider 100 k / 10 k; early power-fail warning, supply shown on the System screen |
| Status LEDs | PD12 (run), PD13 (fault) | `DO_01`, `DO_02` (labelled outputs, active high) |
| Hardware revision straps | PE7, PE8 | read once at boot |
| Board role jumper (J7) | PE5, PE6 (inputs, 10 kΩ pull-ups) | read once at boot; section 12 |

| I/O chip selects (SPI4) | PE2 bank A, PE3 bank B, PE4 inputs, PE15 temperatures, PE0 analog (GPIO out, initial high, 10 kΩ pull-ups) | `CS_A`, `CS_B`, `CS_IN`, `CS_T`, `CS_AI` |
| I/O status inputs | PD1 bank A ERR, PC13 inputs ERR, PD14 bank B FAULT, PD15 bank B PGOOD (pull-ups) | all active low except PGOOD |
| Output gate | PB8 `GATE_EN` (out, 100 kΩ pull-down), PB9 `WDI` (out, kick every < 30 ms), PD8 `GATE_FB` (in) | section 14 |
| Pump | PA8 TIM1_CH1 PWM (AF1), PA0 TIM2_CH1 tacho capture (AF1), PC6 DIR, PC7 EN | PWM and EN pass through the gate |
| Relay coil outputs | PB4, PB5 (open-drain out, low = on) | `REL1_N`, `REL2_N`; the gate also drops them |
| USB (service) | PA11 / PA12 (USB FS), PC8 VBUS sense | ST USB bootloader, address, serial number |

Free for later: PA3, PA4, PB11, PC9, PD3-PD7, PD9 (PD4-PD6 kept for a USART2 RS-485 option).

## 5. Power

| Rail | Source | Loads | Budget |
|---|---|---|---|
| 24 VDC in (18-32 V) | Pluggable terminal (+, 0 V, FE) | | about 0.3 A at 24 V |
| Input protection | Fuse 2 A time-lag (F1), series Schottky SS16 for reverse polarity (D1, 0.15 W at 0.3 A), bidirectional 3 kW TVS SMDJ33CA after it (D2), Y-capacitors to chassis (C16, C17), common-mode choke (L3), 47 µF bulk. Sized for IEC 61326-1 industrial, see `CORE_PCB_EMC.md` | | |
| 5 V | Buck, 60 V rated input: TI TPS54360B (U1, 3.5 A, 600 kHz, 22 µH, catch diode B260A) | Display backlight BLVDD (FFC pins 17-18): 400 mA at full brightness; the 3.3 V buck about 450 mA | about 0.9 A |
| 3.3 V | Buck from 5 V: TI TLV62569 (U2, 2 A) | MCU about 120 mA, display logic 240 mA max (235 typ; datasheet), W5500 130 mA, LAN8742A 50 mA, PSRAM about 40 mA, NOR, FRAM | about 0.65 A |
| VBAT | CR2032 in a holder (H1) | RTC, backup registers | µA |
| VDDA / VREF+ | 3.3 V through a ferrite and 1 µF + 100 nF | ADC (supply sense) | |

The 60 V input rating is there because the TVS clamps at about 53 V during a surge; a 30 V
buck would not survive one. Display figures are from the RVT70HSBNWC00 V1.1A datasheet Rev 1.1 (27 Jul 2026):
VDD 3.0-3.6 V, 235 mA typ / 240 mA max (audio on at full volume); BLVDD 4.5-5.5 V (5.0 typ;
it still lights from 2.7 V but not at full brightness), 10 / 310 / 400 mA at minimum / 50 % /
100 % backlight. A 5 % 5 V rail is within range; keep the FFC's 5 V drop under 0.25 V. Brown-out: the MCU's BOR at the highest level
plus the supply-sense pin (H10). The core board drives no field outputs (H6 and H7 are on the I/O boards).

**5 V buck design** (TPS54360B datasheet, equations 44-51; `hardware/datasheets/TPS54360B.pdf`): RT 162 kΩ for
600 kHz; L1 22 µH (about 30 % ripple at 1 A from 24 V); output 22 µF ceramic, about 15 µF
effective at 5 V, plus the 3.3 V buck's input capacitance, about 30 µF in all; crossover about
18 kHz. Type II compensation R6 4.99 kΩ, C5 33 nF, C6 100 pF. **(check L1's Isat, 1.6 A or more,
on the Bourns SRN6045TA-220M datasheet)**.

## 5a. Hold-up after the 24 V input fails

The firmware needs about 20 ms from the moment the supply crosses 16 V: up to 16 ms to see it
through its 16-sample average, plus finishing the current FRAM or NOR write (firmware thread,
26 Oct). The FRAM store and the log are already safe against a power cut at any byte
(`PLATFORM.md` 4, 5a); the hold-up only stops the last record being lost.

Energy available on the +24 V node between 16 V (detection) and 6 V (where the 5 V buck drops
out; the 3.3 V buck then runs on from the 5 V capacitors a little longer):
`E = ½ × C × (16² − 6²) = 110 × C` joules.

| Load at the moment of failure | Input power | C1 only (49 µF) | With C18 + C19 (1 mF) | Same, cold (-20 % C) |
|---|---|---|---|---|
| Full: backlight 100 %, both networks | about 5.3 W | 1 ms | 20 ms | 16 ms |
| Backlight off on power-fail | about 2.6 W | 2 ms | 42 ms | 34 ms |

So the board carries **two 470 µF 63 V electrolytics (C18, C19)** on the protected +24 V node
(63 V because the TVS clamps near 53 V during a surge), and the fuse goes to **2 A time-lag**
for their charging current at switch-on. **Firmware side (26 Oct):** "failing" trips on two raw readings in a row below 16 V
(2 ms, not the 16 ms average; the average still drives the display and the low/high alarms),
and the UI task turns the backlight off (REG_PWM_DUTY 0) as soon as `plat_power_failing()`
is true. That leaves about 2× margin, even cold.

Everything before 16 V is extra: from 24 V down to 16 V the same capacitors give another
28 ms at full load (`½ × C × (23.6² − 16²)` = 0.15 J), which the firmware doesn't count on.

## 6. Connectors

| Ref | Connector | Notes |
|---|---|---|
| J1 | Power in, 3-way pluggable terminal 5.08 mm | +24 V, 0 V, functional earth to chassis |
| J2 | Client RJ45, shielded, with LEDs | LAN8742A, see section 2 |
| J3 | I/O RJ45, shielded, with LEDs | W5500, labelled I/O ONLY |
| J4 | Display FFC, RiBUS, ZIF 20-pin, 0.5 mm pitch | See section 6a |
| J5 | SWD, 10-pin 1.27 mm Cortex debug | SWDIO, SWCLK, SWO, NRST, 3.3 V, GND |
| J6 | Service UART, 6-pin 2.54 mm (FTDI-cable order) | 3.3 V levels |
| BT1 | CR2032 holder | |
| SW1 | Reset button | |
| J7 | Role jumper, 3-pin 2.54 mm | section 12 |
| J10 | Bank A, 26-way latched IDC header | to the breakout board (`hardware/io_breakout`), straight-through ribbon |
| J11 | Bank B, 10-way 3.81 mm pluggable | OUT1-8, 0 V, 0 V |
| J12 | Digital inputs, 26-way latched IDC header | to a second breakout (IN build) |
| J13, J14 | Field supply A and B, 2-way 5.08 mm pluggable | own fuses, never through J1 |
| J15 | Pump power, 2-way 5.08 mm | switched +24 V, 0 V |
| J16 | Pump signals, 4-way 3.81 mm | PWM, DIR, tacho, 0 V |
| J17 | Relay coils, 4-way 3.81 mm | REL1, 0 V, REL2, 0 V (DIN-socket relays) |
| J18 | USB-C | service |
| J19-J21 | Temperatures, 5-way 3.81 mm each | 2 channels + RTD return |
| J22-J24 | 4-20 mA, 6-way 3.81 mm each | 2 × (24 V loop, IN, 0 V) |
| J25 | Sensors, 6-way 3.81 mm | level 5 V / SIG / 0 V, mass flow +10 V / OUT / 0 V |

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
  point-to-point with a series resistor at the PHY (R31 22 Ω), and 33 Ω source series
  resistors on the other lines: TX_EN/TXD0/TXD1 at the MCU (R41-R43), CRS_DV/RXD0/RXD1 at
  the PHY (R44-R46). Keep RMII away from the SPI buses.
- **Any switch, 100 m of cable (Gary, 30 Oct).** Both ports are standard 100BASE-TX, which is
  specified to 100 m of Cat5e. What makes that hold:
  - The client PHY has its own 25 MHz crystal (Y3) and supplies the 50 MHz REF_CLK, the same
    scheme as the Nucleo-H563ZI that works on every switch tried.
  - The straps leave the LAN8742A in MODE 111: all modes advertised, auto-negotiation on,
    HP auto-MDIX on. The firmware must never force speed or duplex. A forced port meets an
    auto-negotiating switch at half duplex (parallel detection) and silently loses
    transmitted frames.
  - The PHY reset (PD11) is under firmware control, so the firmware can renegotiate and
    re-sync the MAC to the result.
  - The line side follows the Microchip reference: 49.9 Ω 1 % to the centre tap, MagJack
    1500 Vrms.
  - Each pair is routed as a 100 Ω differential pair, matched within 1 mm, with no vias,
    under 25 mm from the PHY to the MagJack, over unbroken ground.
  - Placement: R35-R38 (49.9 Ω) within 5 mm of the PHY's TX/RX pins; R30 (RBIAS 12.1 kΩ)
    at its pin with no other trace nearby; Y3 and C46/C47 next to XTAL1/2 over solid ground;
    FB2/C40-C42 (VDDA) and FB3/C48/C49 (centre tap) at their pins.
  - Why it matters: the ST DK, whose PHY shares a 25 MHz oscillator, loses 14-45 % of its
    100 Mbit TX frames through some switches (31 Oct bench, firmware thread), and none at
    10 Mbit. **Prototype acceptance test (flagged by Gary, 31 Oct):** 1000 × 1472-byte pings
    plus page loads, 0 % loss at 100 Mbit, through three different switches (including a cheap
    unmanaged one, and Gary's bench switch that fails with the DK) and over a 100 m Cat5e
    drum. The link must stay at 100 Mbit: in `/api/info` eth, `fallback` stays false and
    `phy_symbol_errors` does not climb (the firmware's automatic 10 Mbit fallback, PR #14, is the
    field safety net only). Small pings hide the fault: the DK lost 10-12 % of 1400-byte
    frames and 0 % of 64-byte ones. If it fails, check the PHY clock (Y3 on MCO or a probe), the TX pair layout and
    the MagJack.
  - The W5500 port has the same line rules. It has no auto-MDIX, but its peer (the I/O
    node's LAN8742A) does.
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
- PSRAM (U10): place it beside the MCU's PB0-PB2/PB10/PC2/PA6 corner. Keep the six OCTOSPI
  traces under 30 mm and roughly equal in length, over unbroken ground; it runs at up to
  about 100 MHz.
- Decoupling per ST's hardware getting-started application note for the STM32H5 **(check)**: 100 nF per VDD pin,
  4.7 µF bulk, and the VCAP capacitors at the values the datasheet gives.

## 8. Decisions and open questions

1. **Supply: 24 VDC (Gary, 25 Oct)**, 18-32 V range, the same as the I/O boards' field supply.
2. **Mounting: DIN rail, behind the display (Gary, 25 Oct)**; see section 7.
3. **No system-OK relay (Gary, 25 Oct):** the PLC's Modbus communication watchdog is
   enough to detect a core board that has stopped.
4. **No RS-485 on the core board (Gary, 25 Oct):** the second RJ45 replaces it. The
   firmware's RS-485 I/O link setting has no port on this board (`bsp_rs485()` returns NULL).
5. **Ethernet: RJ45s with integrated magnetics (Gary, 25 Oct)**, shielded, with LEDs,
   1500 Vrms, -40 to 85 °C.
6. **Enclosure (chosen 25 Oct, Gary can override):** see section 10.
7. **8 MB PSRAM fitted (Gary, 29 Oct):** an AP Memory APS6404L on OCTOSPI1 gives the
   firmware working RAM headroom for instrument models (RAM thread). It costs about AUD 3-5
   and uses six pins that were free. **(check: the -ZR industrial grade, -40 to 105 °C, is
   orderable in AU; the -SN part is rated -40 to 85 °C, which is enough)**.
8. **One PCB for CORE and I/O node (Gary, 30 Oct):** the I/O node section is on this
   board (section 14), selected by the role jumper J7 (section 12).
9. **I/O section choices made by this thread (31 Oct, under Gary's "choose the
   recommended option" rule; any can be reversed):** field supply A feeds bank A, the
   inputs and the pump, supply B feeds bank B and the relay coils; reverse polarity is
   handled by fuse + unidirectional TVS (no series FET, so no 10 A heat); relays are PhotoMOS
   (CPC1017N) coil drivers for DIN-socket relays; the pump switch is an IPS7091S; the 10 V
   mass-flow supply is a REF5010 driving an LT3045's SET pin; analog loop power comes from an
   XP ITX2424SA (6 W) off the logic 24 V.

## 9. BOM summary (main parts)

| Part | Function | Qty |
|---|---|---|
| STM32H573VIT6 (or H563VIT6) | MCU | 1 |
| LAN8742A-CZ-TR (-40 to 85 °C) | Client Ethernet PHY | 1 |
| W5500 (-40 to 85 °C) | I/O Ethernet controller | 1 |
| Würth 7499010121A (RJ45 with magnetics and LEDs) | J2, J3 | 2 |
| MB85RC1MT | 128 KB FRAM, settings | 1 |
| APS6404L-3SQR-SN (or -ZR, industrial temperature) | 8 MB QSPI PSRAM, working RAM | 1 |
| W25Q128JV | 16 MB NOR, event and data log | 1 |
| 25 MHz crystals | MCU HSE, PHY, W5500 | 3 |
| 32.768 kHz crystal | RTC | 1 |
| TPS54360BDDAR (60 V in, 3.5 A) | 24 → 5 V buck | 1 |
| TLV62569DBV | 5 → 3.3 V buck | 1 |
| SS16, SMDJ33CA, 2 A T fuse, Würth 744227 choke (1 A, 80 V) | Input protection | 1 each |
| Bourns SRN6045TA-220M (22 µH) | 5 V buck inductor | 1 |
| 470 µF 63 V radial 12.5 mm, 105 °C | Hold-up (C18, C19) | 2 |
| TPD4E05U06 (or equal) | Ethernet ESD | 2 |
| Hirose FH12-20S-0.5SH(55) | Display FFC | 1 |
| CR2032 holder | VBAT | 1 |
| **I/O node section (rev 0.2)** | | |
| Infineon ISO2H823V25XUMA1 | Bank A, 8 × 0.6 A | 1 |
| ST ISO808A-1TR | Bank B, 8 × 1 A | 1 |
| Infineon ISO1I813TXUMA1 | 8 digital inputs | 1 |
| TI ADS124S08IPBSR | 6 temperatures | 1 |
| ADI AD4112BCPZ | 6 × 4-20 mA, level, mass flow | 1 |
| TI ISOW7741DFMR | Isolated SPI + supply (temperatures, analog) | 2 |
| TI ISO7741FDWR | Pump signal isolator | 1 |
| XP Power ITX2424SA | Isolated 24 V loop supply, 6 W | 1 |
| REF5010AIDR, LT3045EMSE, LM317LDR | Precision 10 V mass-flow supply | 1 each |
| Infineon IPS7091SPBF | Pump high-side switch | 1 |
| MAX6369KA+T, 74LVC1G08 | Output gate (watchdog) | 1, 3 |
| IXYS CPC1017NTR | Relay coil drivers | 2 |
| USBLC6-2SC6, USB-C 16P | Service USB | 1 each |
| Littelfuse 0453 NANO2 6.3 A, 5 A, 10 A in 157 holders | Field fuses | 3 |
| SMCJ33A | Field TVS | 3 |
| 26-way latched IDC box header | J10, J12 | 2 |
| Phoenix MCV 1,5 3.81 mm and MSTBVA 2,5 5.08 mm headers + plugs | Field terminals | 13 |

## 10. Enclosure

The board goes in a DIN-rail electronics housing on the panel's rail. The two RJ45s, the
24 V terminal and the display's FFC exit all need openings, and the board has no field
wiring other than power. Shortlist:

| Housing | For | Against |
|---|---|---|
| **Phoenix Contact BC 107,6 (6 modules, 107.6 mm wide)**, chosen | Made for electronics boards: PCB guides at two levels, top covers for pluggable terminals and RJ45 openings, IP20 for a panel, widely stocked | Openings beyond the standard covers need machining |
| Camdenboss CNMB/6 (6 modules) | Low cost, quick to get | Plain covers: every RJ45 and the FFC slot must be machined |
| Phoenix Contact ICS 50 | Designed for front RJ45s and bus connectors | Dearer, larger than this board needs |

**Housing for rev 0.2: Phoenix Contact BC 215,6 (12 modules), assumed 31 Oct.** With the I/O
node section on the board, the parts' courtyards add up to about 12,100 mm² and the edge
connectors to about 370 mm of edge. The BC 107,6 (about 104 mm of edge per face) was already
out; the BC 161,6 base PCB (about 157.5 × 85 mm, 13,400 mm², 2 × 157 mm of edge) cannot hold
it either. The BC 215,6 (215.6 mm wide, same 89.7 mm height) gives a base PCB of about
211.5 × 85.1 mm. **(check: these sizes are scaled from the BC 161,6 dimension list in
Phoenix's BC brochure; the BC 215,6 PCB drawing has not been seen. `tools/gen_pcb.py` holds
the outline in one constant, `OUTLINE`, to change once it is.)** If Gary prefers the BC
161,6, the fix is to move the passives to the bottom side and use double-level 3.81 mm
headers.

## 10a. Layout (started 31 Oct)

`hardware/core_pcb/cems_core.kicad_pcb` is generated by `tools/gen_pcb.py` from `design.py`:
4 layers, the outline above, all 423 footprints with their nets, the net classes, and
`cems_core.kicad_dru` (2.5 mm between the logic, field, temperature and analog ground domains;
0.5 mm minimum on the field supply and pump tracks). It is a **block plan, not a finished
placement**: connectors sit on the two housing faces, and each sheet's parts are packed into
its own column, single-sided.

- Top face, left to right: J1 24 V, J2 client RJ45, J3 I/O RJ45, J18 USB-C, J13/J14 field
  supplies, J15 pump power, J16 pump signals, J17 relay coils, J11 bank B.
- Bottom face: J19-J21 temperatures, J25 sensors, J22-J24 4-20 mA.
- J10 and J12 (IDC26) stand on the board face as a second tier; the ribbons leave through
  the top cover.
- Columns, left to right: power, core (MCU, memories, Ethernet), field digital I/O, field
  power and pump, analog and temperatures.

Next, by hand in KiCad: move the temperature island over J19-J21 and the analog island over
J22-J25; place the isolation gaps, one under each isolator (ISO2H823, ISO808A, ISO1I813T,
ISO7741, ISOW7741 × 2, ITX2424SA), with no copper crossing them on any layer; then route.
`U14`'s footprint is a **placeholder** (`cems_core.pretty`, from `tools/gen_footprints.py`)
that must be redrawn from the PG-VQFN-70 package drawing before routing. DRC on the block
plan shows only unrouted nets and silkscreen overlaps, with no courtyard overlaps.

## 11. Schematic

`hardware/core_pcb/` holds the KiCad 7 project: a root sheet and eight sheets (power, MCU,
client Ethernet, I/O Ethernet, memories and display, and from rev 0.2 field digital I/O,
field power / pump / relays / USB, analog and temperatures), 423 parts, and a PDF of all sheets
(`cems_core.pdf`) for review without KiCad. The schematic is generated from
`hardware/core_pcb/tools/design.py`, the single description of the circuit, by
`gen_schematic.py`. Each connected pin carries a net label and each unused pin a no-connect
flag. `check_netlist.py` then checks KiCad's own exported netlist against `design.py`, pin by
pin, along with each part's footprint and value. `bom.csv` is the grouped bill of materials.
Values marked CHECK in the sheet notes must be confirmed before layout. Cleared against the
datasheets in `hardware/datasheets/` (26 Oct): the buck compensation and inductor value, the
LAN8742A crystal capacitors (33 pF for its 20 pF CL), straps and line terminations, the W5500
crystal circuit (18 pF, 1 MΩ, 0 Ω) and reserved pins (only 23 to ground, 38-42 open), the FRAM
pinout and the choke rating and pads. Still open: the W5500 line terminations (WIZnet
reference schematic), L1's Isat, the HSE crystal against the DK, the VCAP values, and the FFC
contact side.

The W5500 has no auto-MDIX. The I/O board's LAN8742A has HP auto-MDIX, so a straight cable
works; a W5500-to-W5500 link would need a crossover cable.

## 12. What the firmware reads from the board (CIP-01.3, CIP-07.3)

**Hardware revision straps, PE7 and PE8.** Read once at boot, as inputs with the internal
pull-ups and pull-downs off; each pin has a 10 kΩ to ground (R23, R24) and a 10 kΩ to 3.3 V
footprint (R25, R26), and a revision fits one or the other.

| PE8 | PE7 | `hw_rev` | Board |
|---|---|---|---|
| 0 | 0 | 0 | Rev A (this design, rev 0.1 schematic) |
| 0 | 1 | 1 | Rev B |
| 1 | 0 | 2 | Rev C |
| 1 | 1 | 3 | Rev D |

`hw_rev = (PE8 << 1) | PE7`. The firmware reports it with the firmware version (`/api/info`,
System screen) and can refuse an image built for another revision. A fifth revision needs a
third strap (PE15 is free and next to them).

**Board role jumper, J7 on PE5 (ROLE_A) and PE6 (ROLE_B).** One board serves as the CORE
or as an I/O node (Gary, 30 Oct), and the role comes from a removable shunt (with a pull
tab) on a 3-pin 2.54 mm header with GND in the centre. Each pin has a 10 kΩ pull-up (R27,
R28) and 100 nF to ground (C36, C37). Read at boot as inputs with the internal pulls off,
beside `hw_rev`. Place J7 where it can only be reached with the housing open; silkscreen
"1-2 CORE / 2-3 I/O NODE / none = OFF".

| Shunt | PE5 | PE6 | Role |
|---|---|---|---|
| 1-2 | 0 | 1 | CORE |
| 2-3 | 1 | 0 | I/O node |
| none | 1 | 1 | invalid: outputs held off, alarm |
| (both shorted) | 0 | 0 | invalid: outputs held off, alarm |

**24 V supply sense, PC0 (ADC1_INP10).** R9 100 kΩ over R10 8.2 kΩ, both 1 %, with C13 100 nF:

- `V24 = V_PC0 × (100 + 8.2) / 8.2 = 13.20 × V_PC0`. With the 12-bit ADC and VREF+ = VDDA:
  `V24 = raw × VDDA / 4095 × 13.20`. Take VDDA from the VREFINT calibration value
  (VDDA = 3.3 V × VREFINT_CAL / VREFINT reading), not the nominal 3.3 V, since the 3.3 V rail
  is only ±2 %.
- Full scale is 43.5 V; 24 V reads about 1.82 V (raw about 2260).
- The sense point is after the reverse-polarity diode D1, so it reads about 0.4 V below the
  terminal voltage at full load. Add 0.4 V for display, or report "supply (after protection)".
- Accuracy: about ±3 % with VREFINT correction (resistor tolerances and the ADC).
- Source impedance 7.6 kΩ, but C13 holds the node, so any sampling time works. The node's
  time constant is 0.76 ms: average 16 samples per reading; it is a supervision value, not a
  fast one.
- Suggested thresholds: below 19 V supply low (warning); below 16 V supply failing (log it
  and stop writes that are not power-safe); above 30 V supply high. The bucks run down to
  about 6 V at the input, so the board keeps working through the whole range.
- A surge can briefly lift PC0 to about 4 V; the 100 kΩ limits the injected current to well
  under the pin's 5 mA. Readings in the 100 µs after such an event are not trusted (the
  average covers it).

## 13. Debug lock and recovery (agreed with the firmware thread, 26 Oct; CIP-04.4, CIP-20.4)

Based on ST's STM32H5 product-state model (RM0481 "product life cycle" and debug
authentication). **(check: confirm the state names and what each regression erases in
RM0481 before the first production unit is closed.)**

| Stage | Product state | Debug | Recovery |
|---|---|---|---|
| Development, bench, DK | Open | Full SWD | SWD (J5), BOOT0 jumper (JP1) for the ST bootloader |
| Production units | **Closed, with debug authentication provisioned** | Off: SWD can't read flash, RAM or keys | Authorised regression over SWD (below) |
| Not used | Locked | Off for good | None: a unit with both banks bad would be scrap |

- **Normal updates never need SWD:** the network update writes the free bank, test-boots it
  and rolls back by itself (`PLATFORM.md` 5b). Recovery is only for a unit whose both
  banks are unusable, which the update design prevents.
- **Recovery route:** a technician with the company's debug-authentication credential
  (certificate on the H573; a password on the H563) opens the housing, connects J5, and runs
  a regression with ST's tools. That erases the user flash and returns the chip to Open; the
  factory image is then programmed and the unit closed again. FRAM and the NOR log are
  separate chips and are kept. The serial number (D25), hw_rev, calibration, every setting
  and the HTTPS key all live in FRAM (firmware thread, 26 Oct), so the unit keeps its
  identity and certificate fingerprint and needs no developer token afterwards.
- **Confirmed by the firmware thread (26 Oct)**, with one bench check: the network update
  flips SWAP_BANK from the application, which the H5 allows when Closed. The first Closed DK
  must show an update and a rollback working. Secure boot will keep this route, with no
  Locked state.
- **Hardware for this:** J5 (SWD with NRST) fitted on every unit, inside the housing, not
  reachable without opening it; JP1 BOOT0 footprint fitted, no jumper in production (the
  bootloader is not a route on a closed unit); BOOT0 pulled down by R20.
- The credential is held like the firmware signing key: not in the repository, two people
  hold it.

## 14. I/O node section (rev 0.2, 31 Oct)

The field I/O that Gary listed on 30 Oct, laid out as in the doc
[One PCB for CORE and I/O board](https://claude.ai/artifact/5oGgPyMKgN8YEAyrkMxLkh).
Sheets `io_dig`, `io_pwr` and `io_ana`. Every field chip is on the shared SPI4 at 4 MHz
with its own chip select. Four ground domains: GND (logic), FIELD_0V (field supplies A and
B, banks, inputs, pump, relays), GND_T (temperatures) and GND_AI (analog), each crossing to
GND only through an isolating part.

| Block | Circuit | Firmware note |
|---|---|---|
| Output gate | MAX6369 watchdog (SET = 010, 30 ms) AND `GATE_EN` in a 74LVC1G08 = `GATE`. GATE drives ISO808A OUT_EN, ISO2H823 ODIS (through an inverter), the pump EN and PWM (AND gates) and the relay LEDs | Kick `WDI` from a timer at least every 20 ms; set `GATE_EN` only when outputs may switch; read back `GATE_FB` |
| Bank A | ISO2H823V2.5, serial mode, 12 nF per output, IADJ 6.81 kΩ, J10 IDC26 to the breakout | as io-node |
| Bank B | ISO808A-1, J11 10-way; flyback footprints per channel as on the breakout (S1M + 0 Ω fitted, Zener optional) | PGOOD = field B present |
| Inputs | ISO1I813T, type 1/3, 12 kΩ RIN and 2 kΩ RLED per channel, wire-break 33 kΩ, J12 IDC26 | as io-node |
| Pump | IPS7091S switch on +24V_FP (5 A fuse); PWM and DIR as 24 V push-pull (2N7002 + BC817/BC807); tacho comparator 1.5 V with hysteresis, 30 V tolerant, JP2 pull-up for open collector; all through an ISO7741 | PWM 100 Hz for the 107Z, 5-95 %; EN switches the supply |
| Relay coils | 2 × CPC1017N PhotoMOS from +24V_REL (0.2 A polyfuse) with flyback diodes | `REL1_N`/`REL2_N` low = on |
| Temperatures | ADS124S08 behind an ISOW7741 (3.3 V island), 5.11 kΩ 0.1 % RTD reference, 100 Ω RTD returns, 10 MΩ open-TC bias, 10 nF per pair | DOUT/DRDY on one pin; poll |
| Analog | AD4112 behind an ISOW7741 (5 V island). IIN0-3 = 4-20 mA ch 1-4; VIN0/1 = ch 5/6 on 250 Ω 0.1 % shunts; VIN2 level; VIN3 mass flow; VIN4 = 10 V monitor (÷2); VIN5 = level 5 V. Loop power from an ITX2424SA through 50 mA polyfuses | Correct the mass flow ratiometrically with VIN4 |
| USB | USB-C, USBLC6, 5.1 kΩ CC pull-downs, VBUS sensed only (board is self-powered) | ST bootloader with BOOT0 |

The ISOW7741s drive SPI4_MISO only while their chip select is low (EN_IO1 = NOT CS through
a 74LVC1G04), so they share the bus with the W5500 and the other chips.

**Still to check (CHECK in the sheet notes):** every pin table of the six project symbols
(read through a summarising web fetch, not yet compared with the PDFs): ISO2H823V2.5,
ISO1I813T, ISO808A-1, ADS124S08, AD4112, ISO7741; the ISO2H823 ODIS polarity, OLADJ and
mode pins; the ISO1I813T TS and WB values; the MAX6369 SET table; the ISOW7741 VSEL levels;
the LT3045 PGFB tie; the LMV331 SOT-23-5 pin order; the IPS7091S rating once the pump current
is known; the PG-VQFN-70 footprint (to be drawn); and whether 55 field terminals fit the
BC 107,6 (section 10).

