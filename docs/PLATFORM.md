# CEMS Core Platform: firmware structure, scan contract and integration rules

Rev C, 19 Oct 2026 (Rev B: parameter table, arbitration, FRAM store. Rev C: blocks in the table, Modbus TCP, web interface, lwIP networking). Implements the "Firmware architecture and toolchain" section of
*CEMS Supervisor Design Specification Rev 1.00* (file name: *CEMS Instrument Core
Platform Design Specification Rev 1.00.pdf*). This is the code that every instrument
in the family shares. The VM/MUX program translated in `Existing Design/Analysis/stage4`
runs on top of it.

## 1. What is in this revision

| Module | Files | Status |
|---|---|---|
| UniLogic system tags (`g_dt_ms`, `Ladder_Initial_Cycle`, `RTC_Date`, `RTC_Time`, `ul_timer_t`) | `platform/include/ul_system.h` | Done |
| Scan engine (PLC-style scan cycle) | `platform/include/scan_engine.h`, `platform/core/scan_engine.c` | Done, host-tested |
| Command queue (external writes into the scan) | `plat_cmdq.h/.c` | Done, host-tested |
| Snapshot seqlock (scan → UI/web/Modbus readers) | `plat_seqlock.h` | Done, host-tested |
| Task monitor + watchdog supervisor | `task_monitor.h/.c`, in `plat_tasks_freertos.c` | Done, host + RTOS tested |
| Fault record that survives a reset | `plat_diag.h/.c`, `port/stm32h5/noinit_section.ld` | Done, host-tested, placement checked in the ELF |
| FreeRTOS task set, priorities, hooks | `platform/os/plat_tasks_freertos.c`, `port/stm32h5/FreeRTOSConfig.h` | Done, run under the FreeRTOS POSIX port |
| Board support: interface, PC simulation, STM32H5 | `bsp.h`, `platform/bsp/bsp_host.c`, `port/stm32h5/bsp_stm32h5.c` | H5 version compiled and linked, not yet run on the DK |
| Shared parameter table, local/remote arbitration, remote-link watchdog | `param.h/.c`, `plat_sys.c` | Done, host-tested |
| Parameter store: FRAM images, dual copies, factory/user split, fail safe, migration, backup/restore | `param_store.h/.c`, `nvm.h`, `nvm_ram.c`, `plat_crc32.c`, `platform/os/plat_store_task.c` | Done, host + RTOS tested |
| FRAM drivers: I2C (DMA, interrupt or polling) and SPI | `port/stm32h5/nvm_fram_i2c.c`, `nvm_fram_spi.c` | Compiled and linked in every configuration, not yet run on hardware |
| Text, UTF-16 text, byte buffers and arrays as table entries | `param.h` (`PARAM_TEXT`, `PARAM_TEXT16`, `PARAM_BYTES`, `PARAM_ARRAY_*`) | Done, host-tested |
| Modbus TCP server (port 502) | `modbus.h/.c` (engine and map), `mbtcp.h`, `platform/net/mbtcp.c` | Done; host-tested, and tested on real sockets with pymodbus |
| Web interface (port 80): JSON API, logins, sessions, backup/restore, the page | `http_api.h`, `http_server.h`, `web_auth.h`, `platform/net/*`, `web/index.html` | Done; host-tested, on real sockets, and in headless Chromium |
| Network on the H5: lwIP 2.2.1 + FreeRTOS, Ethernet driver (LAN8742 PHY), bring-up, live address changes | `port/stm32h5/lwip/*`, `platform/os/plat_net_tasks.c` | Compiled and linked with the real lwIP, HAL and PHY driver; not yet run on hardware |
| Demo application (shows the contract) | `app_demo/` | Done |

Not in this revision (next modules, in the order proposed): logger; firmware
update/bootloader; EVE driver and touch UI.

## 2. Tasks and priorities

Priorities follow the design spec: control first, then the parameter store and logger,
then networking, and the UI last. Nothing below the scan can delay it.

