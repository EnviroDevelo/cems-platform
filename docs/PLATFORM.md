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
| Firmware update over the network: streamed upload into the free flash bank, bank swap, trial boot, automatic confirm or rollback; image tool | `fw_update.h/.c`, `fw_flash_sim.c`, `port/stm32h5/fw_flash_stm32h5.c`, `tools/mkfwimage.py` | Done; host-tested, end to end on real sockets (upload, swap, trial, confirm, crash, rollback) and in Chromium. Flash driver compiled and linked, not yet run on hardware |
| EVE BT817 display driver (Riverdi RVT70 timing, co-processor commands, touch tags, fault recovery) and the touch UI (home, settings, edit, log in, events, system) | `eve.h`, `platform/ui/eve.c`, `ui.h`, `platform/ui/ui.c`, `platform/os/plat_ui_task.c`, `port/stm32h5/eve_bus_stm32h5.c`; `host/eve_sim/`, `tools/eve_preview.py` | Done; host-tested against a simulated BT817 driven by simulated touches; screens rendered from the real command lists (`docs/ui/`). Not yet run on the display |
| Firmware image signing (D14): ECDSA P-256 signature over the image header (which holds the payload's SHA-256); key tool | `platform/net/fw_sig.c`, `platform/core/fw_pubkey.c` (generated), `tools/fwkey.py`, `tools/mkfwimage.py --key` | Done; host-tested (keys made in the test: product key accepted; unsigned, other-key, edited-header and edited-payload images refused) and end to end through netsim with a key made by `fwkey.py`. Compiled and linked for the H5 |
| HTTPS (D11): own key and certificate, TLS 1.2 server, session tickets, port-80 redirect | `tls.h`, `platform/net/tls.c`, `tls_config.h`, `third_party/mbedtls` | Done; host-tested, handshakes against Python's TLS client (resumption, certificate trust by IP, redirect) and Chromium. Compiled and linked for the H5 |
| Health: heap and every task's stack margin sampled every 5 s (web, System screen), alarm 11 RESOURCE_LOW; soak test | `plat_health.h`, `plat_tasks_freertos.c` (sampling), `tests/integration/soak_test.py` | Done; host-tested; soak run for 15 min on the PC build (no failures, sockets and memory flat) |
| Own touchscreen fonts (D24): DejaVu Sans at the ROM font sizes with ° ² ³ µ ± and 26 more symbols, kept compressed in the firmware, inflated into the display's RAM at start-up and CRC-checked; UTF-8 text encoded for them; ROM fonts if loading fails | `tools/mkfont.py`, `platform/ui/eve_fonts.c` (generated), `eve.h` (`eve_fonts_load`, `eve_font_encode`), `ui.c`; simulator: `host/eve_sim/zinflate.c` | Done; host-tested against the simulated BT817 (inflate, CRC read back from the FIFO, a fault while loading, a damaged stream) and glyph sheets checked by eye. Not yet run on the display |
| Clock: set from the touchscreen, the web page, the PLC (parameter 85) or the site's SNTP server; every change logged with its source; local standard time with a UTC offset | `plat_clock.h`, `platform/core/plat_clock.c`, `platform/net/sntp.c`, `bsp_rtc_write`; System > Set clock; Clock on the web page | Done; host-tested and end to end against a test time server |
| Alarm actions and email: per-alarm email / Modbus common alarm / 4 relay outputs; SMTP client (STARTTLS, TLS, plain) with certificate pinning, batching, retries, NOTIFY_FAIL; write-only secrets kept out of backups | `plat_alarm.h` (routes, relays), `mail.h`, `platform/net/mail.c`, `tls.c` (client); Alarm actions on the web page and the touchscreen | Done; host-tested and end to end against a test mail server (plain, STARTTLS with RSA, TLS with ECDSA, pinning, a changed certificate, retries), in Chromium and on the simulated touchscreen |
| Alarm manager: one table of alarm definitions, on/off delays, latch, status alarms, acknowledge one or all from the touchscreen, web and Modbus, suppression, range alarms with deadband, editable descriptions, platform alarms, event-log entries | `plat_alarm.h`, `platform/core/plat_alarm.c`; Alarms screen in `ui.c`; `/api/alarms` | Done; host-tested (every state change, delays, latch, deadband, ordering, paging), on real sockets with Modbus and HTTPS, and in Chromium |
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

**Keep-alive.** The server handles one HTTPS connection at a time. After a GET the
connection stays open (HTTP/1.1 keep-alive; bodies without a known length are sent
chunked), so the page's polls reuse it instead of opening a TLS connection each time.
It gives way at once when another connection is waiting, so no browser can hold the
server; it also closes after 5 s idle, after 200 requests, after any POST (browsers do
not retry a POST on a connection the server closed) and before an alarm email borrows
the TLS context. Measured with Chromium and the page open: about 0.7 new TLS
connections a second (all resumed by ticket) instead of one per request (3-4 a second).

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
| Host unit tests (`make test`): scan order, first-scan bit, dt including 2^32 wrap and late scans, RTC snapshot and all 7 weekday conversions, frozen inputs, outputs default off, command order/limit/reject, seqlock retry and bounded failure, monitor grace/limits/wrap/re-enable, diag record across warm reset and after corruption; parameter definitions, every check, arbitration in both modes, interlocks, the remote-link watchdog; the store tests in section 4; the logger: calendar, averaging and quality, interval and schema changes, clock steps, wrap-around and even wear, FRAM pointer missing or stale, worn cells, failed erases, queue overflow, CSV and JSON export, and 400 random power cuts (during writes and erases, with the FRAM pointer current, stale or lost; also run with 20 seeds) after each of which every complete record is still there and in order | 12,494 checks, 0 failures, run with both memory maps, gcc -Werror with AddressSanitizer and UBSan |
| PC simulation (`make sim`): demo app on the parameter table and store, blank FRAM, writes from touchscreen/web/Modbus, power cycle | Modbus refused in LOCAL; web refused below min, then accepted in REMOTE; IP change refused over Modbus; setting and retained string survive the power cycle; mode back to LOCAL; calibration alarm stays on an uncalibrated unit |
| FreeRTOS POSIX run of the real task file (`make rtos`) | 280 scans in 2.8 s, ctrl 1 kHz, IWDG kicked every 100 ms; the real store task saves a queued web write; a UI task that hangs at 2 s gives a recorded "ui" fault, outputs to safe state, and a reset at 7.0 s |
| Firmware update end to end (`tests/integration/fw_test.py`): image from `tools/mkfwimage.py`, netsim with file-backed banks; a swap exits the process as a reset | 39 checks: login needed; 300 KB upload; damaged file and other-hardware image refused; activate, restart as the new version on trial, confirmed after the trial, events logged; second update "hangs" in its trial, watchdog reset rolls it back, the old version runs with `rolled_back`, the failed image stays available; with a product key (`CEMS_FW_PUBKEY`): unsigned and other-key images refused, the product-signed image accepted; gates: refused with the application's reason while a sequence runs, held for the touchscreen (refused, then confirmed and restarted), an image older than the minimum refused, every step logged |
| Touchscreen fonts (`tests/test_ui.c`, `t_fonts`) | All four fonts inflated and CRC-checked by the simulated co-processor (a zlib decoder written for the simulator); the metric block's layout; the CRC answer read back from the FIFO; a co-processor fault while loading leaves that size on the ROM font; a damaged stream detected; UTF-8 encoding (symbols, Greek mu, unknown characters, cut-short sequences, control characters); a unit with ° shown in the settings list |
| Touchscreen (`make test`, `tests/test_ui.c`): the EVE driver against a simulated BT817 that decodes the SPI framing and executes the command lists; the UI driven by simulated touches | Start-up sequence and panel registers; chip missing; co-processor fault recovered; command-buffer overflow caught. UI: every screen; settings refused without login with the reason; wrong then right password; out-of-range refused then accepted; text with capitals; arrays sent to the web; mode to REMOTE only after confirmation, touchscreen writes then refused, LOCAL taken back without login; IP entry with the network confirmation; ON/OFF; a PF_CONFIRM entry; events show the touchscreen as the source; firmware restart confirmed; lockout after five wrong passwords; slide-off cancels; idle logout, dimming and wake-only first touch; the alarm pill counts alarms and new ones, opens the list, one acknowledged then all, without a login; alarm actions visible to all but changed only by admin |
| Network integration (`make netsim` + `tests/integration/net_test.py`): real sockets, threads, file-backed FRAM, pymodbus 3.6.9 as the Modbus client, plain HTTP with JSON parsing | 88 checks: reads of every shape, mode refusals, range/half-value/read-only exceptions, text through registers, coils, web sessions and batch writes (all or nothing), client eviction, IP filter, 3 Modbus clients + 1 browser for 4 s with no errors, remote-link watchdog in real time, backup/restore and a damaged file refused; the event log records the mode change, Modbus and web writes with old/new value and login level, logins, the password change and the restore, and its CSV matches; after a restart: settings and the changed password kept, mode back to LOCAL, the event log continues with contiguous sequence numbers from the FRAM pointer; alarms: the platform's calibration alarm, a latching application alarm raised and held, the counts and per-alarm bits read over Modbus, acknowledged over Modbus (and logged as such), a range alarm waiting out its 2 s on-delay with its edited description, acknowledge refused without a login, acknowledge all, suppression, the Modbus common-alarm word following each alarm's Modbus action |
| Web page in headless Chromium (`tests/integration/web_ui_test.py`) | Admin firmware upload with progress, "Ready in the spare bank" and the restart button (`docs/web_firmware.png`); login, an out-of-range value refused with a plain message, an accepted change shown within a second and listed in the event log ("start_delay (1001): 2000 → 1500"), the connection-lost banner when the unit stops, no script errors; the alarm badge and list, one alarm acknowledged from its row; an alarm's relay action ticked, a relay given an output, the email set-up saved with the password left blank on reload |
| Target compile and link (`make target`, `make -f port/stm32h5/check.mk link`) with arm-none-eabi-gcc 13.3.1, FreeRTOS V11.2.0, STM32H5 HAL and CMSIS from ST's GitHub | Clean with -Werror (`tools/check_target.sh`: no FRAM/no NOR, I2C FRAM, SPI FRAM, and each FRAM with the SPI NOR), each linked with lwIP 2.2.1, the Ethernet driver, both servers and the logger. With everything fitted, flash and RAM by module are in `docs/MEMORY.md` (now 34 % of the 1 MB bank and 51 % of the RAM); the RAM stand-ins add 33 KB when no FRAM or NOR is fitted. `plat_diag` is in the NOLOAD `.noinit` section |
| Soak (`tests/integration/soak_test.py 15`) on the PC build | 15 minutes: no request failed; open sockets steady; memory flat after warm-up; no sanitizer errors or leaks |
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
| D18 | Memory record | `docs/MEMORY.md`, generated from the linker map by `tools/mem_report.py` (run by `tools/check_target.sh`) after every module: flash and RAM by module, totals against the H573's limits, and what is still to come | The hardware choice depends on it (Gary, 20 Oct: we are getting closer to the limits) |

## 9. Repository layout

```
platform/include/   public headers (no HAL, no FreeRTOS except plat_port.h on target)
platform/core/      portable logic: scan engine, queue, monitor, diag
platform/os/        FreeRTOS binding (tasks, hooks)
platform/net/       Modbus TCP, HTTP server, web API, logins (socket API: POSIX or lwIP)
platform/bsp/       simulated board, RAM FRAM and RAM NOR (also the target's stand-ins)
port/stm32h5/       STM32H5 BSP, FRAM and NOR drivers, FreeRTOSConfig.h, linker snippet, compile/link check
port/stm32h5/lwip/  lwipopts.h, Ethernet driver (from ST, SLA0044), network bring-up
platform/ui/         EVE display driver and the touch UI
web/                the web page source (embedded by tools/embed_web.py)
app_demo/           example application on the scan contract
host/               PC simulation, FreeRTOS POSIX run, networked simulation (netsim), BT817 simulator
tools/              web page embedding, firmware image tool, screen previews, target compile check
tests/              host unit tests; tests/integration: network and browser tests
docs/               this file, CUBEMX_SETUP.md
```
