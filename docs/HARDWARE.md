# Hardware: schematic list and bring-up checks

What the firmware needs from the product board, collected as the modules were built.
Keep it current; tick items off at schematic review.

## Schematic list

| # | Item | Why the firmware needs it | Status |
|---|---|---|---|
| H1 | **RTC backup supply on VBAT** (coin cell, e.g. CR2032 with a holder, or a supercap sized for the longest expected outage) and a **32.768 kHz LSE crystal** with load capacitors per ST AN2867 | The clock (D22) must survive power cuts: CEMS records, alarms and the event log are time-stamped. Without VBAT every power-up starts "clock not set" until someone, the PLC or the time server sets it | **Added (Gary, 20 Oct)** |
| H2 | FRAM for settings and calibration, I2C (or SPI), on its own bus or with pull-ups checked | Parameter store (D6). Size to be chosen once the translated application's data is known; the firmware supports 32 KB and 128 KB parts by one setting. Prototype: the existing 32 KB MB85RC256V breakout | **Open: size decided later (Gary, 20 Oct)** |
| H3 | 16 MB (or larger) JEDEC SPI NOR flash on its own chip select | Event log and data logger (D12, D13) | |
| H4 | Riverdi RVT70HSBNWC00-B on the 20-pin RiBUS FFC: SPI (SCK, MOSI, MISO, CS) + PD_N + INT; 3.3 V supply sized for the display (235 mA plus backlight peak) | Touchscreen (D16, D24). Single-lane SPI is enough (QSPI not needed, D24) | |
| H5 | Ethernet: RMII PHY (LAN8742A as on the DK), magnetics, RJ45 with LEDs | Modbus TCP, HTTPS, email, SNTP | |
| H6 | Digital outputs for the alarm relays (up to 4, any DO channel) with a defined de-energised state at reset | Alarm actions (D20); fail-safe relays rely on outputs being off during reset and faults | |
| H7 | Outputs off at reset by hardware (pull-downs on driver inputs) | `bsp_io_safe_state()` covers the firmware side; the hardware must not glitch outputs on during power-up or a watchdog reset | |
| H8 | STM32H563/H573 with 2 MB flash (dual-bank) and Ethernet MAC; the H573 if hardware crypto/secure boot is wanted | Firmware update by bank swap (D14, D15): an image may use at most half the flash. Memory record: `docs/MEMORY.md` | |
| H9 | SWD connector (and BOOT0 access) on the product board | First programming, recovery if an image can't start (see 5b limits) | |
| H10 | Brown-out/supply supervision as ST recommends for the chosen part | Brown-out resets are reported as UNEXPECTED_RESET (alarm 6) | |

## To check on the DK before relying on it

| Check | Where |
|---|---|
| The DK has a 32.768 kHz crystal for the LSE | `CUBEMX_SETUP.md` RCC |
| Which I2C the DK brings out for the FRAM breakout, and its pull-ups | `CUBEMX_SETUP.md` 3a |
| SPI prescalers for the display (11 MHz at start-up, 30 MHz after) and the NOR | `CUBEMX_SETUP.md` 3c, 3e |
| EVE touch configuration for the RVT70 (uxTouch) | `CUBEMX_SETUP.md` 3e |
| The own fonts line up (glyph pointer convention) | `PLATFORM.md` 5g |
| Bank swap: free bank's physical number under SWAP_BANK, first update with the debugger attached | `PLATFORM.md` 5b |
| Duplicate-address check and DHCP on a real network | `PLATFORM.md` 5d |
| 72-hour soak with `health` on `/api/info` watched | `PLATFORM.md` 5h |