| Task | FreeRTOS prio | Runs | Watchdog silence limit | Body |
|---|---|---|---|---|
| ctrl | 14 | every 1 ms (`PLAT_CTRL_PERIOD_MS`) | 50 ms | `plat_ctrl_tick()`, fast control loops (weak, empty for now) |
| scan | 13 | every 10 ms (`PLAT_SCAN_PERIOD_MS`) | 200 ms | `scan_step()`: the VM/MUX program |
| supervisor | 12 | every 100 ms | n/a | kicks the IWDG only if every task is alive |
| FreeRTOS timer service | 11 | on demand | n/a | kernel |
| store | 10 | event-driven | 2 s | `plat_store_task_body()` (next module) |
| lwIP tcpip_thread | 8 | event-driven | via modbus/http | lwIP |
| modbus | 7 | socket events | 5 s | `plat_modbus_task_body()` |
| http | 6 | socket events | 5 s | `plat_http_task_body()` |
| net | 4 | every 250 ms | 5 s | `plat_net_task_body()`: lwIP start, link, address changes |
| ui | 3 | touch/redraw | 5 s | `plat_ui_task_body()` |
| eth_rx | 9 | Ethernet receive | n/a | created by the Ethernet driver; ST runs it highest, here it is below control |

Every service body is a weak default that only checks in. A module replaces it by
defining a function with the same name, so adding Modbus later doesn't touch this file.

**Watchdog policy.** Each task calls `plat_checkin()` from its loop. If any task stays
silent past its limit, the supervisor records the task's name in `plat_diag`, sets the
outputs to their safe state, and resets the unit. If the supervisor itself is starved
(a higher-priority task stuck in a loop), nothing kicks the IWDG and the hardware resets
the unit about 1 s later. After either reset, `plat_diag` tells the next boot what happened.

## 3. The scan contract (for the application)

One `scan_step()` is one UniLogic scan (11_UniLogic_Semantics E1, E10, E11):

1. `g_dt_ms` = ms since the previous scan started, **measured**. It is 0 on the first scan. A late scan reports the real gap, so timers never lose time.
2. `RTC_Date` / `RTC_Time` = one clock snapshot for the whole scan. `Day_Of_Week` uses UniLogic's convention (1 = Sunday); the STM32 RTC uses 1 = Monday, and the conversion is tested for all 7 days. If the RTC is invalid, the previous snapshot is kept and the scan counts it.
3. Inputs: `bsp_io_read()` fills a `plat_io_in_t`, then `app->io_in()` maps it onto application tags (`TA30_DI[]`, `UID_DI[]` …). Inputs stay frozen for the whole scan.
4. External writes: up to 16 queued commands (from UI, web or Modbus) go through `app->apply_cmd()`. The logic never sees a value change half way through a scan.
5. Logic: `Ladder_Initial_Cycle` is true for the first scan only, then `app->logic()` runs (that is `Function1`).
6. Outputs: the image starts **all off**, `app->io_out()` sets what it drives, and then `bsp_io_write()` runs. An unmapped output can never be left on.
7. Publish: `app->publish()` copies display values into the application's snapshot, inside the seqlock.
8. Statistics: scan count, late starts, max `g_dt_ms`, execution time (from the DWT cycle counter) and overruns.

The application provides one `scan_app_t` (see `app_demo/app_demo.c`) and calls
`plat_start(&app)` from `main()`. That is the whole interface.

**Timer timing note.** A TON that starts in a scan is credited with that scan's
`g_dt_ms`, so it can end up to one scan period early (the demo shows 1990 ms for a
2000 ms preset). Every scan-based PLC behaves this way. The application's timer model
(assumption A4.10) keeps responsibility for matching UniLogic.

## 4. Parameter table and store

**One table.** Every setting the touchscreen, web page or Modbus can see is a
`param_def_t` bound to the application tag that holds it (`PARAM_INT`, `PARAM_UINT`,
`PARAM_REAL`, `PARAM_BIT` macros). Writes from any of the three are checked in the same
order: known ID, writable, login level, local/remote mode, type, range, then the
application's interlock. So a limit can't be bypassed by using a different route.
`param_check()` gives a server its answer straight away (Modbus exception, HTTP
status), and `param_apply()` re-checks in the scan before the tag changes.

