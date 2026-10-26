# Datasheets for the core PCB

Reference copies of the datasheets the core board design was checked against (Gary, 26 Oct
2026). Use them when changing the schematic or doing layout; check the vendor's site for a
newer revision before a production build.

| File | Part | Revision | Used for |
|---|---|---|---|
| `LAN8742A_DS00001989A.txt` | Microchip LAN8742A, client Ethernet PHY (U4) | DS00001989A (text extract of the PDF) | Straps, crystal CL 20 pF, VDDCR, RBIAS, line terminations, LEDs |
| `W5500_ds_v110e.pdf` | WIZnet W5500, I/O Ethernet controller (U6) | v1.1.0 | Pinout, reserved pins, crystal circuit, SPI timing, no auto-MDIX |
| `TPS54360B.pdf` | TI TPS54360B, 24 V to 5 V buck (U1) | TI datasheet | RT, inductor, compensation (eq. 44-51) |
| `MB85RC1MT_DS5v1.pdf` | Fujitsu/RAMXEED MB85RC1MT, 128 KB I2C FRAM (U9) | DS5 v1 | Pinout, I2C address 0x50, WP |
| `Wurth_744227_WE-SL2.pdf` | Würth WE-SL2 744227, 24 V common-mode choke (L3) | Würth datasheet | Rating 1 A / 80 V, winding and pad mapping |
| `RVT70HSBNWC00_V1.1A_Rev1.1.pdf` | Riverdi RVT70HSBNWC00-B, 7" display | V1.1A, Rev 1.1 (27 Jul 2026) | Supplies, RiBUS FFC pinout, temperature range |

Still wanted (not here yet): the WIZnet W5500 reference schematic (line terminations), the
Bourns SRN6045TA datasheet (L1 Isat), the Phoenix Contact BC 107,6 drawing (board outline),
and the STM32H573 datasheet (VCAP values).
