# CEMS Core Platform: firmware structure, scan contract and integration rules

Rev D, 19 Oct 2026 (Rev B: parameter table, arbitration, FRAM store. Rev C: blocks in the table, Modbus TCP, web interface, lwIP networking. Rev D: event log and data logger, firmware update over the network, EVE display driver and touch UI). Implements the "Firmware architecture and toolchain" section of
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
| CPU fault handlers (D4): safe state, PC/LR and fault registers recorded, reset | `port/stm32h5/fault_stm32h5.c`, `plat_diag_cpu_fault()` | Record and naming host-tested; handlers compiled and linked, in the vector table (checked in the ELF). Not yet run on the DK |
| FreeRTOS task set, priorities, hooks | `platform/os/plat_tasks_freertos.c`, `port/stm32h5/FreeRTOSConfig.h` | Done, run under the FreeRTOS POSIX port |
| Board support: interface, PC simulation, STM32H5 | `bsp.h`, `platform/bsp/bsp_host.c`, `port/stm32h5/bsp_stm32h5.c` | H5 version compiled and linked, not yet run on the DK |
| Shared parameter table, local/remote arbitration, remote-link watchdog | `param.h/.c`, `plat_sys.c` | Done, host-tested |
| Parameter store: FRAM images, dual copies, factory/user split, fail safe, migration, backup/restore | `param_store.h/.c`, `nvm.h`, `nvm_ram.c`, `plat_crc32.c`, `platform/os/plat_store_task.c` | Done, host + RTOS tested |
| FRAM drivers: I2C (DMA, interrupt or polling) and SPI | `port/stm32h5/nvm_fram_i2c.c`, `nvm_fram_spi.c` | Compiled and linked in every configuration, not yet run on hardware |
| Text, UTF-16 text, byte buffers and arrays as table entries | `param.h` (`PARAM_TEXT`, `PARAM_TEXT16`, `PARAM_BYTES`, `PARAM_ARRAY_*`) | Done, host-tested |
| Modbus TCP server (port 502) | `modbus.h/.c` (engine and map), `mbtcp.h`, `platform/net/mbtcp.c` | Done; host-tested, and tested on real sockets with pymodbus |
| Web interface (port 80): JSON API, logins, sessions, backup/restore, the page | `http_api.h`, `http_server.h`, `web_auth.h`, `platform/net/*`, `web/index.html` | Done; host-tested, on real sockets, and in headless Chromium |
| Network on the H5: lwIP 2.2.1 + FreeRTOS, Ethernet driver (LAN8742 PHY), bring-up, live address changes | `port/stm32h5/lwip/*`, `platform/os/plat_net_tasks.c` | Compiled and linked with the real lwIP, HAL and PHY driver; not yet run on hardware |
| Event log and data logger on serial NOR, CSV export, event list on the web page | `plat_log.h/.c`, `nor.h`, `nor_sim.c`, `log_csv.h/.c`, `port/stm32h5/nor_spi.c` | Done; host-tested incl. 400 random power cuts, on real sockets and in Chromium. SPI NOR driver compiled and linked, not yet run on hardware |
| Firmware update over the network: streamed upload into the free flash bank, bank swap, trial boot, automatic confirm or rollback; image tool; the date and time of every update kept in the instrument (last 6, FRAM) | `fw_update.h/.c`, `fw_flash_sim.c`, `port/stm32h5/fw_flash_stm32h5.c`, `tools/mkfwimage.py` | Done; host-tested, end to end on real sockets (upload, swap, trial, confirm, crash, rollback) and in Chromium. Flash driver compiled and linked, not yet run on hardware |
| EVE BT817 display driver (Riverdi RVT70 timing, co-processor commands, touch tags, fault recovery) and the touch UI (home, settings, edit, log in, events, system) | `eve.h`, `platform/ui/eve.c`, `ui.h`, `platform/ui/ui.c`, `platform/os/plat_ui_task.c`, `port/stm32h5/eve_bus_stm32h5.c`; `host/eve_sim/`, `tools/eve_preview.py` | Done; host-tested against a simulated BT817 driven by simulated touches; screens rendered from the real command lists (`docs/ui/`). Not yet run on the display |
| Firmware image signing (D14): ECDSA P-256 signature over the image header (which holds the payload's SHA-256); key tool | `platform/net/fw_sig.c`, `platform/core/fw_pubkey.c` (generated), `tools/fwkey.py`, `tools/mkfwimage.py --key` | Done; host-tested (keys made in the test: product key accepted; unsigned, other-key, edited-header and edited-payload images refused) and end to end through netsim with a key made by `fwkey.py`. Compiled and linked for the H5 |
| HTTPS (D11): own key and certificate, TLS 1.2 server, session tickets, port-80 redirect | `tls.h`, `platform/net/tls.c`, `tls_config.h`, `third_party/mbedtls` | Done; host-tested, handshakes against Python's TLS client (resumption, certificate trust by IP, redirect) and Chromium. Compiled and linked for the H5 |
| Serial number with developer authority (D25): no ordinary write at any level or from any route, not restored from backups; changed only with a token signed by a separate developer key, bound to the chip's unique ID and the current serial; every change and refusal logged | `plat_serial.h`, `platform/core/plat_serial.c`, `platform/core/dev_pubkey.c` (generated), `tools/serial_token.py`, `tools/fwkey.py devpubkey`; web Administration → Serial number | Done; host-tested (every source and level refused, wrong key / chip / serial / tampered token refused, replay refused, kept over a power cut, a factory-level backup restore leaves it alone) and end to end with tokens from the tool (`tests/integration/serial_test.py`). Needs the developer key made and `dev_pubkey.c` committed |
| Application screens and record editors (D26): the application describes screens (values, buttons, valve grids, dialogs) and sequence/schedule editors by parameter ID; drawn on the touchscreen and the web page; records written whole after the application's check | `ui_app.h`, `platform/ui/ui_app.c`, `ui.c`, `param.h` (`PARAM_RECORD`), `/api/ui`, `/api/record`, web page | Done; host-tested on the simulated display and the web API (`tests/test_ui_app.c`), end to end in Chromium with the demo's screen and record; the VM/MUX application links and runs with it |
| Health: heap and every task's stack margin sampled every 5 s (web, System screen), alarm 11 RESOURCE_LOW; soak test | `plat_health.h`, `plat_tasks_freertos.c` (sampling), `tests/integration/soak_test.py` | Done; host-tested; soak run for 15 min on the PC build (no failures, sockets and memory flat) |
| Own touchscreen fonts (D24): DejaVu Sans at the ROM font sizes with ° ² ³ µ ± and 26 more symbols, kept compressed in the firmware, inflated into the display's RAM at start-up and CRC-checked; UTF-8 text encoded for them; ROM fonts if loading fails | `tools/mkfont.py`, `platform/ui/eve_fonts.c` (generated), `eve.h` (`eve_fonts_load`, `eve_font_encode`), `ui.c`; simulator: `host/eve_sim/zinflate.c` | Done; host-tested against the simulated BT817 (inflate, CRC read back from the FIFO, a fault while loading, a damaged stream) and glyph sheets checked by eye. Not yet run on the display |
| Clock: set from the touchscreen, the web page, the PLC (parameter 85) or the site's SNTP server; every change logged with its source; local standard time with a UTC offset | `plat_clock.h`, `platform/core/plat_clock.c`, `platform/net/sntp.c`, `bsp_rtc_write`; System > Set clock; Clock on the web page | Done; host-tested and end to end against a test time server |
| Alarm actions and email: per-alarm email / Modbus common alarm / 4 relay outputs; SMTP client (STARTTLS, TLS, plain) with certificate pinning, batching, retries, NOTIFY_FAIL; write-only secrets kept out of backups | `plat_alarm.h` (routes, relays), `mail.h`, `platform/net/mail.c`, `tls.c` (client); Alarm actions on the web page and the touchscreen | Done; host-tested and end to end against a test mail server (plain, STARTTLS with RSA, TLS with ECDSA, pinning, a changed certificate, retries), in Chromium and on the simulated touchscreen |
| Alarm manager: one table of alarm definitions, on/off delays, latch, status alarms, acknowledge one or all from the touchscreen, web and Modbus, suppression, range alarms with deadband, editable descriptions, platform alarms, event-log entries | `plat_alarm.h`, `platform/core/plat_alarm.c`; Alarms screen in `ui.c`; `/api/alarms` | Done; host-tested (every state change, delays, latch, deadband, ordering, paging), on real sockets with Modbus and HTTPS, and in Chromium |
| I/O boards on Ethernet (Modbus TCP) or RS-485 (D27): master for up to 3 CEMS I/O boards (8 × 0.6 A, 8 × 1 A, 2 relays, 8 inputs each), their channels in the platform's image, per-channel fault diagnostics, temperature inputs and a pump PWM output (register map 2), seven platform alarms, events, the I/O boards screen and web section; discovery and IP commissioning from the web page; a simulated node and a simulated network that implement the register map and the protocol | `iobus.h`, `iob_net.h`, `platform/core/iobus.c`, `iob_net.c`, `mb_rtu.h/.c`, `platform/net/iob_tcp.c`, `platform/os/plat_iobus_task.c`, `port/stm32h5/rs485_stm32h5.c`, `platform/bsp/iob_node_sim.c`; `/api/iob`, `/api/iob/net`, `/api/iob/temp/zero` | Done; host-tested on both links against simulated boards (`tests/test_iobus.c`), the socket transport over the loopback (`tests/test_iobtcp.c`), end to end over netsim (`tests/integration/iob_test.py`, browser included) and against the real node firmware on sockets (io-node `make net-e2e`). Compiled for the H5 with lwIP, not yet run on hardware |
| I/O port: W5500 driver, the I/O bus on its hardware sockets, a simulated W5500 (HARDWARE.md H5, Gary 25 Oct) | `w5500.h`, `platform/core/w5500.c`, `iob_w5500.h`, `platform/core/iob_w5500.c`, `port/stm32h5/w5500_stm32h5.c`, `platform/bsp/w5500_sim.c` | Done; the driver against the simulated chip (registers, commands, wrapping buffers, UDP, refused and closed connections), every I/O bus test also run over driver, transport and simulated chip (`tests/test_iobus.c`, `tests/test_w5500.c`), commissioning over the web API, and netsim end to end (`iob_test.py`, browser included) on the I/O port. Compiled and linked for the H5, not yet run on a W5500 |
| 24 V supply monitoring, SUPPLY alarm and power-fail write hold (CIP-07) | `plat_power.h`, `platform/core/plat_power.c`, `port/stm32h5/supply_stm32h5.c` | Done, host-tested; ADC read compiled and linked |
| Instrument state and quality of live values (Common Instrument Platform Specification CIP-09, CIP-10): one state (STARTING ... SAFE_MODE), forced FAULT, maintenance from the touchscreen or web, per-measurement quality with automatic stale detection, on Modbus, `/api/*` and for the touchscreen; the supervisory link age (CIP-15.2) | `plat_state.h`, `platform/core/plat_state.c` | Done, host-tested (5l) |
| Calibration records (CIP-18, CIP-17.6, CIP-19.3): per channel a factory record and the record in force, a history of the last 12 (any outcome), begin/accept/abort/revert with a permission level, a product check hook, due dates and alarm 21, `/api/cal` | `plat_cal.h`, `platform/core/plat_cal.c` | Done, host-tested and end-to-end (5m) |
| Service history (CIP-22.2, 22.4, 23.4, 05.4): operating hours and hours since service, a service log, the manufacturing test record, a configuration checksum, `/api/service` | `plat_service.h`, `platform/core/plat_service.c` | Done, host-tested and end-to-end (5n) |
| Demo application (shows the contract) | `app_demo/` | Done; includes two alarms (a latching fault and a range alarm with an enable and an editable description) |

Not in this revision: everything in the spec for the core platform is now built and
host-tested; what remains is bringing it up on the DK (`docs/CUBEMX_SETUP.md`) and the
72-hour soak on hardware.

## 2. Tasks and priorities

Priorities follow the design spec: control first, then the parameter store and logger,
then networking, and the UI last. Nothing below the scan can delay it.

| Task | FreeRTOS prio | Runs | Watchdog silence limit | Body |
|---|---|---|---|---|
| ctrl | 14 | every 1 ms (`PLAT_CTRL_PERIOD_MS`) | 50 ms | `plat_ctrl_tick()`, fast control loops (weak, empty for now) |
| scan | 13 | every 10 ms (`PLAT_SCAN_PERIOD_MS`) | 200 ms | `scan_step()`: the VM/MUX program |
| supervisor | 12 | every 100 ms | n/a | kicks the IWDG only if every task is alive |
| FreeRTOS timer service | 11 | on demand | n/a | kernel |
| iobus | 11 | every 20 ms (`PLAT_IOB_PERIOD_MS`) | 1 s | `plat_iobus_task_body()`: the I/O boards, over Modbus TCP (default) or RS-485 (`iob_link`, D27); sleeps on the socket or the UART while it waits for replies |
| store | 10 | every 20 ms | 2 s | `plat_store_task_body()`: parameter store and logger (sole owner of the FRAM and the log flash) |
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
3. Inputs: `bsp_io_read()` fills a `plat_io_in_t` from the main board's pins, the I/O boards' latest inputs replace their channel ranges (`iobus.h`), then `app->io_in()` maps it onto application tags (`TA30_DI[]`, `UID_DI[]` …). Inputs stay frozen for the whole scan. There are 64 digital input and 64 output channels; `di_fault` / `do_fault` mark channels that are not to be trusted (a wire break, an open-circuit valve, a board not answering), and `plat_di_fault()` / `plat_do_fault()` read them.
4. External writes: up to 16 queued commands (from UI, web or Modbus) go through `app->apply_cmd()`. The logic never sees a value change half way through a scan.
5. Logic: `Ladder_Initial_Cycle` is true for the first scan only, then `app->logic()` runs (that is `Function1`).
6. Outputs: the image starts **all off**, `app->io_out()` sets what it drives, the alarm relays are added, and then `bsp_io_write()` and the I/O bus take it. An unmapped output can never be left on.
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

**The platform's block (CIP-01.6, 14.4, 15.1, 15.2).** Whatever the application maps, the
platform adds its own read-only block at input registers 9000-9089 (9000-9199 reserved), the
same in every product, so a PLC or DAHS can identify and supervise any CEMS instrument the same
way: block layout version (9000), the application's map version (9001, `mb_ident_t.map_version`),
a heartbeat that counts every 100 ms (9002), the instrument state (9003), seconds since the last
Modbus or web request (9004), alarms active and the common alarm word (9005-9006), hardware
revision, serial number and firmware version (9007-9012), web API version (9013), uptime
(9014-9015), the supply voltage (9016-9017), when the state was entered (9018-9019), the quality
of each measurement (9020-9051), and product, vendor and firmware as text (9052-9083); layout 2 adds the
clock and its quality (9084-9089: the clock, synchronised, source, when last set or confirmed).
Registers are only ever added, never moved, and each addition raises the layout version. The full
layout is in `modbus.h`. An application map entry in that range of the input registers stops the
start-up ("modbus map"). Function 43/14 (device identification) gives vendor, product and firmware
as before. Compatibility (CIP-14.5): addresses in the block never move; a new value is added at
the end and the block version goes up.

**Web interface.** One page (4.6 KB gzip) loads once, then polls JSON: values every
0.5 s, status every 2 s. Viewing needs no login. Changes need a login (operator, admin
or factory); a session allows changes for 10 minutes after the login or the last change.
Five wrong passwords lock login for 30 s. Passwords are stored as PBKDF2-HMAC-SHA256 (4096
rounds, per-unit salt; CIP-20.3) in FRAM, outside the readable table; one set by older firmware
still works and is re-stored that way at its next good login. Every password (operator, admin,
factory) needs 4 characters or more. **Lost passwords
(CIP-19.3):** a one-time token from the developer, signed with the developer key over the chip ID
and the unit's reset count (`tools/reset_token.py`; `GET /api/auth/reset` shows what to sign,
`POST /api/auth/reset sig=..` needs no login), puts all three back to the defaults and ends every
session; a wrong token counts as a failed login, and both are logged. On the web page, the login row has a
"Lost passwords?" panel that shows the line to send the developer (with a Copy button) and a box
for the token, which is checked as 128 hex digits before it is sent; firmware without a developer
key says recovery is not possible (`tests/integration/reset_web_test.py`, `docs/web_reset.png`). Every POST must carry an `X-CEMS: 1` header and the cookie
is `HttpOnly; SameSite=Strict`, so another web page cannot submit changes. If the
instrument stops answering, a red "Connection lost, last update hh:mm:ss" banner appears
and the values grey out until polling recovers. Admin users can download a parameter
backup and restore it. Screenshots: `docs/web_ui.png`, `docs/web_ui_connection_lost.png`.

