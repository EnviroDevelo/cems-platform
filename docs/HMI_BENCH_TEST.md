# Touchscreen bench test (STM32H573I-DK + Riverdi RVT70HSBNWC00-B)

This is the first run of the touchscreen on real hardware. Until now the driver and screens have
only been tested against the simulated BT817 (`make test`, screenshots in `docs/ui/`). Work
through the steps in order, because each step needs the one before it.

For each step, write down **pass / fail** and the numbers asked for. When a step fails, its
"If not" line says what to look at first. Stop at the first failure you can't clear, and send
the record back. The firmware side can then be fixed before the next session.

**Bring:**
- the STM32H573I-DK and the RVT70 module;
- the Riverdi Break Out Board 20 with its 15 cm FFC;
- a 5 V supply able to give 0.5 A for the backlight (BLVDD);
- jumper wires;
- a multimeter;
- the debugger (STM32CubeIDE);
- a browser on the DK's network, for the steps that compare with the web page.

**Build:** the firmware as set up in `docs/CUBEMX_SETUP.md`, sections 1 to 5, with section 3e for the
display. Write down the SPI peripheral and the pins you used, because the DK's are not the core
PCB's. (The core PCB uses SPI3: PC10 SCK, PC11 MISO, PC12 MOSI, CS PA15, PD_N PD2, INT PD0.)

## 1. Wiring and supplies (power off, then on with the FFC unplugged)

| # | Check | Pass | If not |
|---|---|---|---|
| 1.1 | Breakout pins 1 (VDD) and 2 (GND) to the DK's 3.3 V and GND. Pins 17-18 (BLVDD) to the 5 V supply, and pins 19-20 (BLGND) to GND, with the grounds joined | Pin numbers checked against `HARDWARE.md` H4 | Rewire. The module's pin 1 is marked on the breakout |
| 1.2 | SCLK (3), MISO (4), MOSI (5), CS (6) and PD_N (8) to the pins configured in CubeMX. INT (7) is optional | Matches CubeMX | Rewire, or change the CubeMX labels `EVE_CS` and `EVE_PD` |
| 1.3 | Power on with the FFC out: 3.3 V and 5 V present at the breakout | 3.2-3.4 V and 4.75-5.25 V | Check the supply wiring |
| 1.4 | SPI clock prescalers in `main.h` for your SPI kernel clock | Slow clock 11 MHz or less, fast 30 MHz or less (write both down) | Change `EVE_SPI_SLOW` / `EVE_SPI_FAST` |

## 2. The controller answers (FFC in, power on, debugger attached)

Watch `plat_ui_bringup` in the debugger (`platform/os/plat_ui_task.c`):

| Field | Meaning |
|---|---|
| `last_init` | 0 = started; -1 = no chip ID; -2 = engines not ready or the first frame failed; 1 = not tried yet |
| `inits` | Start-up attempts, one every 5 s while it fails |
| `lost` | How many times a working display stopped answering |
| `reg_id` | REG_ID as last read. 0x7C is the BT817 |

| # | Check | Pass | If not |
|---|---|---|---|
| 2.1 | `last_init` after start | 0, with `inits` = 1 and `reg_id` = 0x7C | `reg_id` 0x00 or 0xFF: SPI wiring, mode 0, CS and PD_N levels (a scope on SCLK and CS helps). -2: the 12 MHz crystal and CLKEXT (`ext_clock` in `eve_panel_rvt70h`) |
| 2.2 | The backlight comes on and the Home screen appears within 2 s | Picture shown | Backlight dark but `last_init` = 0: check BLVDD, and that REG_PWM_DUTY is not 0 (see 3.1) |
| 2.3 | Current draw at 80 % brightness, then at 100 % | 3.3 V about 235 mA; 5 V up to about 400 mA at 100 % | Write the values down, because they size the core PCB's rails |

## 3. Display test page (System > Service > Display test)

The status page lists what the BT817 reports. Write down every line.

| # | Check | Pass | If not |
|---|---|---|---|
| 3.1 | Controller: "REG_ID 0x7C (answering), chip 0817" | As shown | "WRONG": SPI corruption at the fast clock. Lower `EVE_SPI_FAST` one step and retest |
| 3.2 | Driver: frames rising, faults 0, timeouts 0; Last fault "none" | 0 faults after 1 min on this page | Write down the fault text. Co-processor faults mean a bad command list (send the text) |
| 3.3 | Refresh | 58-62 Hz | Far off: the pixel clock (REG_PCLK_FREQ 0xD12, PCLK 1). Check the timing against the module's datasheet |
| 3.4 | System clock: measured against set | Measured within 1 % of the set value. "72 MHz set" is expected; "60 MHz set" means start-up found the chip on its 60 MHz default and corrected REG_FREQUENCY to match | Write down both numbers. If it is 60 MHz, everything still works; the 72 MHz clock select (host command CLKSEL, 0x61 with 0x46, before ACTIVE) can then be added as a follow-up. Measured more than 2 % off the set value: send the number |
| 3.5 | Size "1024 x 600"; Fonts "4 of 4 own fonts loaded" | As shown | Fonts fewer: inflate/CRC failed in RAM_G. Values still show, in the ROM fonts, but ° ² ³ µ come out as "?" |
| 3.6 | Touch: config value and mode 3 | Write the config value down | Used in 5.1 if touch doesn't work |
| 3.7 | Backlight "80 %, PWM duty 102 of 128"; change brightness on System and come back | Duty follows (10 % → 12, 100 % → 128) | |

