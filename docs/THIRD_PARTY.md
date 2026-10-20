# Third-party software in the firmware

Everything here may be used in a closed-source commercial product. Keep this list
current when a component is added; ship the notices with the product documentation.

| Component | Where | Licence | What it asks of us |
|---|---|---|---|
| FreeRTOS kernel V11.2.0 | fetched by the build (`../ext/FreeRTOS-Kernel`) / CubeMX | MIT | keep the copyright notice |
| lwIP 2.2.1 | `../ext/lwip` / CubeMX | BSD 3-clause | keep the notice in the documentation |
| Mbed TLS 3.6.7 LTS (subset) | `third_party/mbedtls` | Apache-2.0 (chosen of Apache-2.0 / GPL-2.0+) | keep `third_party/mbedtls/LICENSE`; note changes (we only removed unused files) |
| STM32H5 HAL, CMSIS device files | CubeMX | BSD 3-clause | keep the notices |
| ST Ethernet driver glue (`port/stm32h5/lwip/ethernetif.c`), LAN8742 driver | from ST's examples | ST SLA0044 | use on ST microcontrollers only (we do) |
| DejaVu Sans glyphs (in `platform/ui/eve_fonts.c`, made by `tools/mkfont.py`) | rendered into the firmware | Bitstream Vera / DejaVu licence (free to embed and redistribute) | keep the notice; do not sell the font on its own |

Test-only (not in the firmware): pymodbus (BSD), Playwright and Chromium (Apache-2.0 /
BSD), Pillow (HPND), openssl command line (Apache-2.0).
