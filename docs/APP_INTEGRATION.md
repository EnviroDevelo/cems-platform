# Handover: building the VM/MUX application on the CEMS core platform

For the team translating the old Unitronics Valve Manager / Multiplexor program
(UniLogic) into C. This file says what the core platform already does, what the
application has to supply, and the rules for joining the two. The core platform is
maintained in a separate thread. Ask for changes to it there; don't edit
`platform/` from the application side.

Revision: 20 Oct 2026. Platform commit `f2aa6cf` and later.
Repository: `CEMS Supervisor/Firmware/cems-platform` (git).
Analysis: `CEMS Supervisor/Existing Design/Analysis/` (00_Breakdown_Plan.md, 10_IO_Map.md,
11_UniLogic_Semantics.md, the Stage 4 files ...).

Read with this file:
- `docs/PLATFORM.md` sections 3 (scan contract), 4 (parameters), 5e (alarms) and 6 (integration rules).
- `app_demo/app_demo.c`: a complete, working, 140-line example application that uses every hook.

---

## 1. Target and environment (fixed)

| Item | Value |
|---|---|
| MCU | STM32H573 (Cortex-M33, 250 MHz, 2 MB flash dual-bank, 640 KB RAM). Bench: NUCLEO-H563ZI |
| Display | Riverdi EVE4 7" BT817, 1024x600, capacitive touch (old panel was 800x480) |
| RTOS / stack | FreeRTOS, lwIP, mbedTLS (HTTPS) |
| Toolchain | STM32CubeIDE, plain C11, no vendor GUI tools |
| Non-volatile | FRAM for settings and retained data (32 KB part for development; 128 KB if needed); SPI NOR for the event and data logs |
| Host build | `make test` (unit tests, gcc with ASan and UBSan, `-Werror -Wconversion`), `make sim`, `make netsim` (real sockets on the PC) |

Application code must build warning-free with the platform's flags:
`-std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror`.

## 2. What the platform already provides. Do not re-implement any of it