**Arbitration.** The unit always powers up in LOCAL mode, and only the touchscreen can
change the mode. In LOCAL, web and Modbus writes are refused. In REMOTE, the
touchscreen's setpoint writes are refused, but the local operator can always switch
back. Entries flagged `PF_ANY_MODE` (alarm acknowledge, network settings) ignore the
mode. Modbus can be made read-only with `modbus_write_en`, and network and security
settings are never writable over Modbus. In REMOTE mode, if no web or Modbus request
arrives within `comms_timeout` (10 s by default), `remote_link_lost` comes on, and the
application moves to its defined safe state.

**What is stored.** Every `PF_PERSIST` parameter, plus *retained blocks* registered
with `RETAIN()`: the strings, arrays and structs the application keeps across power-off.
The old project retains 342 tags (10.8 KB), mostly strings and buffers
(20_Data_Dictionary.md), so blocks are essential, not optional.

**How it is stored.** There are two regions, FACTORY (calibration, serial, hardware
revision) and USER, each with two copies (A/B) in FRAM. Each copy is a header (magic,
layout version, sequence, CRC-32) followed by records tagged by ID. Saves alternate
between copies, and boot picks the newest copy whose CRCs pass. The store task saves
within about 1 s of a change, and only when something actually changed. That covers
values the logic writes (counters, sequences) as well as operator settings. FRAM
endurance (10^13 to 10^14 writes) makes a once-a-second rate a non-issue over the
product's life.

| Spec rule | How it is met | Test |
|---|---|---|
| Versioned layout; new firmware keeps calibration | Records found by ID; unknown IDs skipped, missing IDs default; `migrate` hook for renumber/rescale; rewritten in the new layout | `t_migration`, `t_unknown_and_resized_records` |
| Dual copies, CRC and sequence, newest valid wins | As above; corrupting either copy loads the other | `t_alternates_copies` |
| Safe on power loss | Power cut at **every byte offset** of a save: the unit always comes up with the complete old or complete new values | `t_power_cut_every_byte` |
| Factory vs user; restore defaults never touches calibration | Separate regions; `restore_defaults` resets the user region only | `t_restore_defaults_cmd` |
| Fail safe: never run silently on defaults | No valid factory image sets `PSTAT_CAL_INVALID` (alarm; the application blocks measurement via `param_store_calibration_valid()`). The damaged image is not overwritten until someone actually changes a factory value | `t_blank_fram`, `t_both_factory_copies_bad` |
| Backup and restore with integrity checks | `param_store_export/import`: CRC over the file and each image; structure checked before anything is applied; factory part only at factory level | `t_export_import` |
| Out-of-range stored value | Ignored, default kept, `PSTAT_REC_SKIPPED` | `t_out_of_range_stored` |

**Memory map.** Chosen by `PLAT_FRAM_SIZE` (plat_config.h):

| | 32 KB (default: MB85RC256V, Adafruit 1895) | 128 KB (`PLAT_FRAM_SIZE 131072u`) |
|---|---|---|
| Factory A / B | 0x0000 / 0x0400, 1 KB each | 0x00000 / 0x01000, 4 KB each |
| User A / B | 0x0800 / 0x4200, 14.5 KB each | 0x02000 / 0x0C000, 40 KB each |
| Reserved (logger write pointer) | 0x7C00-0x7FFF | 0x16000+ |

Sizing against the old project's actual retained set (`data_dictionary.csv`, 342 tags):
a 12,540-byte user image, which leaves 2,308 bytes (16%) of headroom in the 32 KB map.
If the real table doesn't fit, the store reports `PSTAT_TABLE_ERROR` and the platform
stops at start-up with outputs safe; it never truncates. The 2,754 bytes of UTF-16
strings could be stored as UTF-8 if space ever runs short.