## 4. Picture (Colour test: a tap anywhere goes to the next pattern)

| # | Check | Pass | If not |
|---|---|---|---|
| 4.1 | Colour bars: white, yellow, cyan, green, magenta, red, blue, black, left to right | Order and colours right | Colours swapped (red/blue): REG_SWIZZLE. Write down what you see |
| 4.2 | Grey ramp: 32 even steps, black to white, no banding jumps | Even | Uneven: dithering (REG_DITHER) or the panel's gamma. Note only |
| 4.3 | White, black, red, green, blue full screens | No dead or stuck pixels, even backlight | Count and note any dead pixels (for the supplier) |
| 4.4 | Grid, 50 px: lines straight; the outer lines on all four edges visible | All four edge lines visible | Missing edge: HOFFSET/VOFFSET or the sync values are shifted. Write down which edge |
| 4.5 | No flicker or rolling at 100 % and at 10 % brightness | Steady | Flicker at low brightness: REG_PWM_HZ (4 kHz now) |

## 5. Touch (Touch test)

| # | Check | Pass | If not |
|---|---|---|---|
| 5.1 | A finger on the glass gives "Finger at x, y" | Responds | No response: touch config. With the value from 3.6, try `touch_config` for the ILITEK controller in `eve_panel_rvt70h` (the BT817 programming guide's REG_TOUCH_CONFIG section). Send the value |
| 5.2 | Touch each of the 5 crosses. The line under the instructions gives the offset | Each within 10 px (green); write down the 5 offsets | Same offset every time: a touch transform is needed, so send the numbers. Swapped or mirrored axes: send which way |
| 5.3 | Draw slowly along all four edges | Dots follow the finger right to the edge, with no gaps over 20 px | Write down where it drops out |
| 5.4 | Tap Clear, then Done | Back on the status page | |
| 5.5 | Buttons on every screen work on release; sliding off a button cancels it | As described | |

## 6. Screens against the screenshots (`docs/ui/*.png`)

| # | Check | Pass |
|---|---|---|
| 6.1 | Header: product, state pill (STARTING then READY/MEASURING), LOCAL/REMOTE, link pill, alarm pill, time | Same layout as `docs/ui/home.png` |
| 6.2 | Link pill: NO LINK at start; LINK OK while a Modbus master polls; LINK LOST once it stops for `comms_timeout` | All three seen |
| 6.3 | Home tiles, Settings, Events, System, I/O boards, Calibration, Alarms (tap an alarm for "What to check") | Text inside its boxes, nothing cut off, ° ² ³ shown properly |
| 6.4 | The same value on the web page and the touchscreen: same number, same quality word | Match (`UI_CONVENTIONS.md` 7) |
| 6.5 | Maintenance on Home (log in as operator): the state pill shows MAINTENANCE and values show "maintenance"; End maintenance puts it back | As described |
| 6.6 | Log in with the keypad; leave it 10 min and the login lapses; the screen dims after 10 min and the first touch only wakes it | As described |

## 7. Faults and recovery

| # | Check | Pass | If not |
|---|---|---|---|
| 7.1 | Unplug the FFC while running | Instrument carries on (scan, web, Modbus); `lost` +1 within 2 s; display self-test or health shows it | Send what happened |
| 7.2 | Plug it back in | Picture back within about 5 s, without a reset (`inits` +1) | |
| 7.3 | Supply-fail test, if the supply sense is wired (`CUBEMX_SETUP.md` 3i): drop the 24 V input | Backlight off within a few ms, picture back when power returns | |
| 7.4 | Start with the FFC unplugged | Boots normally, UI task idle; plugging in later brings the picture up | |
| 7.5 | Leave it on the Home screen for 1 h | `lost` = 0, Driver faults 0, timeouts 0 | Note the numbers and the time |

## 8. Speed (for the QSPI decision)

| # | Check | Record |
|---|---|---|
| 8.1 | Time `ui_draw` on the busiest screen (Settings, Events), using the DWT cycle counter or a GPIO toggle on a scope | ms per frame at the fast SPI clock |
| 8.2 | Touch to visible response on a button | Feels immediate (under 100 ms) |

Below 20 ms per frame, single SPI is enough (decision D24). Above that, QSPI comes back on the
list.

## What to send back

The pass/fail column and these numbers:
- the SPI pins and clocks (1.2, 1.4);
- currents (2.3);
- every line of the display test page (3.1-3.7);
- touch offsets (5.2);
- `plat_ui_bringup` after 7.5;
- the frame time (8.1).

Photos of any picture fault help.