**Trends.** The Trends section charts up to 4 values (any scalar bool, int, uint or float
in the table) against time, over the last 1, 5, 15 or 60 minutes. Each value has its own
scale (click its name to show that scale on the axis); a gap shows where the link was
down; Hold freezes the view while recording carries on; hovering reads every value at
that moment; Save CSV downloads what is recorded. It is recorded in the browser from the
same 0.5 s value polls, so it costs the instrument no requests, no RAM and no API (about
0.5 KB of page). It holds what was seen while the page was open; the instrument's own
record is the data logger (5a). The chosen values are remembered by the browser.
Screenshot: `docs/web_trend.png`.

**State, quality and service (CIP-09, 10, 21, 22.1).** The header shows the instrument
state (`/api/info` "state") and the supervisory link (LINK OK / LINK LOST / NO LINK), and
any value whose quality is not valid (`/api/values` "q", keyed "id" or "id.index") is
greyed with its word, in the words and colours of `docs/UI_CONVENTIONS.md`; trend lines
break where a sample was not valid, and the trend CSV gains a quality column. The page
reads these fields when the instrument sends them. **Diagnostic package** (Event log
section, no login): one JSON file put together in the browser from `/api/info`, time,
alarms and their actions, firmware (with its update history), log status, I/O boards,
every setting with its value and quality (secret entries left out) and the event log CSV.

**Phones and tablets.** The page fits a phone's width (390 px) without sideways
scrolling: the parameter and alarm lists stack into cards, and wider tables (alarm
actions) scroll inside their own section. `web_ui_test.py` checks this.

**Keep-alive.** The server handles one HTTPS connection at a time. After a GET the
connection stays open (HTTP/1.1 keep-alive; bodies without a known length are sent
chunked), so the page's polls reuse it instead of opening a TLS connection each time.
It gives way at once when another connection is waiting, so no browser can hold the
server; it also closes after 5 s idle, after 200 requests, after any POST (browsers do
not retry a POST on a connection the server closed) and before an alarm email borrows
the TLS context. Measured with Chromium and the page open: about 0.7 new TLS
connections a second (all resumed by ticket) instead of one per request (3-4 a second).