**FRAM drivers.** I2C (`nvm_fram_i2c.c`, DMA by default so a ~110 ms save at 1 MHz uses
no CPU and doesn't starve the network tasks; polling during the boot load) and SPI
(`nvm_fram_spi.c`). `bsp_param_nvm()` tries I2C, then SPI, then the RAM stand-in.

**Known limits of this revision.**
- External writes carry one scalar each. Writing a *string* from the touchscreen or
  web (valve names, alarm descriptions) needs a string command path, which comes with
  the HTTP/UI modules. Retained strings are already stored.
- A value the logic changes within the last second before power-off may be lost (the
  demo shows this). Operator settings are saved within 1 s of the write.
- Import and export run in the store task. The HTTP module will add the hand-over.

## 5. Modbus TCP and the web interface

**One source of truth.** Both servers only ever touch the parameter table. A Modbus
write, a web write and a touchscreen write pass the same checks (level, mode, range,
interlock), and both servers read the copy the scan publishes, so neither can catch the
logic half way through a scan. Multi-value writes are checked completely and queued as
one batch, and the scan applies everything queued before it runs the logic. So a
16-register write either lands whole in one scan or not at all.

**Modbus map.** The application lists where each table entry appears (`mb_map_t`):
coils, discrete inputs, holding and input registers; 16- and 32-bit values (either word
order); floats as IEEE or scaled integers (`dec`); text and byte buffers packed two bytes
per register (either byte order); bit arrays on coils. That covers every shape in the
old slave map (`stage4/4G/modbus_map.csv`: 201 entries; INT16, BIT arrays, TIMER
.Preset/.Out, REAL, 10- and 30-register BUFFERs), so the old addresses can be kept for
existing DAHS configurations. Exceptions: 01 writes disabled or level too low; 02
unmapped, read-only, or half a 32-bit value; 03 range or type; 04 wrong local/remote
mode; 06 queue full. Up to 4 clients; when full, the client silent longest makes room
(a PLC that rebooted without closing would otherwise hold a slot). Optional single
allowed client IP. TCP keepalive notices a vanished client in about 50 s.

**Web interface.** One page (4.6 KB gzip) loads once, then polls JSON: values every
0.5 s, status every 2 s. Viewing needs no login. Changes need a login (operator, admin
or factory); a session allows changes for 10 minutes after the login or the last change.
Five wrong passwords lock login for 30 s. Passwords are stored as salted SHA-256 in FRAM,
outside the readable table. Every POST must carry an `X-CEMS: 1` header and the cookie
is `HttpOnly; SameSite=Strict`, so another web page cannot submit changes. If the
instrument stops answering, a red "Connection lost, last update hh:mm:ss" banner appears
and the values grey out until polling recovers. Admin users can download a parameter
backup and restore it. Screenshots: `docs/web_ui.png`, `docs/web_ui_connection_lost.png`.

**Network on the H5.** lwIP 2.2.1 on FreeRTOS with BSD sockets; ST's H5 Ethernet driver,
ported to native FreeRTOS. The address comes from the parameter table (static
192.168.1.50/24 by default, or DHCP) and a change from the touchscreen or web takes effect
immediately. The MAC address is derived from the chip's unique ID: locally
administered, different on every unit, and stable across reboots.

## 6. Integration rules for the application (VM/MUX) code

These come from reading the Stage 4 sources against this platform. **Items 1 and 2
stop the Stage 4 chunks from linking together correctly**, so they need fixing in that
work before integration.

1. **One definition of the system tags.** `g_dt_ms` is defined in `4G_faithful.c`, `4H_faithful.c` and `4I_faithful.c`, and 4H also defines `Ladder_Initial_Cycle` and `RTC_Date`. Linked together, that is a multiple-definition error. On the platform, `scan_engine.c` owns all four. The faithful files should only declare them, via `#include "ul_system.h"`. The test files can keep their own definitions.
2. **`rtc_date_t` layout differs.** `4F2.h` declares `{ Year, Month, Day_In_Month, Day_Of_Week }`, while `4E.h` and `4H.h` declare `{ Day_Of_Week, Day_In_Month, Month, Year }`. Two different layouts of one global in one program is undefined behaviour: dates would be silently wrong. Every chunk should use the typedef in `ul_system.h`. Members are accessed by name, so only the typedef lines change.
3. `ul_timer_t` is repeated in every chunk header. Use the one in `ul_system.h`, which has the same layout.
4. `Touched_Bit` and `VNC_Touched_Bit` (General.* HMI bits, used in 4F4) will be supplied by the UI module. Until then, the application should define them itself.
5. Physical I/O tags (`TA30_DI[]`, `UID_DO[]` …) remain application tags. The platform hands over channel images, and `io_in`/`io_out` do the mapping. Channel numbers follow `10_IO_Map.md`.
6. The application must not call the BSP or FreeRTOS directly. Anything it needs from the platform comes through `plat_app_t`, `ul_system.h` or `param.h`.
7. **Parameter table from the data dictionary.** The 148 SETTING tags become `param_def_t` entries (IDs from 1000), and the other retained tags become `RETAIN()` blocks. `stage2/data_dictionary.csv` has the type, retain flag and power-up value for each, so the table can be generated rather than typed. Ranges must come from the HMI element limits (Stage 5 `hmi_elements.csv`); where a limit isn't known, it is a Rule 0 item, not a guess.
8. Tags the logic rewrites every scan should not be `PF_PERSIST` or retained unless the original retains them; the store only saves real changes, so this costs nothing either way, but it keeps the image meaningful.
9. The application must read `param_store_calibration_valid()` and `param_remote_link_lost()` and act on them (block measurement; go to its safe state).
10. **Modbus map from the old slave map.** Generate `mb_map_t` from `stage4/4G/modbus_map.csv`. Strings and buffers the master reads become `PARAM_TEXT`/`PARAM_BYTES` entries; bit arrays become `PARAM_ARRAY_BOOL`; timer presets become `PARAM_UINT` on `.Preset`. **Open item (Rule 0):** the byte order UniLogic uses to pack BUFFER bytes into registers, and the word order of 32-bit values, aren't in the analysis. The map supports both orders (`MBF_BYTE_LO_FIRST`, `MBF_WORD_LO_FIRST`), so this is a single flag per entry once it is known (a UniLogic screenshot of the Modbus slave data map, or one register read from a running unit, settles it).

## 7. How it was checked

| Check | Result |
|---|---|
| Host unit tests (`make test`): scan order, first-scan bit, dt including 2^32 wrap and late scans, RTC snapshot and all 7 weekday conversions, frozen inputs, outputs default off, command order/limit/reject, seqlock retry and bounded failure, monitor grace/limits/wrap/re-enable, diag record across warm reset and after corruption; parameter definitions, every check, arbitration in both modes, interlocks, the remote-link watchdog; the store tests in section 4 | 2,191 checks, 0 failures, run with both memory maps, gcc -Werror with AddressSanitizer and UBSan |
| PC simulation (`make sim`): demo app on the parameter table and store, blank FRAM, writes from touchscreen/web/Modbus, power cycle | Modbus refused in LOCAL; web refused below min, then accepted in REMOTE; IP change refused over Modbus; setting and retained string survive the power cycle; mode back to LOCAL; calibration alarm stays on an uncalibrated unit |
| FreeRTOS POSIX run of the real task file (`make rtos`) | 280 scans in 2.8 s, ctrl 1 kHz, IWDG kicked every 100 ms; the real store task saves a queued web write; a UI task that hangs at 2 s gives a recorded "ui" fault, outputs to safe state, and a reset at 7.0 s |
| Network integration (`make netsim` + `tests/integration/net_test.py`): real sockets, threads, file-backed FRAM, pymodbus 3.6.9 as the Modbus client, plain HTTP with JSON parsing | 49 checks: reads of every shape, mode refusals, range/half-value/read-only exceptions, text through registers, coils, web sessions and batch writes (all or nothing), client eviction, IP filter, 3 Modbus clients + 1 browser for 4 s with no errors, remote-link watchdog in real time, backup/restore and a damaged file refused, and after a restart: settings and the changed password kept, mode back to LOCAL |
| Web page in headless Chromium (`tests/integration/web_ui_test.py`) | Login, an out-of-range value refused with a plain message, an accepted change shown within a second, the connection-lost banner when the unit stops, no script errors |
| Target compile and link (`make target`, `make -f port/stm32h5/check.mk link`) with arm-none-eabi-gcc 13.3.1, FreeRTOS V11.2.0, STM32H5 HAL and CMSIS from ST's GitHub | Clean with -Werror in five configurations (no FRAM, SPI, I2C+DMA, I2C+interrupt, I2C with the 128 KB map), each linked with lwIP 2.2.1, the Ethernet driver and both servers. With the I2C FRAM: 136.5 KB flash (of 2 MB), 250 KB RAM (of 640 KB). `plat_diag` is in the NOLOAD `.noinit` section |
| On the STM32H573I-DK | **Not done yet.** Needs the CubeIDE project (`docs/CUBEMX_SETUP.md`) |

## 8. Decisions (working defaults, change any of them)

| # | Decision | Proposal | Why it matters |
|---|---|---|---|
| D1 | Scan period | 10 ms, fixed period, measured `g_dt_ms` | The UniLogic scan time is unknown. 10 ms matches the Stage 4 tests and leaves the CPU mostly free |
| D2 | Fast control loop rate | 1 kHz task tick for now; a hardware timer ISR if any loop needs more | The spec allows 1 to 10 kHz; the loop list doesn't exist yet |
| D3 | Network stack | lwIP 2.2.1 from ST's `stm32h5-classic-coremw-apps`, added by hand | CubeMX doesn't offer lwIP for the H5 (ST recommends NetX Duo). The spec says lwIP; NetX Duo is ThreadX-based, and choosing it would mean changing the RTOS |
| D4 | Hard-fault handling | Record PC/LR in `plat_diag`, then reset | Not written yet; small, belongs in the port |
| D5 | Factory network default | Static 192.168.1.50/24, gateway 192.168.1.1, DHCP off | Always settable from the touchscreen, so a unit can be recovered locally |
| D6 | FRAM | Development: your 32 KB I2C MB85RC256V (16% headroom with today's data). Product: decide at schematic; a 128 KB part (e.g. MB85RC1MT, I2C, same driver) is one setting away | 32 KB works but is tight once the instrument family adds data |
| D7 | Modbus writes at first power-up | Enabled; any client IP | Plugs into a DAHS without setup; an admin can make Modbus read-only or limit it to one IP |
| D8 | Touchscreen writes in REMOTE mode | Refused, except the mode switch and `PF_ANY_MODE` entries | Stops the local panel and the PLC fighting over a setpoint; the spec only states the remote side |
| D9 | Default passwords | operator 1111, admin 2222, factory 9999; the page warns while any is unchanged; "restore defaults" also restores these | Change at commissioning. A per-unit factory password (e.g. from the serial) is an option if units leave the factory |
| D10 | MAC address | Locally administered, from the chip unique ID | Fine for site networks. For production, buy an IEEE OUI block or fit an EUI-48 chip (e.g. 24AA02E48) |
| D11 | Web transport | HTTP on port 80 now; HTTPS (mbedTLS) later | The spec allows HTTP on an isolated control network; HTTPS adds about 100 KB flash and 40 KB RAM and can come with the firmware-update work |

## 9. Repository layout

```
platform/include/   public headers (no HAL, no FreeRTOS except plat_port.h on target)
platform/core/      portable logic: scan engine, queue, monitor, diag
platform/os/        FreeRTOS binding (tasks, hooks)
platform/net/       Modbus TCP, HTTP server, web API, logins (socket API: POSIX or lwIP)
platform/bsp/       simulated board for host builds
port/stm32h5/       STM32H5 BSP, FRAM drivers, FreeRTOSConfig.h, linker snippet, compile/link check
port/stm32h5/lwip/  lwipopts.h, Ethernet driver (from ST, SLA0044), network bring-up
web/                the web page source (embedded by tools/embed_web.py)
app_demo/           example application on the scan contract
host/               PC simulation, FreeRTOS POSIX run, networked simulation (netsim)
tests/              host unit tests; tests/integration: network and browser tests
docs/               this file, CUBEMX_SETUP.md
```