| Old program feature | Platform equivalent |
|---|---|
| PLC scan cycle, `General.Ladder_Initial_Cycle`, RTC tags | Scan engine: 10 ms scan; `g_dt_ms`, `Ladder_Initial_Cycle`, `RTC_Date`, `RTC_Time` in `ul_system.h` |
| Retained tags, settings, "restore defaults" | Parameter table plus FRAM store (two CRC'd copies, power-cut safe, migration on layout change) |
| Password protection screens | Logins: operator / admin / factory (tech), each parameter has a write level. Default passwords 1111 / 2222 / 9999 |
| Modbus slave | Modbus TCP server on port 502. The application only supplies the register map (`mb_map_t`) |
| Alarm bits, panel alarm table, acknowledge | Alarm manager (`plat_alarm.h`): delays, latch, acknowledge, history, colours, touchscreen and web lists |
| Email settings and alarm emails | SMTP client (STARTTLS/TLS), configured per alarm by the user (D20). **Remove the old email logic** |
| LOG / data tables | Event log and data logger on NOR, CSV download from the web page |
| HMI generic screens (settings list, edit, keypad, events, alarms, system) | Touch UI built from the parameter table (`ui.c`) |
| Clock setting | Touchscreen, web, Modbus parameter 85, or SNTP. Local standard time, no daylight saving (D22) |
| Firmware update | Signed images over HTTPS, dual-bank with rollback, update history |
| Local/remote control | Parameter 1 (touchscreen only), comms watchdog, `param_remote_link_lost()` |
| Unit identity | Serial number (developer-signed only, D25), chip ID, MAC |

## 3. What the application supplies

One `plat_app_t`, passed to `plat_start(&app)` from `main()`. See `platform/include/plat_tasks.h`; `app_demo.c` fills every field.

```c
const plat_app_t app = {
    .scan       = &scan,          /* scan_app_t: init, io_in, apply_cmd, logic (Function1), io_out, publish */
    .params     = params,  .n_params = N,      /* param_def_t, IDs 1000..65535 */
    .retain     = retain,  .n_retain = N,      /* RETAIN() blocks: retained tags not in the table */
    .layout_version = 1, .migrate = NULL,      /* bump the version when a stored record changes meaning */
    .modbus_map = map,     .n_modbus_map = N,  /* mb_map_t */
    .ident      = { .vendor, .product, .revision = "x.y.z", .write_level },
    .log_chans  = chans,   .n_log_chans = N,   /* data-log columns */
    .ui_home_ids = ids,    .n_ui_home = N,     /* up to 6 tiles on the home screen */
    .alarms     = alarms,  .n_alarms = N,      /* alarm_def_t, IDs 100..255 */
    .fw_safe    = fw_safe,                     /* false + reason while a sequence/calibration runs */
};
```

### The scan (`scan_app_t`), one call per 10 ms

The order is fixed by the platform (PLATFORM.md section 3):

1. Clock snapshot and `g_dt_ms` (measured, 0 on the first scan).
2. `io_in()`: copy the frozen `plat_io_in_t` into your input tags (`TA30_DI[]`, `UID_DI[]`, ...).
3. External writes are applied between scans, never halfway through.
4. `logic()`: `Function1`. `Ladder_Initial_Cycle` is true on the first scan only.
5. `io_out()`: the output image starts **all off** each scan; set what you drive.
6. `publish()`: copy display values into your snapshot (inside a seqlock).

Timers (TON/TP/TA) are modelled in the application from `g_dt_ms`. `ul_timer_t` comes from `ul_system.h` (Preset/Current in ms, counting down).

### I/O image (`plat_io.h`)
- `di` (32 bits), `do_bits` (32 bits).
- `ai_raw[8]` with `ai_status[]` (`PLAT_AIN_OK/UNDER/OVER/FAULT/NOT_FITTED`).
- `rtd_cdeg[4]` (0.01 °C) with status.
- `ao_raw[4]`.

Channel numbering follows `10_IO_Map.md`. The analog and RTD front end is not chosen yet, so those channels report `NOT_FITTED` on the bench. The application must treat that like a sensor fault, not as a value of 0.

## 4. Integration rules (from reading the Stage 4 sources; PLATFORM.md section 6)

**Must fix before the chunks will link:**
1. **One definition of the system tags.** `g_dt_ms` is defined in 4G, 4H and 4I, and 4H also defines `Ladder_Initial_Cycle` and `RTC_Date`. On the platform, `scan_engine.c` owns all four. Faithful files `#include "ul_system.h"` and only declare them. Test files may keep their own definitions.
2. **`rtc_date_t` layout.** 4F2.h declares `{Year, Month, Day_In_Month, Day_Of_Week}`, while 4E/4H declare `{Day_Of_Week, Day_In_Month, Month, Year}`. Use only the typedef in `ul_system.h`; members are accessed by name. `Day_Of_Week` is 1 = Sunday (UniLogic convention; the platform converts).
3. `ul_timer_t`: remove the per-chunk copies and use `ul_system.h`.

**Rules:**
- No BSP, HAL or FreeRTOS calls from the application. Everything comes through `plat_app_t`, `ul_system.h`, `param.h`, `plat_alarm.h` and `plat_log.h`.
- Application tags stay plain globals (as in Stage 4). The platform never touches them except through the table entries you declare.
- `Touched_Bit` / `VNC_Touched_Bit` (4F4): define them in the application for now. The new UI doesn't need them. If the logic uses them to mean "someone is at the panel", ask the core thread for a UI-activity flag.
- Act on `param_store_calibration_valid()` (false: block measurement) and `param_remote_link_lost()` (go to the application's safe state).
- `fw_safe()` must refuse a firmware restart while a sequence, calibration or purge is running, and give the reason in words.

## 5. Parameter table (`param.h`)

- Generate it from `stage2/data_dictionary.csv`: the 148 SETTING tags become `param_def_t` entries (IDs from 1000), and other retained tags become `RETAIN()` blocks.
- Ranges come from the HMI element limits (Stage 5 `hmi_elements.csv`). **A limit that isn't known is a Rule 0 question, not a guess.**
- Macros:
  - Scalars: `PARAM_INT`, `PARAM_UINT`, `PARAM_REAL`, `PARAM_BIT`, `PARAM_BIT_D`.
  - Text: `PARAM_TEXT` (8-bit), `PARAM_TEXT16` (UTF-16, like the old panel strings), `PARAM_BYTES` (BUFFER).
  - Arrays: `PARAM_ARRAY_INT/UINT/REAL/BOOL`.
- Flags:
  - `PF_PERSIST`: in FRAM.
  - `PF_RO`: status, never written from outside.
  - `PF_FACTORY`: calibration; separate image, survives "restore defaults".
  - `PF_ANY_MODE`: writable in local and remote mode.
  - `PF_LOCAL_ONLY`, `PF_NO_MODBUS`.
  - `PF_CONFIRM`: the touchscreen asks "are you sure?"; use for critical setpoints.
  - `PF_SECRET`: write-only.
- Every write from any source goes through the same checks: ID, writable, level, local/remote mode, type, range, then your optional `interlock` function at apply time.
- Don't make tags the logic rewrites every scan `PF_PERSIST` unless the original retains them.
- **FRAM budget (tightest store):** user image 14.5 KB per copy on the 32 KB part. The analysis estimates 12.5 KB of retained data. The platform uses about 0.8 KB, so about 92 % is used. The real figure decides the FRAM part (32 KB or 128 KB). **Report it to the core thread as soon as the table compiles.**
- IDs 1..999 belong to the platform. Do not write parameter 50 (serial). It is refused anyway.

## 6. Alarms (`plat_alarm.h`; PLATFORM.md section 5e)

- One `alarm_def_t` per distinct `new_id` in `70_alarm_map.csv`, IDs 100..255. The table holds 96 alarms in total; the platform uses about 10.
- The logic only reports conditions: `alarm_set(id, cond)` or `alarm_set_range(id, value, lo, hi, deadband)`. The platform owns delays, latch, acknowledge and state.
- **Agreed with Gary (D19):**
  - Acknowledging needs no login on the touchscreen.
  - Alarm states are re-evaluated at power-up (not restored).
  - Valve-feedback settling time defaults to **5 s**, via `delay_param` on a settling-time parameter.
- Mapping:
  - FLOW_VALIDATION → `AL_LATCH`.
  - DIGITAL_STOP and ANALYSER_NOT_OK → `AL_NO_ACK` (status alarms).
  - Analog range alarms → `alarm_set_range` with their 10 s delays and an `enable_param`.
  - Editable descriptions → `text_param` on the existing description parameters (`PARAM_TEXT16`). This fixes the old K1/K2 text drift by construction.
- **Actions are user-configurable (D20, Gary):** each alarm can email, count in the Modbus common alarm, drive one of 4 relay outputs, or do nothing. The application gives only a sensible default in `.route` (`AR_EMAIL | AR_MODBUS | AR_RELAY(k)`). It must not drive alarm relays itself.
- The old source arrays (`AlarmsVM`, `AlarmsMUX`, `FBAlarmsMUX`) may stay for the old Modbus coils, but **nothing clears them on acknowledge any more**. Acknowledge is `PARAM_ID_ALARM_ACK` (write 65535 = all). Map the old coil 91 onto it only if the master still needs it.
- Platform alarm IDs 2, 3 and 10 cover the old store, log and email faults; don't duplicate them.

## 7. Modbus map (`modbus.h`)

- Generate `mb_map_t` from `stage4/4G/modbus_map.csv`:
  - `MB_ENTRY(table, addr, param_id, format)`.
  - Formats: `MBF_BIT`, `MBF_U16/I16/U32/I32/F32`, `MBF_BYTES`, `MBF_ARRAY`.
  - `MB_ENTRY_X` adds flags and a decimal scale for floats shown as integers.
- **Open Rule 0 item:**
  - We don't know the byte order UniLogic packs BUFFER bytes into registers in, or the word order of its 32-bit values.
  - The map supports both, one flag per entry (`MBF_BYTE_LO_FIRST`, `MBF_WORD_LO_FIRST`).
  - Settle it with a UniLogic screenshot of the slave data map, or one register read from a running unit.
- Platform registers the master may need are in the demo map: alarm acknowledge, alarm counts, the common alarm word, and per-alarm active and unacknowledged bit arrays.

## 8. Data logging (`plat_log.h`)

- The old LOG columns become `plat_log_chan_t` entries (name ≤ 16 characters, unit ≤ 6, decimals).
- Call `plat_log_put(channel, value, quality)` **once per scan per channel** with the real reading. Quality is one of:
  - `LQ_OK`.
  - `LQ_FLAG`: in calibration or purge, or one of its alarms is active (`alarm_active(id)`).
  - `LQ_BAD`: sensor fault.
  - `LQ_NA`: not measured.
- Never log a last-held value. Averaging over the log interval (parameter 30) is done by the platform.
- Application events: `plat_log_event(code >= 100, ...)`.

## 9. Touchscreen and web screens (`ui_app.h`, D26; PLATFORM.md section 5j)

- The platform UI is generic: home tiles (`ui_home_ids`), settings list and edit from the parameter table, alarms, events, clock, system. It renders `PARAM_TEXT16` and units with ° ² ³ µ.
- **Application screens:** describe them in `plat_app_t.ui` (`ui_app_t`), entirely by parameter ID. The platform draws them on the touchscreen (as tabs) and on the web page.
  - Widgets: `UW_SECTION`, `UW_VALUE` (scalar or array element), `UW_BUTTON` (command / state / enable), `UW_GRID` (numbered items: count, visible, enable and lit arrays, names), `UW_DIALOG` (shown while a status parameter is set; Yes/No or OK commands) and `UW_RECORD`.
  - Any widget can carry `show_id` and `show_min`.
  - A button writes `cmd_value`, which defaults to 1; use `UW_WRITE_ZERO` to write 0.
- **Sequences and schedules are records:** `PARAM_RECORD(id, name, Var, check_fn, level, flags)` replaces the `RETAIN()` block, keeping the same ID.
  - They are written only whole and applied in one scan after `check_fn` accepts the complete new image. The reason the check gives is shown to the operator.
  - Describe the fields in a `ui_record_t` on `ui_app_t.records`:
    - table records: rows, plus the offset of the "in use" count;
    - form records: fields, with tick-box arrays.
  - The editors work on a copy and Save it whole.
  - Not reachable over Modbus.
- **Worked example:** `app_demo/app_demo.c` (the demo steps record and the Demo screen) and `tests/test_ui_app.c`, which uses a VM-shaped sequence and schedule.

## 10. Decisions already made that affect the application

| # | Decision |
|---|---|
| D1 | Scan period 10 ms |
| D6 | FRAM size to be chosen once the application's retained size is known (32 KB part on the bench) |
| D16 | UI is a plain-C layer on the EVE co-processor, not LVGL |
| D19 | Ack without login on the touchscreen; alarms re-evaluated on power-up; 5 s valve settle |
| D20 | Alarm actions configurable per alarm by the user (email / Modbus / relay / nothing) |
| D21 | Passwords and the mail password are never in backup files |
| D22 | Clock is local standard time (no daylight saving); `RTC_Date`/`RTC_Time` are that time |
| D23 | Firmware restarts need `fw_safe()`; optional touchscreen confirmation; minimum version |
| D25 | Serial number is changed only with a developer-signed token |
| D26 | Application screens and record editors are described by parameter ID; sequences and schedules are whole records with an application check |

Full list: PLATFORM.md section 8.

## 10a. Linking the application into the platform's builds (R1, R5)

- Every application defines `const plat_app_t *app_select(void)`, declared in `plat_tasks.h`.
- `app_vmmux/app_vars.mk` sets `APP_SRC`, `APP_INC` and `APP_HDRS`.
- With those in place:
  - `make netsim APP=app_vmmux` builds `build/netsim-app_vmmux`. The `-Dapp_demo_plat` define is no longer needed.
  - `make target APP=app_vmmux` compiles the application for the target.
  - `tools/check_target.sh` links it and adds its totals to `docs/MEMORY.md` automatically.
- The FRAM map is 128 KB by default (D6). Build with `PLAT_FRAM_SIZE=32768u` for the 32 KB breakout.

## 11. Suggested order of work

1. Fix rules 4.1-4.3 so the Stage 4 chunks compile together against `ul_system.h`.
2. Generate the parameter table and `RETAIN()` blocks; build with `make test`; **report the FRAM size**.
3. `scan_app_t` around `Function1` with `io_in`/`io_out` per `10_IO_Map.md`; run on `make sim` and `make netsim`.
4. Alarm definitions, Modbus map, log channels, home tiles, `fw_safe`.
5. Host tests for each chunk in the same style as `tests/` (the `tst.h` CHECK macros). Integration with `tests/integration/*` (netsim) where comms are involved.
6. Send the screen list (section 9) and any platform change requests to the core thread.

## 12. Questions still open for Gary (Rule 0, not for the platform to guess)

- Modbus BUFFER byte order and 32-bit word order (section 7).
- RealLinear pin order for the analog scaling (10_IO_Map.md section 4, update 18 Oct).
- Analog and RTD front end (input type, ranges): a schematic decision.
- HMI limits that aren't in the export.