**Several viewers.** Browsers open spare connections that may never send a request.
Accepted connections wait in a table of 3 (with the kept connection, the 4 HTTP sockets
in lwIP's budget) with no TLS yet; the handshake starts only when a connection's first
bytes arrive, and one that stays silent for 2 s is closed. Before this, each spare
connection held the server for its 2 s read timeout, and 4 browsers got one value poll
every 5-8 s with nothing on the page to say so. The port-80 redirect is polled without a
wait, so it no longer adds 50 ms to every HTTPS request. Measured on netsim with 4
browsers (`tests/integration/multi_viewer_test.py`): each gets its 2 value polls a second,
95 % within 52 ms; the cost is about 18 TLS connections a second, all resumed by ticket
(no public-key work), which still needs measuring on the H5. The page also gives up on a
request after 5 s, so a stalled server shows "Connection lost" rather than stale values
marked live.

**Slow links (remote access through a VPN router, D17).** Measured with Chromium's network
emulation against netsim (round trip / bandwidth: page live after, value polls a second,
95 % within): 100 ms / 2 Mbit/s: 0.6 s, 1.7, 71 ms; 300 ms / 1 Mbit/s: 1.3 s, 1.5,
180 ms; 600 ms / 256 kbit/s: 2.8 s, 1.1, 412 ms; 1.2 s / 128 kbit/s (poor cellular):
5.5 s, 0.8, 868 ms. The page never showed "Connection lost" on any of them: each poll
waits for the previous answer, so a slow link slows the updates instead of piling up
requests. The emulation adds the delay per request, not to the TCP and TLS handshakes,
so first loads on a real link are somewhat slower.

**Network on the H5.** lwIP 2.2.1 on FreeRTOS with BSD sockets; ST's H5 Ethernet driver,
ported to native FreeRTOS. The address comes from the parameter table (static
192.168.1.50/24 by default, or DHCP) and a change from the touchscreen or web takes effect
immediately. The MAC address is derived from the chip's unique ID: locally
administered, different on every unit, and stable across reboots.

## 5a. Event log and data logger

Fixes the old logger's faults (Analysis `48_Logging.md` L1-L8) as set out in
`70_Alarms_Logging.md` section 8.

**Two rings on one serial NOR flash.** The event ring (1/8 of the flash, at most 1 MB)
holds the audit trail: power-up with reset cause and firmware version, every external
parameter change (old and new value, touchscreen/web/Modbus, login level), mode changes,
logins and failed logins, password changes, backup restores, clock changes, storage and
log faults, and (next) alarms and firmware updates. The data ring (the rest) holds one
record per interval (`log_interval`, parameter 30, default 60 s): the **mean** of each
channel over the interval and its **quality** (OK, FLAG, BAD, N/A), not a snapshot. The
application lists its channels in `plat_app_t.log_chans` and calls `plat_log_put(i, value,
quality)` from the logic; the platform does the averaging, the timing and the writing.
Changes made by the unit itself are not audit events.

**Never stops, never clears.** When a ring is full the oldest 4 KB sector is erased
(decision D12): the old program stopped at 256 rows (L1) and erased the log when the
column set changed (L3). Here a configuration change writes a schema record (channel
names, units, decimals, interval) and the CSV gets a new header line at that point.

**Gaps (CIP-16.6).** When two consecutive data rows with the clock set are more than two
intervals apart (the unit was off, or the clock was stepped forward), the CSV has one row
at the first missing interval with every value empty and every quality `GAP`, so a
spreadsheet shows the hole instead of drawing across it. The power-up event says why.

**Power cuts.** Records are 64-byte slots (a data record with up to 32 channels is 4
slots; the schema up to 17), each with its own CRC-32, written only into erased flash and
read back to verify. A cut loses at most the record being written. The write position
is kept in FRAM (two copies, 0x7C00); if it is missing or stale, it is rebuilt by
reading the first slot of every sector (about 50 ms for 16 MB). A slot that fails
verification is skipped and the record is written again in the next one
(`LSTAT_NOR_FAULT`, which the alarm map raises as LOG_STORAGE_FAULT, alarm 3).

**Capacity** (16 MB part, e.g. W25Q128JV): events about 16,000; data with 10 channels
at 60 s about 45 days, at 300 s about 7 months. A 32 MB part doubles that; the driver
switches to 4-byte addressing by itself.

**Export.** `GET /api/log/events.csv` and `/api/log/data.csv` (optionally
`?from=&to=` in seconds since 1970) stream the whole ring; `/api/log/events?n=` returns
the newest events as JSON, which the web page shows. Without flash fitted (the DK), a 32 KB
RAM stand-in keeps the event log until power-off and the page says so.

## 5b. Firmware update

**Image header version 2 (CIP-01.3, 03.2, 25.1).** `tools/mkfwimage.py` now writes header
version 2, which adds the range of board revisions the image runs on (`--hw 0-1`; the board's
straps, `CUBEMX_SETUP.md` 3j), the settings layout version it was built with (`--layout`) and the
build ID (git short hash, `--build-id`, default the working tree's). Version 2 signs these fields
too (the signature covers header bytes 0..83 and 148..251). An image for other board revisions is
refused at upload and again at activation ("built for another board revision"); a version 1 image,
or a board without straps, is not range-checked. `/api/fw` shows `build` (running) and, for a
staged image, `staged_build` and `staged_hw`; `/api/info` shows `build` and `board_rev`.

**Update record (Gary, 20 Oct).** Every firmware change is stored in the instrument: the
date and time of the restart onto the new firmware, the date and time it was kept (or
rolled back), the versions from and to, and who asked (login level, web or touchscreen).
The last 6 changes are kept in FRAM (two CRC-checked copies at `STORE_META_ADDR + 384`,
so a power cut while writing loses nothing), outside the settings, so "restore defaults"
and backups never touch it. It is shown on the touchscreen's System screen ("Updated
2026-10-20 09:30, was 0.4.0"), in a table under Administration → Firmware on the web page
and in `/api/fw` (`history`), alongside the `fw_update` / `fw_rollback` events. A time
taken while the clock was not set is shown as "clock not set". A rollback is recorded
once the clock has been read after the restart (within 3 s).

The STM32H573 has two 1 MB flash banks; the running image is always mapped at
0x08000000 and the other bank is free. A technician (factory-level login) uploads an image file (`.cfw`, made by
`tools/mkfwimage.py` from the build's `.bin`) on the web page. It is streamed straight
into the free bank while the instrument keeps measuring: the header is checked first
(product, size), the payload is hashed as it arrives, and at the end the flash is read
back and hashed again. Nothing changes until the technician presses "Restart on x.y.z".

Then the boot record in FRAM is set, the banks are swapped (option bit SWAP_BANK) and the
unit restarts on the new image, which runs **on trial**. After `fw_trial_ms` (60 s) of
healthy running (the scan running, every task checking in, which the watchdog
enforces) it keeps itself and logs `fw_update 0.3.0>0.4.0`. If it resets before
that (watchdog, fault, or a second power cut; one power cut is tolerated), the next
boot swaps back to the previous image, which logs `fw_rollback`. The previous image
is never erased by an update, and an image that went bad in flash after staging is
never swapped to (checked again just before the swap).

**Gates before a restart (D23, the spec's "safe idle state", "optional local
confirmation" and "optional anti-rollback"):**

| Gate | Set by | Effect |
|---|---|---|
| Safe state | the application: `plat_app_t.fw_safe(why, cap)` | false + a reason ("a calibration is running") refuses the activation, shown on the web page and touchscreen; checked when asked and again by the store task just before the swap |
| Touchscreen confirmation | parameter 87 `fw_local_confirm` (admin) | an activation from the web is held; the touchscreen asks "Restart on firmware x? Asked from the web page (factory)"; Yes restarts, No refuses, nothing in 5 minutes drops it. A stolen tech login alone can then not restart a unit nobody is standing at |
| Minimum version | parameter 88 `fw_min_version` (factory; major<<16 \| minor<<8 \| patch) | older images are refused at upload, and at activation if the minimum was raised since. Use it when a version has to be kept off units |

Every step (asked, held, confirmed, refused, dropped, not safe) is in the event log.

Limits of this foundation, all planned:
- an image too broken to reach `fw_boot()` (very early in start-up) can't roll itself
  back. The watchdog keeps resetting it and the debugger or a bootloader is needed. An
  immutable first-stage bootloader (ST's OEMiRoT, or MCUboot) closes this and comes with
  secure boot;
- the minimum version is a setting in FRAM (factory level), not a hardware counter: it
  keeps withdrawn versions off units, but someone with the factory login can lower it;
- the free bank's physical number for the sector-erase register under SWAP_BANK
  follows ST's FLASH_SwapBank example; check it on the DK with the debugger attached
  before the first update.

## 5c. Touchscreen

Riverdi RVT70HSBNWC00-B (BT817, 1024x600, capacitive touch) on SPI, driven by the UI
task (lowest priority: it slows under load and never delays control). The BT817 draws
from a list of co-processor commands, so the H5 keeps no frame buffer: each frame is
about 2 to 4 KB of commands, built in RAM and sent in one burst. Touch comes back as
the tag of the widget under the finger, and an action happens on release over the
same widget (sliding off cancels it).

Screens (`docs/ui/all_screens.png`, rendered from the actual command lists):

- **Home**: up to six value tiles (`plat_app_t.ui_home_ids`) and the LOCAL/REMOTE
  control with a plain explanation.
- **Settings**: the whole parameter table, six rows a page, each row showing whether
  it can be changed and, if not, why ("Log in as admin to change this", "Not allowed
  in REMOTE mode", "Change this one on the web page" for byte buffers and arrays).
- **Edit**: numeric keypad with range shown; IP address entry; ON/OFF; a full
  keyboard for text (ASCII and UTF-16 entries). Writes go through `param_post` with
  source `PLAT_SRC_LOCAL`, so every limit, mode rule and interlock applies.
- **Log in**: operator/admin/factory with the web's passwords; five wrong attempts
  lock it for 30 s; a login lapses after 10 minutes without a touch.
- **Events**: the event log, newest first, paged.
- **System**: running and spare firmware, trial countdown, upload progress, "Restart
  on x.y.z" (factory/tech level, confirmed: the spec's local confirmation), network address/link/MAC,
  serial, restarts and last fault, calibration, log and store status, brightness.

A header shows LOCAL/REMOTE and the alarms (CAL INVALID, LINK LOST, LOG FAULT) on
every screen. Confirm dialogs come up before a mode change, a network change,
"restore defaults", a firmware restart, and any entry flagged `PF_CONFIRM` (critical
setpoints). Taking LOCAL control needs no login (the local operator can always take
control back); handing control to REMOTE needs an operator login. The screen dims
after 10 minutes; the first touch only wakes it. A display that doesn't answer is
retried every 5 s and the instrument runs on without it; the firmware trial treats a
fitted-but-dead display as unhealthy.

## 5d. Duplicate addresses, network security and remote access

**Duplicate address check.** Before a static address is used it is probed with ARP
(RFC 5227, lwIP's ACD). If another device answers, the instrument does not take the
address, so the device already using it (often the PLC) keeps working; the IP CONFLICT
alarm shows on the touchscreen header and the web page, the System screen says
"192.168.1.50 IN USE by another device", the event log records it, and the address is
checked again every 30 s and whenever the cable is plugged in. A device that claims the
address later is detected too (the address is defended and kept, and the alarm raised).
DHCP leases get the same check inside the DHCP client. Probing takes 2-7 s, during
which the unit has no address. Status: parameter 14 `net_status` (`NETST_*`).

**Security model (D11/D17).** Defence in depth, in the spirit of IEC 62443:
- HTTPS only for the web interface (D11, built): the unit's own key and self-signed
  certificate, fingerprint shown on the touchscreen (System) and the web page, the
  certificate downloadable (`/api/tls/cert`) to trust it on the site's laptops; port 80
  only redirects; logins with lockout, tech-level firmware updates with signed images
  (D14), every change in the event log;
- every web reply carries `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`
  and `X-Frame-Options: DENY`; the page also carries a Content-Security-Policy (scripts,
  styles, requests and frames only from the unit itself; no plug-ins, no framing) and
  `Referrer-Policy: no-referrer`. HSTS is not sent: browsers ignore it for an IP address
  and for a certificate they were told to accept;
- Modbus TCP has no security of its own (nor do the PLCs that use it): it stays on the
  control network, with the existing allowed-client IP and the "Modbus read-only"
  switch, and is never routed to the internet;
- the instrument makes no outbound connections of its own.

**Remote access (D17).** Never by port-forwarding the instrument to the internet. A
site that wants remote access fits an industrial remote-access router (for example
Ewon Cosy, Teltonika RUT with RMS, or Moxa/Phoenix equivalents) on the control
network, cellular or site internet. The router keeps an outbound, encrypted VPN to the
vendor's service; an engineer connects their laptop to that VPN and opens
`https://192.168.1.50/` exactly as on site, with the same logins. The instrument
needs only HTTPS, a gateway setting (already there) and a correct clock. A cloud
dashboard reached by the instrument itself (MQTT/HTTPS outbound) is possible later but
means building and running a service, and is not proposed now.

## 5e. Alarm manager

Answers Analysis `70_Alarms_Logging.md` section 8. The old program had two layers (source
bits in the logic, plus the panel's own alarm table) and they disagreed: texts landed on
the wrong alarms (K1, K2), range alarms were evaluated twice with different rules (K3),
one coil acknowledged 70 alarms and also cleared their sources (K7). Now there is one
layer. The logic only reports a condition each scan:

```c
alarm_set(ALARM_VALVE_FB_01, ValveOut[0] && !ValveFeedback[0]);
alarm_set_range(ALARM_AI1_RANGE, AnalogSensor[0], RangeMin[0], RangeMax[0], 0.5f);
```

and the manager owns everything else, from one `alarm_def_t` per alarm in
`plat_app_t.alarms`:

| Field | Meaning | Old program |
|---|---|---|
| `id`, `name` | 100..255 for the application (1..99 are the platform's); the name goes in the event log | the panel table ID |
| `severity` | status, warning, alarm, fault: the colour and the order of the list | (classification 2/4, meaning unknown) |
| `on_delay_ms`, `off_delay_ms` | the condition must last this long to raise / be gone this long to clear | delay on/off (10 s on the analog alarms) |
| `delay_param` | an integer parameter in seconds that replaces the on-delay (valve feedback settling time, N11) | none |
| `enable_param` | a bool parameter: off suppresses the alarm at once | `AnalogSuppression[n]`, but RAM (K4); make it `PF_PERSIST` |
| `text_param` | a text parameter with an editable description, shown everywhere | Modify Alarm Property (K1, K2) |
| `AL_LATCH` | stays active after the condition goes, until acknowledged | `FlowValidationAlarm` |
| `AL_NO_ACK` | status alarm: shown while active, nothing to acknowledge | alarms 79, 80, 87 (K8) |
| `AL_NOTIFY` | passed to `alarm_on_change` (notification, Stage 8) | the email rungs |

States: active and not acknowledged; active and acknowledged; cleared but not yet
acknowledged (so a short alarm is never missed). Acknowledging only moves the state: it
never touches the condition or a source tag and never re-sends anything (K7, N3). One
alarm or all, from:

- the touchscreen: the alarm pill in the header (count, and how many are new, coloured
  by the worst) opens the Alarms screen: most urgent first, an Acknowledge button per
  row and "Ack all" (`docs/ui/alarms.png`). No login needed (D19);
- the web page: the Alarms section and a badge in the header; acknowledging needs a
  login (every web write does);
- Modbus: write the alarm ID (65535 = all) to parameter 40 `alarm_ack`, wherever the
  application maps it; read 41/42 (counts) and 43/44 (16 words each: bit ID % 16 of
  word ID / 16 is active / unacknowledged), so the master finally sees the
  acknowledge state (K12). The demo maps them at holding 20 and input registers 0, 1,
  10-25 and 30-45.

Every raise, clear and acknowledge is in the event log, the acknowledge with where it
came from and the login level (K11).

Platform alarms (always present; the application doesn't define them):

| ID | Name | Raised when |
|---|---|---|
| 1 | CAL_INVALID (fault) | no valid factory data: measurement blocked |
| 2 | PARAM_WRITE_FAIL (fault) | the FRAM write did not verify for 2 s, or no FRAM (settings not kept) |
| 3 | LOG_STORAGE_FAULT (fault) | a log flash erase, write or read-back failed |
| 4 | IP_CONFLICT (alarm) | another device answers at the static address |
| 5 | REMOTE_LINK_LOST (alarm) | REMOTE mode and no remote traffic for the timeout |
| 6 | UNEXPECTED_RESET (warning, latched) | the last reset was the watchdog or a brown-out |
| 7 | FW_ROLLED_BACK (warning, latched) | a firmware update failed its trial |
| 8 | DISPLAY_FAULT (warning) | the touchscreen stopped responding for 5 s |
| 10 | NOTIFY_FAIL (warning) | an alarm email failed three times (clears at the next email that goes through) |
| 11 | RESOURCE_LOW (warning, latched) | heap or a task stack nearly used up (5h) |

Costs: 4.1 KB of flash and 2.7 KB of RAM for up to 96 alarms (`docs/MEMORY.md`); the
scan spends a few microseconds per alarm.

### Alarm actions (D20)

What an alarm does is the user's choice, per alarm, not the firmware's (Gary, 20 Oct:
"the user should be able to configure this accordingly"). Each alarm has any mix of:

| Action | What happens |
|---|---|
| Email | the raise (and the clear, if `mail_on_clear`) is emailed; alarms within `mail_gap` seconds share one email |
| Modbus | the alarm counts in parameter 45 `alarm_common` (bit 0 a Modbus-routed alarm is active, bit 1 one waits for an acknowledge; bits 2/3 the same for every alarm; bits 8-11 relays 1-4): the control system polls one register. Every alarm's own active/unacknowledged bits (parameters 43/44) are readable whatever this is set to |
| Relay 1-4 | drives an alarm relay: a digital output chosen per relay (parameter 47, 0 = not used), switching while any of its alarms is active, until acknowledged (a horn: the acknowledge silences it), or until cleared and acknowledged (a lamp) (parameter 48), optionally fail-safe (energised when healthy, parameter 49). The relay overrides whatever the application puts on that output; outputs go to the safe (de-energised) state on a fault, which a fail-safe relay reports as an alarm |
| nothing | shown on the touchscreen and web, logged; nothing else |

The application gives each alarm a default (`alarm_def_t.route`); the user's choice is
one byte per alarm ID in parameter 46 (bit 7 set = the user's choice, clear = the
default), persisted, and changed on the web page (Administration: Alarm actions, Alarm
relays) or on the touchscreen (Alarms, then Actions; admin login). Platform defaults:
faults and alarms email + Modbus, warnings Modbus. Changes are logged like any setting.

### Alarm email

`mail.h`. To the site's own mail server by IP (sites seldom have DNS or internet), as
SMTP with STARTTLS (587), TLS (465) or plain (25). Security choices:

- a login is only ever sent over TLS; plain SMTP is allowed without a login (an internal
  relay that accepts the instrument's address);
- the server is trusted by pinning its certificate: "Send test email" shows the
  fingerprint it presented, "Trust this server" stores it, and from then on any other
  certificate is refused before anything is sent. Unpinned, the link is still encrypted.
  No certificate authorities are stored (they would need updating, and the server is
  normally the site's own);
- the password is write-only (`PF_SECRET`): never shown on the web or touchscreen, never
  readable over Modbus, never in a backup file;
- TLS 1.2 with ECDHE (ECDSA or RSA server keys up to 4096 bits); the client shares the
  web server's one TLS context and pool, running in the web task between requests, so a
  send (a second or two) only delays a web request, and RAM is not doubled;
- failures are retried after 1 and then 2 more minutes; after three the batch is dropped
  and NOTIFY_FAIL raised; every attempt is in the event log;
- with email off (the default) the instrument opens no outbound connections at all.

The email: subject `[<unit name>] ALARM: <first alarm> (and more)`, one line per
raise/clear with time, text, severity, name and ID, then everything active now, the
serial number and the unit's https:// link.

## 5f. Clock (D22)

The RTC holds the site's **local standard time** with no daylight saving: a CEMS record
must not jump an hour twice a year, and the regulator's reports are usually in local
standard time. Parameter 82 (`time_utc_offset`, minutes) says how far that is from UTC;
SNTP and email Date headers use it.

| Way | Who | Notes |
|---|---|---|
| Touchscreen, System > Set clock | admin | digits for year, month, day, hour, minute (and seconds) |
| Web page, Clock | admin | "Set to this computer's time", or a typed date and time |
| Modbus / any parameter writer: parameter 85 `clock_set` | operator, REMOTE mode for the PLC | local seconds since 1970; how a DAHS keeps the clock in step. Parameter 86 `clock_unix` reads the clock |
| SNTP from the site's time server (parameter 83, IP; 84 interval, default 60 min) | automatic | RFC 4330 client, one packet each way; the answer must echo our request; unsynchronised servers (stratum 0/16, leap indicator 3) are ignored; the clock is only stepped when 2 s or more out, so the log is not filled with tiny corrections; 60 s retry after a failure |

Every change goes to the event log once, with its source and login level (the
logger's own "clock stepped" detection stays for unexplained jumps). A request is
applied by the scan task, between scans, so no scan sees a half-set clock. The
logger closes its averaging interval at a step, as before.

**Clock quality (CIP-02.6).** Parameters 316 `clock_synced`, 317 `clock_sync_time` and 318
`clock_source` (also in the Modbus block and `/api/time` `synced`, `sync_time`, `source`) say
whether the clock is trustworthy: synchronised means set or confirmed from outside (an SNTP
answer, even one that did not need a step; a setting from the web, touchscreen or Modbus) within
the last 24 h (`PLAT_CLOCK_SYNC_MAX_S`). It is not kept across a restart: after power-up the clock
runs on the RTC battery and shows "not synchronised" until SNTP answers or someone sets it.

The product board needs a VBAT supply for the RTC (`docs/CUBEMX_SETUP.md`).

## 5g. Touchscreen fonts (D24)

The BT817's ROM fonts have ASCII only, so a CEMS unit such as "mg/m³", "°C" or "µg"
could not be shown. `tools/mkfont.py` renders DejaVu Sans at the sizes of the ROM fonts
the UI uses (26, 28, 30, 31) as EVE "legacy" L4 fonts: ASCII 32..126, plus in codes 1..31
° ² ³ µ ± · × ÷ ¼ ½ ¾ ≤ ≥ ≠ ≈ Ω Δ √ ‰ € £ ä ö ü Ä Ö Ü é è à ñ (Greek mu and the ohm sign
map to µ and Ω). The UI's text calls convert UTF-8 to those codes (`eve_font_encode`);
anything else shows as "?".

The spec suggested programming assets into the display module's own flash. For fonts
that is not needed: they are 24 KB compressed in the H5's flash (249 KB once inflated,
in the BT817's 1 MB RAM_G), so they are versioned with the firmware automatically, need
no flash programming step on the module and no wear, and do not depend on which flash
part Riverdi fits. At start-up the co-processor inflates them (`CMD_INFLATE`, about 10 ms
at 30 MHz SPI) and each is checked with `CMD_MEMCRC` against the CRC in the firmware; a
font that fails is simply not used (the ROM font of that size is). The module's flash
stays available for large images (logos, photos) if the UI ever needs them.

The link stays single-lane SPI: a frame is 2-4 KB of commands (about 1 ms at 30 MHz),
so QSPI would save nothing visible and costs four more pins.

**To check on the display:** the metric block's glyph pointer convention for a first
character other than 0 (we store the address of code 0's cell, as the BT81x guide
describes); if glyphs appear shifted by one character, that is the line to change in
`eve_fonts_load()`.

## 5h. Health and the soak test

The supervisor task samples the FreeRTOS heap (free now, lowest ever; lwIP allocates from
it) and every task's stack margin (lowest-ever unused, including lwIP's tcpip and
Ethernet tasks) every 5 s. `/api/info` returns them under `health`, the System screen
shows the heap's worst figure and the tightest stack, and alarm 11 RESOURCE_LOW (latched,
email and Modbus by default) is raised when the heap's worst falls below 4 KB or a stack
margin below 64 words, so a slow leak or an undersized stack is reported long before it
stops the unit.

`tests/integration/soak_test.py [minutes]` runs the networked simulation under a steady
load (3 Modbus clients, keep-alive and one-shot HTTPS polling, web writes every second,
an alarm raised and acknowledged every 20 s, event CSV downloads) and watches the
process's memory and open sockets every 10 s. It passes when no request failed, the
socket count stays flat, memory stops growing after the first third and the address
sanitizer reports no leak at exit. The spec's 72-hour soak is the same script with
4320 minutes; on the DK the figures to watch are `health` on `/api/info`.

## 5i. Serial number (D25)

The serial (parameter 50) names the unit on the web page and touchscreen, in alarm
emails, in backup and CSV file names and in its HTTPS certificate. Changing it needs
developer authority, which no login gives on its own:

- The parameter carries `PF_DEV`: the touchscreen, the web parameter write and Modbus are
  refused at every level, factory included ("read only" / "developer"), and a parameter
  backup restore never sets it (the file still records it), so a backup loaded into a
  second unit cannot clone the first unit's identity.
- The one way in is `/api/serial` with a factory login **and** a token: an ECDSA P-256
  signature, by the developer key, over
  `CEMS-SERIAL v1 uid=<chip unique ID, 24 hex> old=<serial now> new=<serial wanted>`.
  The developer key is separate from the technicians' firmware key (D14) and stays with the
  developer; the firmware holds only its public half (`dev_pubkey.c`). Because the text
  names the chip and the current serial, a token works on one unit, once: after the change
  "old" no longer matches, and on another chip the ID differs.
- `tools/serial_token.py` makes the token off the unit; the web page shows the exact text
  and the command line. Firmware built without a developer key cannot change the serial at
  all (there is deliberately no development fallback).
- Every change is logged (old → new, who, from where), and so is every refused token.

The HTTPS certificate takes the new serial at the next restart. The chip ID comes from
the H5's 96-bit unique ID (`bsp_chip_uid`), the same one the MAC address is derived from.

## 5j. Application screens and records (D26)

Asked for by the VM/MUX build (its `docs/SCREENS.md`, requests R1-R5).

**Records.** A sequence or schedule is one `PARAM_RECORD` table entry holding the whole C
structure (up to 1 KB, `PF_PERSIST` keeps it in FRAM, not on Modbus). It is written only
whole: `param_record_post()` copies the new image into a single slot, the scan applies it
at its next command point (access and mode checked again, then the record's
`check_block(id, image, why, cap)`, then the copy, all in one scan), and
`param_record_result()` gives the outcome with the check's reason. One record write is in
flight at a time. Chunked block writes and `/api/write` are refused for records, so the
logic can never see half an edit.

**Editors.** `ui_record_t` describes a record's fields (`ui_field_t`: text, unsigned,
signed, tick box, milliseconds shown as seconds, h:m:s, a list of names), either as a
table (rows = periods, a "rows in use" count such as `NumberOfPeriods`) or as a form
(tick-box rows such as Month[12], Day_Of_Week[7]). A field's maximum can follow a live
parameter (inlet <= FirstInlets). The touchscreen and the web page both edit a copy: tap a
cell for the keypad (or tick a box), + and - for the rows in use, Save sends one record
write and shows "saved" or the check's reason, Cancel drops the copy.

**Screens.** `ui_screen_t` lists widgets by parameter ID: a section heading, a value (a
scalar or one element of an array; tap to change it), a button (writes a command
parameter; lit by a state parameter, greyed by an enable parameter), a grid of numbered
items (valves, inlets: count, visible, touchable and lit from arrays or scalars, names
from consecutive text parameters; a tap writes the item number), a dialog (shown while a
status parameter is set; Yes/No or OK commands), and a record button. Any widget can be
shown only while a parameter is at least a value (the second tube while
NumInterconnections >= 2). The application's screens come first, as tabs, with the
platform's own Status screen beside them; the Settings list opens a record's editor too.
On the web page the same description is drawn under its own heading (`/api/ui`,
`/api/record`). Commands go through the table like any write, so levels, LOCAL/REMOTE and
`PF_CONFIRM` ("are you sure?") apply. `ui_app_check()` refuses, at start-up, a
description naming a missing parameter or a field past the end of its record.

**For the application build:** `app_select()` names the application a build runs, and
`APP=<dir>` with that directory's `app_vars.mk` links it into `make netsim` and
`make target` (R1). `ui_last_touch_ms` is the uptime of the last touch, for the old
screen-saver logic (R3). The target check and the memory report include `app_vmmux` when
its directory is present (R5): with it (and the I/O bus, 5k) the firmware is 485 KB of flash
(48 %) and 438 KB of RAM (68 %).

## 5k. I/O boards on Ethernet or RS-485 (D27)

The valves, loads, relays and digital inputs sit on separate I/O boards (the *I/O Board
Design Specification* and the *I/O Module Requirements* documents). Each board has 8 outputs
of 0.6 A with per-channel diagnostics (bank A), 8 outputs of 1 A (bank B), 2 socketed relays
and 8 isolated inputs with wire-break detection. The main board is the master.

**The link (Gary, 24 Oct).** The boards are on **Ethernet** with **Modbus TCP** (an STM32H563
with a LAN8742A on each board, the same chip as the main board). **Their own network (Gary,
25 Oct):** the main board has two Ethernet ports, the client port (the H5's MAC, lwIP: HTTPS,
Modbus TCP, email, SNTP) and the **I/O port**, a WIZnet W5500 on its own SPI (`w5500.h`,
`port/stm32h5/w5500_stm32h5.c`, `CUBEMX_SETUP.md` 3h). The W5500 runs TCP/IP in its own hardware
sockets, so the I/O traffic never enters lwIP and nothing can route between the plant network
and the I/O boards. Parameter 102 `iob_link`: 2 = the I/O port (default), 1 = Ethernet on the
main port through lwIP (the DK, which has no W5500), 0 = RS-485. The single RS-485 pair (Modbus
RTU, up to 100 m) stays as a setting, for the bench and older boards. The CORE PCB has no RS-485
transceiver (Gary, 25 Oct: the second RJ45 replaces it), so `BSP_RS485_HUART` stays undefined
there. A write of a link whose port this board does not have is refused (`PERR_INTERLOCK`, HTTP 409).

On the I/O port `iob_w5500.c` is the transport: sockets 0-2 are the three boards' connections,
socket 3 discovery, 4-7 free. It sets the chip up on first use (its own IP `iob_port_ip`,
parameter 103, default 192.168.1.60, and `iob_port_prefix`, 104, default /24; MAC: the main
port's with bit 1 of the last byte flipped), tries again once a second while the chip does not
answer, applies a new IP at once, and keeps `iob_port_status` (105: 1 chip found, 2 link up)
current. ARP and TCP retries are 20 ms x 3, so a board that is not there fails within the
cycle. Commissioning on the I/O port gives the boards the I/O port's IP as their supervisor
(no gateway, and the main port's DHCP setting does not matter). A board's IP may not be the I/O
port's own. The register map, the requests and every rule
below are the same on both: the master builds the same RTU frames and, on Ethernet,
`iob_net.c` turns each into a Modbus TCP frame (MBAP header, unit id = the board's address,
no CRC) and the reply back (`iob_net.h`). Each board has an IP (`iobN_ip`, parameters
146-148, default 192.168.1.61-63) as well as its address; the master keeps one TCP
connection per board on port 502 (`platform/net/iob_tcp.c`, sockets: lwIP on the H5, POSIX on
the PC), opened on first use, with a 50 ms connect timeout and 15 ms for each reply, so three
boards still fit the 20 ms cycle. A board's settings conflict if two enabled boards have the
same IP (on RS-485: the same address) or an IP is not a usable one.

**The application does not change.** Board k fills the platform's generic channels: its
18 outputs from `iobK_first_do` and its 8 inputs from `iobK_first_di` (defaults: board 1
outputs 1-18 / inputs 1-8, board 2 19-36 / 9-16, board 3 37-54 / 17-24). The image grew to
64 inputs and 64 outputs for this. A board's inputs replace those channels; its outputs are
taken after the application and the alarm relays, so an alarm relay can be a board's relay.

**The bus.** 115200 baud, 8N1. Every 20 ms each running board gets one function 23
request that writes its outputs (with a sequence number the node echoes once it has applied
them) and reads its 12-register status block: about 5 ms per board, so three boards fit the
cycle. A new or returning board is first identified (magic number, register-map version,
channel counts: anything else is "wrong type" and is never written to), then sent its
settings (hold time, input filter, wire-break and open-circuit checks per channel). A
node that restarts says so (CONFIG_NEEDED) and gets its settings again.

**Safe state.** The node holds its outputs while the link is quiet, and turns everything
off when no output write has come for its hold time (parameter 100 `iob_hold`, default
5 minutes, 1 s to 10 min; Gary, 22 Oct: a short break or a main-board restart must not
drop the valves). Its hardware gate still turns everything off within about 50 ms if the
node's own firmware stops. The master sends all-off when the scan has not produced outputs
for 250 ms, so a stopped scan cannot hold valves open over the bus, and
`plat_io_safe_state()` (every fatal path) sends all-off at once if the bus task gets the
chance. A board with no valid reply for 1 s (`PLAT_IOB_LOST_MS`, whatever the hold time)
is "not responding": its inputs read 0, all its channels are marked faulty and IOB_LOST is
raised, while the node may still be holding its outputs. A few lost or damaged frames are
only counted.

**Faults.** Bank A reports open circuit (only for channels with the check on, so a channel
with no valve fitted does not alarm), overload/short and over-temperature per channel. Bank
B has one fault line for its 8 channels. A missing field supply marks its bank (and, for
supply A, the inputs). Inputs report a wire break where the check is on.

| Alarm | ID | Raised when |
|---|---|---|
| IOB_LOST | 12 | an enabled board does not answer (fault, email) |
| IOB_CONFIG | 13 | a board is the wrong type, or its channels or address overlap another's |
| IOB_OUT_FAULT | 14 | any output fault for 1 s |
| IOB_IN_FAULT | 15 | any input wire break for 1 s |
| IOB_SUPPLY | 16 | a field 24 V supply missing for 0.5 s |
| IOB_NODE_RESET | 17 | a node was restarted by its own watchdog (latched until acknowledged) |
| IOB_TEMP_FAULT | 18 | a configured temperature input is open, shorted, out of range or its converter has failed, for 3 s |

Every change goes to the event log as "I/O board": board 2: online / not responding /
out 3 open circuit / in 5 wire break / field A 24V missing / restarted / watchdog reset.

**Settings** (admin level, parameters 100-104, 110-139 and 146-148): `iob_hold` (300 s), `iob_filter` (8 ms, as the old panel), `iob_link` (the I/O port), `iob_port_ip`, `iob_port_prefix`, and
per board `enable` (off by default), `address`, `ip`, `first_do`, `first_di`, `open_check`
(bits: bank A outputs with a valve fitted), `wire_check` (bits: inputs with the wire-break
resistor). Status, read-only: `status`, `out_faults`, `in_faults`, `errors`, 140-145
`inputs` / `outputs`, and 105 `iob_port_status`. `/api/iob/net` reports `"link":"ethernet"` for both Ethernet links, with `"port":"io"` or `"main"`, and its `supervisor` block is the address the boards see (the I/O port's on the I/O port).

**IP and address on Ethernet (commissioning).** A new board has no IP and no address: it
serves no Modbus TCP, but answers discovery (UDP port 50223, `iob_net.h`), whatever subnet
it is on. Administration → I/O boards on the network on the web page (admin login,
`docs/web_iob_net.png`) → *Find boards* broadcasts FIND and lists every board that answers:
serial, chip ID, MAC, IP, address, and whether it is running or belongs to another
supervisor. *Give it* Board 1, 2 or 3 sends SET to that board's chip ID with the slot's IP
(`iobN_ip`) and address (`iobN_address`) and this supervisor's own IP, mask and gateway (the
supervisor needs a fixed IP for this: refused while `net_dhcp` is on). The board saves them,
reads them back and answers with what it saved, then changes over to the new IP; the master
reports OK only when the answer matches, and the board comes online by itself within a
second ("IP .1.62 set" in the event log). From then on the board takes settings and Modbus
TCP connections only from that supervisor's IP, so a second supervisor on the network can
neither take it over nor drive it. Setting its address to 0 over USB makes it a new board
again. An IP already used by another listed board, or by the supervisor, is refused before
anything is sent (`iob_dc_find` / `iob_dc_set`, `POST /api/iob/net/find`,
`POST /api/iob/net/set` with `uid` and `board` (and `ip` to change the slot's IP at the same
time), result on `GET /api/iob/net`).

**Addresses on RS-485 (commissioning).** A new board has no address and answers only at 246
(`IOB_COMMISSION_ADDR`), for reads and the address change. Administration → I/O board
addresses on the web page (admin login, `docs/web_iob_commission.png`) finds it ("Find new
board", or "Read board at address" for any address), shows its serial number, chip ID and
firmware, and gives it the address of Board 1, 2 or 3 or any free address: one guarded FC16
of registers 40-42 (key 0xA55A, address, complement; node requirements FW-61). The master
first checks that nothing answers at the new address, and reads the board back there before
it reports success; the change goes to the event log ("addr 246->2 set"). Connect one new
board at a time. The bus task does the request between the boards' own cycles
(`iob_cm_find` / `iob_cm_assign`, `POST /api/iob/find`, `POST /api/iob/assign`, result on
`GET /api/iob/commission`). The setting `iobN_address` is 1-245: 246 and 247 (the node's USB
port) are never a running board's address.

**Temperatures and the pump (register map 2, 25 Oct).** The rev A board adds an ADS124S08
front end with three universal connectors and one PWM output with direction and tach for
a pump or fan (lessons from the legacy dryer slave: per-channel sensor type and calibration,
explicit sensor faults). Connector c gives temperature channels 2c-1 (A) and 2c (B): two
thermocouples (K, J, T, E, S; cold junction from the ADC's own sensor), or one RTD (PT100 or
PT1000, 2-, 3- or 4-wire) or one NTC (R25 and beta set per connector) on A. The board
converts to 0.1 °C and reports a state per channel (OK, off, waiting, open, short, under or
over range, ADC failed, cold junction failed) plus front-end flags (ADC, supply, CJ,
reference). A map 1 board still works: it simply has none of these.

| Registers | Contents |
|---|---|
| 3-4 | PWM command 0-1000 (0.1 %), direction; written with the outputs (function 23 of 0-4) |
| 28-31 | PWM applied, tach rpm, temperature flags, conversion count (status read 16-31) |
| 79 | channel counts: low byte temperature channels (6), high byte PWM outputs (1) |
| 80-92 | temperatures (0.1 °C, -32768 = none), their states, cold junction |
| 96-106 | sensor type per connector, NTC R25 (10 Ω units) and beta, PWM frequency (10-25000 Hz), tach pulses per revolution |

The master reads 80-92 every 500 ms (`PLAT_IOB_TEMP_MS`, one board per cycle, and only when
a sensor is configured) and sends 96-106 when they change, and to a restarted board. It
applies the calibration (`iob_temp_cal`: offset in 0.01 °C plus span in 0.01 %) and puts
each channel in the platform's RTD channels (`rtd_cdeg` / `rtd_status`, 24 of them) from
`iobK_first_temp` (defaults 5, 11, 17). The pump runs from analogue output `iobK_pump_ao`
(0 = none; `ao_raw` in 0.1 %, the sign sets the direction), and is held at 0 by the node's
hardware gate like every other output. Parameters 160 + 16(K-1) + 0..13: `first_temp`,
`pump_ao`, `temp_type[3]`, `ntc_r25[3]`, `ntc_beta[3]`, `temp_offset[6]`, `temp_span[6]`,
`pwm_freq`, `tach_ppr`, and read-only `temps[6]`, `temp_state[6]`, `cj_temp`, `pump_rpm`,
`pump_duty`. An unknown type is sent as none. On the web page each board card shows a
temperature table and the pump line; an admin can press *Set to…* beside a channel and enter
a reference thermometer's reading, which sets that channel's offset (`POST /api/iob/temp/zero`
with `board`, `ch`, `ref`: refused unless the channel is reading, or if the change is over
50 °C; an ordinary logged parameter write).

**Screens.** System → I/O boards on the touchscreen (`docs/ui/iob.png`) and the I/O boards
section of the web page (`docs/web_iob.png`, `/api/iob`): each board's state, its
outputs and inputs as lit squares, faults in words, firmware version and request counts.

**Hardware.** One USART in RS-485 mode (the USART drives DE) and an isolated transceiver
(`docs/CUBEMX_SETUP.md` 3f, `docs/HARDWARE.md` H11). `rs485_stm32h5.c` sends with
interrupts and receives to line idle; the task sleeps meanwhile.

**The simulated board** (`platform/bsp/iob_node_sim.c`) implements the register map exactly
as the node firmware must; it is the master's test partner, and netsim runs two of them
(console: `node 1 open 4`, `node 2 power 0`, `node 1 wdg` ...).

## 5l. Instrument state, quality, safe mode and supply (CIP-07 to CIP-10)

Agreed with Gary 26 Oct (`INSTRUMENT_PLATFORM_SPEC.md` sections 6 and 7). The aim is principle P1: a
plausible number from a stalled or faulted instrument never looks valid.

**State.** Parameter 300 (`state`, read-only) holds one of STARTING, INITIALISING, WARMING_UP,
READY, MEASURING, MAINTENANCE, CALIBRATION, FAULT, SAFE_MODE. The application sets WARMING_UP,
READY or MEASURING with `plat_app_state()` and asks for CALIBRATION the same way. The operator
selects MAINTENANCE with parameter 301 (`maintenance`, operator level, kept across a restart so a
reset during maintenance does not report valid data). The platform forces FAULT while
CAL_INVALID or an application alarm marked `AL_STOPS` is active, and SAFE_MODE after a reboot
loop (below). Precedence: SAFE_MODE, FAULT, CALIBRATION, MAINTENANCE, then the
application's state. Every change except the normal start is an event (`state`). Parameter 302
holds the clock when the state was entered.

Actions the state refuses (`plat_state_allows`): a firmware restart and restore defaults in
CALIBRATION (the web page shows "not allowed in CALIBRATION"); a calibration in WARMING_UP,
FAULT and SAFE_MODE; the application's logic in SAFE_MODE.

**Quality.** The application lists its measured values in `plat_app_t.meas`
(`plat_meas_def_t`: parameter ID, array index, maximum age, valid range; up to 32) and calls
`plat_meas_update(i, own_quality)` whenever it writes one. Each scan the platform works out the
reported quality:

| Condition | Quality |
|---|---|
| State not MEASURING | WARMING_UP: warmup; MAINTENANCE: maint; CALIBRATION: cal; FAULT: fault; otherwise unavailable |
| Never updated | unavailable |
| The value's own quality is not valid | that quality |
| Not updated for longer than its maximum age | stale |
| Outside its range, or NaN | range |
| Otherwise | valid |

Codes (`PLAT_Q_*`, in this order): 0 valid, 1 stale, 2 unavailable, 3 range, 4 warmup, 5 maint,
6 cal, 7 fault. The data log keeps OK/FLAG/BAD/N/A as its summary.

**Where it shows.** Modbus: parameter 300 for the state and parameter 303 (`meas_q`, one register
per measurement, in table order) as a status block; the application's map places them (the demo:
input registers 50 and 52-83). Web: `/api/info` has `"api":2`, `"state"` and `"state_since"`;
`/api/values` has a sparse `"q"` with only the values that are not valid
(`{"1002":"stale","1010.3":"cal"}`); `/api/params` marks measurements with `"q":true`.
Touchscreen: `plat_state()`, `plat_state_name()`, `param_quality(id, index)`,
`plat_quality_name()`, `plat_link_age_ms()` (time since the last Modbus or web request, for the
header's link indicator) and `plat_state_request()` to enter or leave maintenance.

A measurement table that names a missing or non-numeric parameter stops the start-up like the
other tables (fault record "meas table").

**Reboot loop and safe mode (CIP-08.5).** The fault record (`plat_diag`, kept across a reset)
counts crashes: a watchdog reset, or a software reset after a recorded fault (task silent, stack,
heap, assert, CPU fault). Three crashes (`PLAT_LOOP_RESETS`) with less than 10 minutes
(`PLAT_LOOP_WINDOW_MS`) of running between them in total start the unit in SAFE_MODE: none of the
application's code runs, not even its `init`, and every output stays off. The network, the web
page, Modbus, the touchscreen and firmware update all run, so the unit can be looked at and fixed
remotely, and platform alarm 19 SAFE_MODE is raised (email, Modbus). A power cycle, a clean restart
(a firmware update) or a boot that runs for the whole window starts normally again.

**Supply and power-fail shutdown (CIP-07.3, CIP-07.5).** The ctrl task reads the 24 V supply
every ms (`bsp_supply_mv`, `CUBEMX_SETUP.md` 3i) into a 16-reading average, shown as parameter
305 (`supply_mv`, mV; 0 = not measured on this board). Outside 19-30 V for 2 s: alarm 20 SUPPLY
(warning, email, Modbus). Two readings in a row below 16 V (2 ms; not the average, since the board holds up for only about 20 ms from 16 V at full load, `CORE_PCB.md` 5a) and the supply is failing: the store task starts no new FRAM
image write or log-flash write (it lets the event log run for 60 ms more so the `power_fail`
event, with the voltage, is written first). FRAM images are dual-copy with CRC anyway, so the
hold is to keep both copies and the log's current page intact, not to save the last settings.
Back above 18 V for a full second (a dip, not a loss): `power_restored`, and writing resumes,
with nothing lost (changes made during the hold are saved then).

## 5m. Calibration records (CIP-18)

The product declares its calibrated channels (`plat_cal_channel_t`: a name, the PF_FACTORY float
parameters holding the coefficients, an interval in days, an optional check hook). The procedure
itself (gases, waits, the maths) stays in the product; the platform owns the record, the
permission, the state and the history (`plat_cal.h` has the whole sequence):

| Step | Who | What the platform does |
|---|---|---|
| Begin | touchscreen, web (`POST /api/cal cmd=begin&ch=N`) or the product | Refused below setting 321 `cal_level` (factory setting, default admin), in WARMING UP, FAULT, SAFE MODE or start-up (factory login may calibrate in FAULT, so a unit that lost its calibration data can be recalibrated). Otherwise CALIBRATION until the end; event `cal` "start"; a history slot written ABORTED, so a power cut part way leaves an honest record |
| Procedure | the product's logic, while `plat_cal_active()` | Collects up to 4 points (`plat_cal_point`: reference, reading as found, reading as left) and the proposed coefficients (`plat_cal_propose`) |
| Accept | anyone with the level (`cmd=accept`) | The check hook may reject the whole calibration; every coefficient is range-checked; all written or none. PASS (now in force) or FAIL (nothing changes); event `cal` with the reason |
| Abort | `cmd=abort`, or 2 h with no end | ABORTED |
| Revert | factory login (`cmd=revert&ch=N`) | The factory record's coefficients back in force, recorded REVERTED |

A calibration at factory login is a factory calibration: it replaces the channel's factory
record, which field calibrations never touch (CIP-18.3). Records are in the FACTORY region of
FRAM (dual copy; survive firmware updates and restore defaults, CIP-18.5): 2.4 KB for 8 channels
and 12 history records on the 128 KB part, 2 and 3 on the 32 KB part. A channel's due date is the
record in force plus its interval; alarm 21 CAL_DUE (warning, email, Modbus) while any channel is
overdue. Commands go through parameter 320 like every other write, so the local/remote mode rules
apply (web and Modbus only in REMOTE; Modbus never). `GET /api/cal` gives the channels (in force,
factory, due time, overdue), the history (newest first) and which channel is calibrating.

**On the web page** (Calibration section, shown when the product declares channels): each
channel's record in force, its factory record and its due date (marked overdue); Begin and,
at factory login, Revert to factory; while a channel is calibrating, a box with Accept and
Abort; the history with outcome, who, the points (reference, as found, as left) and the
coefficients. Refusals show the instrument's reason. `tests/integration/cal_web_test.py`
runs a calibration and an abort through the page. Screenshot: `docs/web_cal.png`.

## 5n. Service history (CIP-22, CIP-23.4, CIP-05.4)

| Record | What | Where it shows |
|---|---|---|
| Operating hours | powered hours, and hours since the last service log entry, in 0.1 h steps (up to 6 minutes per power cycle is not counted) | parameters 330, 331; `/api/info` `run_hours`; `/api/service` |
| Service log | the last 8 entries (2 on the 32 KB part): date, login level, technician, work done; entered at factory login (`POST /api/service/log tech=..&work=..`); each entry restarts "since service" and logs a `service` event | `/api/service` |
| Test record | the manufacturing test: date, station, pass/fail per test (up to 24, in the test procedure's order), overall pass; written at factory login (`POST /api/service/test station=..&r=1,1,2`) | `/api/service` |
| Configuration checksum | CRC-32 over the ID and value of every user setting (not factory, not secret, not status), worked out every second: two units with the same checksum have the same settings | parameter 332; `/api/info` and `/api/service` `config_crc` (8 hex digits) |

The first three are in the FRAM factory region (dual copy; survive firmware updates and restore
defaults) and belong to the unit: a backup carries them for reference, but restoring it into
another unit never copies them (`RF_UNIT`, like the serial number).

On the web page, the Service section shows operating hours, hours since service, the
configuration checksum, the service log and the last manufacturing test (PASSED/FAILED badge
plus a pass/fail/not-run count per station). At factory login it also shows two forms: add a
log entry (technician up to 15 characters, work up to 39) and record a test result (station up
to 11 characters, results as `1,1,2,...` where 1 = pass, 2 = fail, 0 = not run; anything else
is refused before it is sent). Below factory login the forms are hidden and a 403 says so. The
section hides itself on firmware without `/api/service`. Checked by
`tests/integration/cal_web_test.py`; screenshot `docs/web_service.png`.

## 5o. Start-up self-test (CIP-11.1)

After every start the unit works through a fixed list of checks (`plat_selftest.h`), each
with one result: pass, fail or skipped.

| Check | Pass when | Skipped when |
|---|---|---|
| `image` | the running firmware hashes to the SHA-256 in the header the update left in its flash bank (16 KB per store-task pass, about 0.6 s for 500 KB) | no header for this version (programmed with the debugger) |
| `ram` | a March C- test of a 1 KB block passes (before the tasks start; the H573's SRAM also has ECC) | - |
| `rtc` | the clock's seconds move on within 3 s | the clock has never been set |
| `settings` | the FRAM answers and holds (not the RAM fallback; no write or table error) | - |
| `calibration` | a valid factory image was loaded | - |
| `log_flash` | the log flash answers and is large enough | the RAM stand-in (no flash fitted: the DK) |
| `net_phy` | the LAN8742A answered when the Ethernet driver started | nothing reported it (PC simulation) |
| `io_port` | the I/O-port W5500 answered when the I/O bus first used it | not used in the first 10 s (none fitted, no board enabled) |
| `display` | the touchscreen answers | no display on this hardware |
| product checks | up to 8 of the product's own (`plat_app_t.selftests`), run once at 10 s | the check says so |

The first six are known within about a second; the rest 10 s after start. Then parameter 333
holds the failed bits (bit *i* = check *i* above, bit 16 + *j* = product check *j*), the event
log gets one `selftest` event ("passed", or the failed names), `/api/info` `selftest` lists
every result, and alarm 23 SELFTEST (warning, email and Modbus) is on while a check failed that
has no alarm of its own (settings, calibration and display already raise PARAM_WRITE_FAIL,
CAL_INVALID and DISPLAY_FAULT).

On the web page, the Service section shows the self-test as one badge per check (pass, fail,
"not checked" for skipped, or pending), with a summary line naming any that failed. It is visible
without a login (`tests/integration/cal_web_test.py`, `docs/web_service.png`).

**Fault and alarm code list (CIP-11.3).** `docs/FAULT_CODES.md` lists the platform's alarms
(with what to check), self-test checks, fault record codes, reset causes, event codes and
firmware update refusals. `make fault-codes` writes it from the firmware's own tables
(`tools/fault_codes.c`), and `make test` fails if it is out of date or if a platform alarm has
no help text. A product's own list: `make fault-codes APP=<dir>`. `/api/alarms` carries the same help text
(`help`), and the web page's alarm list shows it under each alarm as "What to check".

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
11. **Logged values.** The old project's LOG columns become `plat_log_chan_t` entries (name up to 16 characters, unit up to 6). Call `plat_log_put()` once per scan per channel with the real reading and its quality (FLAG while in calibration or purge, BAD on a sensor fault, N/A when the channel is not measured); never a last-held value. Alarm 3 (LOG_STORAGE_FAULT) is raised by the platform itself. A value logged while one of its alarms is active can be marked FLAG with `alarm_active(id)`.
12. **Touchscreen.** List the values the operator should see first in `plat_app_t.ui_home_ids`. Mark critical setpoints `PF_CONFIRM`. `Touched_Bit`/`VNC_Touched_Bit` (rule 4) are not needed by this UI; if the logic uses them as "someone is at the panel", the UI can set them from its activity time.
13. **Alarms from `70_alarm_map.csv`.** One `alarm_def_t` per distinct `new_id` from 100 up (the core IDs 2, 3 and 10 are already the platform's), in `plat_app_t.alarms`; `alarm_set()` from the logic where the old program set the source bit (the source arrays `AlarmsVM`, `AlarmsMUX`, `FBAlarmsMUX` can stay for the old Modbus coils, but nothing should clear them on acknowledge any more). Range alarms: `alarm_set_range()` with the retained limits and an enable parameter; the analog ones keep their 10 s delays. Valve feedback: `delay_param` on a settling-time parameter (default 5 s, to be confirmed). FLOW_VALIDATION: `AL_LATCH`. DIGITAL_STOP and ANALYSER_NOT_OK: `AL_NO_ACK` if they stay status alarms. Descriptions: `text_param` on the existing description parameters (`PARAM_TEXT16`), which fixes K1/K2 by construction. Map `alarm_ack` onto the old coil 91 only if the master still needs it, as "acknowledge all" (write 65535).

## 7. How it was checked

| Check | Result |
|---|---|
| Alarm email end to end (`tests/integration/mail_test.py`): netsim against a mail server written in the test (plain, STARTTLS, TLS; certificates from openssl, signed with SHA-384) | 23 checks: test email to two recipients; an alarm emailed with its line and what is active, its clear emailed, two alarms in one email; a login refused on plain SMTP; STARTTLS with an RSA-2048 certificate and AUTH PLAIN; the fingerprint reported and pinned; a changed certificate refused before logging in, three tries, NOTIFY_FAIL; TLS with an ECDSA certificate, NOTIFY_FAIL cleared; the password unreadable and absent from the backup file; "nothing" sends nothing; sends and failures in the event log; TLS pool within its 44 KB (also run with RSA-4096: 38 KB) |
| Clock (`tests/test_clock.c`, `tests/test_ui.c`; `tests/integration/clock_test.py` against a test SNTP server) | Range refused; applied by the scan only, once; calendar and weekday (a leap day, a Sunday); through parameter 85 from Modbus in REMOTE; the logger told not to log the step twice; UTC offset. Touchscreen: Set clock, incomplete entry refused, set. End to end (14 checks): login and admin level needed, a set from the web, SNTP finds the clock an hour fast and steps it, no step when in step, an unsynchronised server ignored, no answer reported, following the server back 30 s, each step logged once with its source |
| Alarm manager (`make test`, `tests/test_alarm.c`) | Table errors; raise, acknowledge (through the parameter, any mode, no login), clear; cleared-but-unacknowledged kept; on- and off-delays with restarts; the delay parameter; latch held until acknowledged, or cleared later if acknowledged while active; status alarms; suppression at once and delays again after; range deadband on both sides and a value sitting on a limit raising once; UTF-16 descriptions; order and paging; acknowledge all; the watchdog-reset and IP-conflict platform alarms; routes: defaults, the user's choice and "nothing", relays in all three modes and fail-safe, overriding the application's output, the common-alarm word |
| Host unit tests (`make test`): scan order, first-scan bit, dt including 2^32 wrap and late scans, RTC snapshot and all 7 weekday conversions, frozen inputs, outputs default off, command order/limit/reject, seqlock retry and bounded failure, monitor grace/limits/wrap/re-enable, diag record across warm reset and after corruption; parameter definitions, every check, arbitration in both modes, interlocks, the remote-link watchdog; the store tests in section 4; the logger: calendar, averaging and quality, interval and schema changes, clock steps, wrap-around and even wear, FRAM pointer missing or stale, worn cells, failed erases, queue overflow, CSV and JSON export, and 400 random power cuts (during writes and erases, with the FRAM pointer current, stale or lost; also run with 20 seeds) after each of which every complete record is still there and in order | 13,926 checks, 0 failures, run with both memory maps, gcc -Werror with AddressSanitizer and UBSan |
| PC simulation (`make sim`): demo app on the parameter table and store, blank FRAM, writes from touchscreen/web/Modbus, power cycle | Modbus refused in LOCAL; web refused below min, then accepted in REMOTE; IP change refused over Modbus; setting and retained string survive the power cycle; mode back to LOCAL; calibration alarm stays on an uncalibrated unit |
| FreeRTOS POSIX run of the real task file (`make rtos`) | 280 scans in 2.8 s, ctrl 1 kHz, IWDG kicked every 100 ms; the real store task saves a queued web write; a UI task that hangs at 2 s gives a recorded "ui" fault, outputs to safe state, and a reset at 7.0 s |
| Firmware update end to end (`tests/integration/fw_test.py`): image from `tools/mkfwimage.py`, netsim with file-backed banks; a swap exits the process as a reset | 41 checks (incl. the stored update date and time, and the rollback in the history): login needed; 300 KB upload; damaged file and other-hardware image refused; activate, restart as the new version on trial, confirmed after the trial, events logged; second update "hangs" in its trial, watchdog reset rolls it back, the old version runs with `rolled_back`, the failed image stays available; with a product key (`CEMS_FW_PUBKEY`): unsigned and other-key images refused, the product-signed image accepted; gates: refused with the application's reason while a sequence runs, held for the touchscreen (refused, then confirmed and restarted), an image older than the minimum refused, every step logged |
| Touchscreen fonts (`tests/test_ui.c`, `t_fonts`) | All four fonts inflated and CRC-checked by the simulated co-processor (a zlib decoder written for the simulator); the metric block's layout; the CRC answer read back from the FIFO; a co-processor fault while loading leaves that size on the ROM font; a damaged stream detected; UTF-8 encoding (symbols, Greek mu, unknown characters, cut-short sequences, control characters); a unit with ° shown in the settings list |
| Touchscreen (`make test`, `tests/test_ui.c`): the EVE driver against a simulated BT817 that decodes the SPI framing and executes the command lists; the UI driven by simulated touches | Start-up sequence and panel registers; chip missing; co-processor fault recovered; command-buffer overflow caught. UI: every screen; settings refused without login with the reason; wrong then right password; out-of-range refused then accepted; text with capitals; arrays sent to the web; mode to REMOTE only after confirmation, touchscreen writes then refused, LOCAL taken back without login; IP entry with the network confirmation; ON/OFF; a PF_CONFIRM entry; events show the touchscreen as the source; firmware restart confirmed; lockout after five wrong passwords; slide-off cancels; idle logout, dimming and wake-only first touch; the alarm pill counts alarms and new ones, opens the list, one acknowledged then all, without a login; alarm actions visible to all but changed only by admin |
| Network integration (`make netsim` + `tests/integration/net_test.py`): real sockets, threads, file-backed FRAM, pymodbus 3.6.9 as the Modbus client, plain HTTP with JSON parsing | 88 checks: reads of every shape, mode refusals, range/half-value/read-only exceptions, text through registers, coils, web sessions and batch writes (all or nothing), client eviction, IP filter, 3 Modbus clients + 1 browser for 4 s with no errors, remote-link watchdog in real time, backup/restore and a damaged file refused; the event log records the mode change, Modbus and web writes with old/new value and login level, logins, the password change and the restore, and its CSV matches; after a restart: settings and the changed password kept, mode back to LOCAL, the event log continues with contiguous sequence numbers from the FRAM pointer; alarms: the platform's calibration alarm, a latching application alarm raised and held, the counts and per-alarm bits read over Modbus, acknowledged over Modbus (and logged as such), a range alarm waiting out its 2 s on-delay with its edited description, acknowledge refused without a login, acknowledge all, suppression, the Modbus common-alarm word following each alarm's Modbus action |
| Web page in headless Chromium (`tests/integration/web_ui_test.py`) | Admin firmware upload with progress, "Ready in the spare bank" and the restart button (`docs/web_firmware.png`); login, an out-of-range value refused with a plain message, an accepted change shown within a second and listed in the event log ("start_delay (1001): 2000 → 1500"), the connection-lost banner when the unit stops, no script errors; the alarm badge and list, one alarm acknowledged from its row; an alarm's relay action ticked, a relay given an output, the email set-up saved with the password left blank on reload |
| Target compile and link (`make target`, `make -f port/stm32h5/check.mk link`) with arm-none-eabi-gcc 13.3.1, FreeRTOS V11.2.0, STM32H5 HAL and CMSIS from ST's GitHub | Clean with -Werror (`tools/check_target.sh`: no FRAM/no NOR, I2C FRAM, SPI FRAM, and each FRAM with the SPI NOR), each linked with lwIP 2.2.1, the Ethernet driver, both servers and the logger. With everything fitted, flash and RAM by module are in `docs/MEMORY.md` (now 34 % of the 1 MB bank and 51 % of the RAM); the RAM stand-ins add 33 KB when no FRAM or NOR is fitted. `plat_diag` is in the NOLOAD `.noinit` section |
| I/O boards (`tests/test_iobus.c`, `tests/test_ui.c` `t_iob_screen`; `tests/integration/iob_test.py`) | RTU frames and CRC against the textbook frame, replies of every kind; the simulated node's rules (addresses, CRC, read-only and out-of-range writes, write-before-read, gate and hold time, unknown function); off by default with a quiet bus; connect, settings sent, outputs and relays out and inputs in, one request per board per cycle; channel ranges and main-board inputs kept outside them; stale scan outputs and safe state send all-off; a board lost (inputs held for 1 s, then 0 and all faulty, alarm, retried once a second) and back on its own; a node restart (settings again, watchdog latch cleared, latched alarm); open circuit only where checked, overload, over-temperature, bank B, field supply, wire break; wrong type never written to; overlapping channels, duplicate address, range past 64; lost and damaged replies counted only; the node holding through a 1.9 s silence, its hold running out and the master clearing it; no port. The touchscreen screen online and not responding. End to end over netsim (26 checks): enable from the console, inputs following the simulated sensor, open circuit, supply, power off and back, node watchdog, overlapping channels, alarms and event log, the web section in Chromium |
| Soak (`tests/integration/soak_test.py 15`) on the PC build | 15 minutes: no request failed; open sockets steady; memory flat after warm-up; no sanitizer errors or leaks |
| On the STM32H573I-DK | **Not done yet.** Needs the CubeIDE project (`docs/CUBEMX_SETUP.md`) |

## 8. Decisions (working defaults, change any of them)

| # | Decision | Proposal | Why it matters |
|---|---|---|---|
| D1 | Scan period | 10 ms, fixed period, measured `g_dt_ms` | The UniLogic scan time is unknown. 10 ms matches the Stage 4 tests and leaves the CPU mostly free |
| D2 | Fast control loop rate | 1 kHz task tick for now; a hardware timer ISR if any loop needs more | The spec allows 1 to 10 kHz; the loop list doesn't exist yet |
| D3 | Network stack | lwIP 2.2.1 from ST's `stm32h5-classic-coremw-apps`, added by hand | CubeMX doesn't offer lwIP for the H5 (ST recommends NetX Duo). The spec says lwIP; NetX Duo is ThreadX-based, and choosing it would mean changing the RTOS |
| D4 | Hard-fault handling | **Built (25 Oct):** HardFault, MemManage, BusFault and UsageFault go to `port/stm32h5/fault_stm32h5.c`: outputs to the safe state, the stacked PC and LR plus CFSR, HFSR and the faulting address into `plat_diag`, then a reset. The next boot shows it as e.g. "bus @0801A2C4" (kind and PC) on the System screen, the web page and in the event log's power-up entry; `/api/info` `last_fault` has `arg` (the PC), `lr`, `cfsr`, `hfsr`, `addr`. A task that runs past its stack is caught by the CPU's stack limit (PSPLIM, set per task by the CM33 port) and named instead, e.g. "stk scan" (no usable PC then). Stacks are sized generously (Gary, 25 Oct: overflows on earlier DK prototypes): task stacks 85 KB in all in `plat_config.h`, FreeRTOS heap 64 KB, main stack 8 KB and C heap 4 KB in the linker settings (`CUBEMX_SETUP.md` 1). CubeMX must not generate these handlers (`CUBEMX_SETUP.md` 4) | A crash in the field is otherwise a silent reset; the PC and LR point at the function, found in the build's map or with `addr2line` |
| D5 | Factory network default | Static 192.168.1.50/24, gateway 192.168.1.1, DHCP off | Always settable from the touchscreen, so a unit can be recovered locally |
| D6 | FRAM | **128 KB (MB85RC1MT, I2C, same driver) for the product (Gary, 20 Oct)**, now the build default (`PLAT_FRAM_SIZE`). The VM/MUX application's settings image is 13.4 KB of the 40 KB slot. The 32 KB development breakout (MB85RC256V) still fits the application: build with `PLAT_FRAM_SIZE=32768u`; the I2C driver reads the part's Device ID and refuses a part smaller than the build's map instead of wrapping onto the factory data. `make test` runs both maps | Room for the instrument family to grow; costs about 60 KB of RAM (the store's slot copy and the web restore buffer) |
| D7 | Modbus writes at first power-up | Enabled; any client IP | Plugs into a DAHS without setup; an admin can make Modbus read-only or limit it to one IP |
| D8 | Touchscreen writes in REMOTE mode | Refused, except the mode switch and `PF_ANY_MODE` entries | Stops the local panel and the PLC fighting over a setpoint; the spec only states the remote side |
| D9 | Default passwords | operator 1111, admin 2222, factory 9999; the page warns while any is unchanged; "restore defaults" also restores these | Change at commissioning. A per-unit factory password (e.g. from the serial) is an option if units leave the factory |
| D10 | MAC address | Locally administered, from the chip unique ID | Fine for site networks. For production, buy an IEEE OUI block or fit an EUI-48 chip (e.g. 24AA02E48) |
| D12 | Log full | Overwrite the oldest sector (4 KB); events and data in separate rings | A CEMS log that stops recording is worse than one that loses the oldest days; separate rings keep the audit trail from being pushed out by data |
| D13 | Log flash | 16 MB SPI NOR (W25Q128JV or equivalent JEDEC part) on its own chip select; the DK's OCTOSPI flash is not used | Cheap, and the logger needs nothing faster than plain SPI. Decide at schematic; any size from 64 KB works |
| D14 | Firmware authenticity | **Decided (Gary, 20 Oct): the signing key is a tech-level credential, never a user-level one.** The private key stays with the technicians/build machine and signs each .cfw; the instrument holds only the public key and rejects unsigned or wrongly signed images. Uploading and activating firmware also need the factory (tech) login, on the web and the touchscreen (Gary, 20 Oct). **Built:** ECDSA P-256 over header bytes 0..83 (which include the payload's SHA-256). A build whose `platform/core/fw_pubkey.c` holds a key (made by `tools/fwkey.py pubkey`) refuses anything not signed by it; the repository's copy has no key, so development builds accept unsigned images and say so on the System screen and web page. Signing a production build is: keep the PEM off the repository, generate `fw_pubkey.c`, build, `mkfwimage.py --key` | Site users (operator, admin) can't change firmware; and even a tech login can't make firmware the unit will accept without the signing key |
| D15 | Trial length and rollback rule | 60 s healthy running; a watchdog/fault reset or a second power cut during the trial rolls back | Long enough to cover start-up and the first scans and connections, short enough that a field engineer sees the result |
| D16 | UI library | Own plain-C layer on the EVE co-processor commands (built-in widgets and ROM fonts), not LVGL | The spec asked for a prototype of each. The co-processor's buttons, keypads and text cover menus and setpoint entry, need no frame buffer or image assets, and keep the UI at about 20 KB of code and 10 KB of RAM. LVGL's EVE renderer is still possible behind the same task if richer graphics are wanted |
| D11 | Web transport | **HTTPS only (Gary, 20 Oct); built.** TLS 1.2, ECDHE-ECDSA-AES128-GCM-SHA256 on P-256, mbedTLS 3.6.7 LTS (vendored subset, `third_party/mbedtls`) in a fixed 44 KB pool (shared with the alarm-email client since D20). Session tickets, so a browser's repeat connections skip the public-key work. Port 80 only redirects to https:// (parameter 6 `http_redirect` closes it). **No production certificate step (Gary, 20 Oct):** each unit makes its own key at first start (kept in FRAM META, never in a backup file) and a self-signed certificate from it (stable: deterministic signing), with its IP address in it. The browser warns once per unit; the fingerprint is on the touchscreen and the web page so a technician can check it is the real unit, and the certificate can be downloaded and trusted on the site laptops to stop the warning. Cost: 70 KB flash, 39 KB RAM | Industrial sites: passwords and settings must not cross the network in clear. A company CA at production would only remove the one-time warning, at the cost of a production step and a key to guard |
| D17 | Remote access | Through a site VPN router (outbound VPN, vendor remote-access service); never port-forwarding; the instrument never connects out, except to the site's own mail server when alarm email is switched on (D20) | Keeps the instrument off the open internet; nothing to build or host; works the same for Modbus diagnostics |
| D19 | Alarm acknowledge and power-up | **Agreed (Gary, 20 Oct).** Acknowledging needs no login on the touchscreen (the operator at the instrument, as on the old panel); the web needs a login like every web write; Modbus follows `modbus_write_en`. Alarm states are not kept over a power cut: at power-up every condition is evaluated afresh (with its delays), and the event log keeps the history | An acknowledge changes nothing in the process, so a login would only slow the operator down. Keeping latched, unacknowledged alarms across a power cut is 32 bytes of FRAM if wanted |
| D20 | Alarm actions | **Gary, 20 Oct: configurable per alarm by the user.** Email, Modbus common alarm, up to 4 relay outputs, or nothing; application defaults, user overrides persisted; email to the site mail server only, login over TLS only, server pinned by certificate fingerprint, password write-only | Sites differ: some have a PLC that polls, some a beacon and horn, some a mail relay, some none of these |
| D21 | Secrets in backup files | Password hashes and the mail password are never written to a parameter backup file, and a restore never changes them | A backup file gets copied around; with 4-digit PINs the hashes could be brute-forced from it. Restoring settings onto a unit should not change who can log in |
| D22 | Clock | Local standard time, no daylight saving; UTC offset as a setting; set from the touchscreen and web (admin), the PLC (parameter 85) or SNTP from the site's time server; every change logged once with its source | CEMS records must not jump twice a year; sites seldom have internet, so the time source is the operator, the DAHS or a site server |
| D23 | Firmware restart gates | The application's safe-state check (always on), touchscreen confirmation of web-requested restarts (setting, off by default), minimum version (setting, factory) | The spec's three conditions; confirmation is off by default so a site without anyone at the panel can still be updated |
| D24 | Display assets | Own fonts (DejaVu, L4) compressed in the firmware, inflated into RAM_G at start-up and CRC-checked; nothing in the module's flash yet; link stays single SPI | Versioned with the firmware with no extra step; the units the instrument shows need ° ² ³ µ; the module's flash is kept for big images if ever needed |
| D26 | Application screens and records | The application describes its screens and record editors by parameter ID (`ui_app.h`); the platform draws them on the touchscreen and the web page the same way, and every press is a write through the table. Sequences and schedules are records: whole C structures, written only whole and applied in one scan after the application's check of the complete new image (`PARAM_RECORD`) | One description serves both screens; no half-edited sequence can reach the logic; the old 7-row editor bits and copies are not needed |
| D25 | Serial number authority | **Gary, 20 Oct: developer level authority to change it at all.** No login level can write it; only a token signed by a separate developer key, bound to the chip's unique ID and the current serial (single use, single unit); not restored from backups; every change and refusal logged; no key built in = cannot be changed | The serial is the unit's identity for the customer, certificates and records; a tech login or a copied backup must not be able to change or clone it |
| D27 | Field I/O | **Gary, 22 Oct:** valves on 24 VDC (SMC VX21 class) switched by isolated solid-state outputs, not relays; the I/O on separate boards linked by RS-485 Modbus RTU (deployable up to 100 m away); per board 8 × 0.6 A with diagnostics, 8 × 1 A, 2 socketed relays for 230 VAC, 8 inputs. A board holds its outputs for 5 min (setting) when the link goes quiet. Built: the master here; the boards follow the *I/O Board Design Specification* and the *I/O Module Requirements* | Sockets and relay contacts were the reliability worry; per-channel diagnostics tell a technician which valve is open-circuit; the bus keeps the main board small and the I/O near the valves **Revised (Gary, 24 Oct):** the boards move to the STM32H563 (the H5 is the standard for all development) with a LAN8742A, and **Ethernet with Modbus TCP replaces the RS-485 pair**; each board's IP is set from the HTTPS page like its address (5k). RS-485 stays as a setting (`iob_link`) |
| D18 | Memory record | `docs/MEMORY.md`, generated from the linker map by `tools/mem_report.py` (run by `tools/check_target.sh`) after every module: flash and RAM by module, totals against the H573's limits, and what is still to come | The hardware choice depends on it (Gary, 20 Oct: we are getting closer to the limits) |

## 9. Repository layout

```
platform/include/   public headers (no HAL, no FreeRTOS except plat_port.h on target)
platform/core/      portable logic: scan engine, queue, monitor, diag
platform/os/        FreeRTOS binding (tasks, hooks)
platform/net/       Modbus TCP, HTTP server, web API, logins (socket API: POSIX or lwIP)
platform/bsp/       simulated board, RAM FRAM and RAM NOR (also the target's stand-ins), simulated I/O board
port/stm32h5/       STM32H5 BSP, FRAM and NOR drivers, FreeRTOSConfig.h, linker snippet, compile/link check
port/stm32h5/lwip/  lwipopts.h, Ethernet driver (from ST, SLA0044), network bring-up
platform/ui/         EVE display driver and the touch UI
web/                the web page source (embedded by tools/embed_web.py)
app_demo/           example application on the scan contract
host/               PC simulation, FreeRTOS POSIX run, networked simulation (netsim), BT817 simulator
tools/              web page embedding, firmware image tool, screen previews, target compile check
tests/              host unit tests; tests/integration: network, browser and I/O bus tests
docs/               this file, CUBEMX_SETUP.md
```
