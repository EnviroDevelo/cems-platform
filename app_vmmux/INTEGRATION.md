# app_vmmux: the Valve Manager / Multiplexor on the CEMS core platform

Rev C, 21 Oct 2026 (Rev B: owner decisions Q1-Q7; Rev C: main `45a7cc3` merged: records, screens,
app selection, 128 KB FRAM; requests R1-R5 closed). Branch `app/vmmux` of `cems-platform`. Nothing under
`platform/` is changed.

## 1. What this is

The Stage 4 faithful C (`Existing Design/Analysis/stage4`) joined into one program and wired into
the core platform per `docs/APP_INTEGRATION.md`:

```
main():  plat_start(vmmux_app());
scan:    vm_io_in -> Function1 (VM top level, calls MUX at rung 36) -> vm_after_logic -> vm_io_out
```

| Path | Contents |
|---|---|
| `faithful/` | Stage 4 chunks 4A-4I, edited only as listed in section 3 (every edit is a git diff against the import commit) |
| `include/vm_types.h`, `vm_config.h` | the one copy of the project types; the compile switches (section 4) |
| `src/vm_tags.c` | **generated** (`tools/gen_tags.py`): every tag, once, with the data dictionary's power-up values |
| `src/vm_params.c`, `include/vm_params.h` | **generated** (`tools/gen_params.py`): parameter table + retained blocks |
| `src/vm_modbus.c`, `src/vm_params_modbus.inc` | **generated** (`tools/gen_modbus.py`): the old slave map at its old addresses |
| `src/vm_app.c` | `scan_app_t`, `plat_app_t`, data-log samples, remote-link watch, `fw_safe` |
| `src/vm_io.c` | platform I/O image <-> TA30 / UID tags (10_IO_Map) |
| `src/vm_alarms.c` | alarm table and producers (70_alarm_map) |
| `src/vm_compat.c` | keeps old Modbus coils and buffers meaningful where the platform took over |
| `src/vm_hmi.c` | the old screens' button actions as command parameters (5000+), their status (5100+), and the EntryOK "...Entered" bits |
| `src/vm_records.c` | the six sequence/schedule records (`PARAM_RECORD`): `vm_check_record`, which also writes the derived row numbers |
| `src/vm_ui.c`, `include/vm_ui.h` | the screens and record editors for the platform (`ui_app_t`, D26): seven tabs, five editors, and the display values they need (5150-5159) |
| `src/vm_select.c`, `app_vars.mk` | `app_select()` and the source lists: `make netsim APP=app_vmmux`, `make target APP=app_vmmux` (R1) |
| `docs/MODBUS_MAP.md` | the Modbus TCP register map for integrators (generated, `tools/gen_modbus_doc.py`) |
| `tests/integration/vm_net_test.py` | end-to-end on real sockets: touchscreen stand-in, web API, Modbus TCP, record editor API, restart (`make -f app_vmmux/app.mk vm-nettest`) |
| `tests/integration/vm_web_ui_test.py` | the screens on the web page in headless Chromium: every tab, a manual route by clicking, the sequence editor (`vm-webtest`) |
| `docs/BAT_VMMUX.md` | bench acceptance test procedure, step by step, each step tied to its host test |
| `docs/SCREENS.md` | the application screens as built (`vm_ui.c`), by parameter ID |
| `stage4_tests/` | the Stage 4 tests, run against the integrated code with every switch at the original behaviour |
| `tests/vm_tests.c` | application tests on the real scan engine, parameter store, alarm manager and Modbus engine |
| `tools/param_ids.csv` | parameter / block IDs. **Never renumber**: FRAM records are found by ID. The one exception, before any unit was built (21 Oct): the 57 valve names moved to consecutive IDs (1200-1218 inlets, 1220-1238 outlets, 1240-1258 MUX inlets) so a screen grid can label its items; the old IDs are kept as `retired:` rows and never reused |

Build and test (repository root): `make -f app_vmmux/app.mk vm-test` (12 Stage 4 suites, 110,000+ checks,
and 539 application checks including the touchscreen walk-through on the simulated panel), `vm-nettest`
(29 end-to-end checks on real sockets) and `vm-webtest` (14 checks in headless Chromium); ASan + UBSan,
platform warning flags. `make netsim APP=app_vmmux` runs the platform's networked simulation with this
application (Modbus TCP 1502, HTTPS 8443). Panel pictures: `UI_DUMP=build/vmui ./build/vm_tests` then
`tools/eve_preview.py build/vmui/*.eve`. Target: `tools/check_target.sh EXT=<ext>` links it for the STM32H573.
Regenerate after an Analysis change: `sh app_vmmux/tools/gen_all.sh "<path>/Existing Design/Analysis"`.

## 2. Numbers

| | |
|---|---|
| Tags | 450 definitions covering 768 dictionary tags, + 117 UI/Modbus-only tags |
| Parameters | 422 application entries + 40 platform = 462 of `PARAM_MAX` 512 (364 settings, records and Modbus views; 48 screen commands and status; 10 display values) |
| STM32H573 (21 Oct, `check_target.sh`, fullest configuration) | flash 509.5 KB of 1016 KB (50 %), RAM 434.5 KB of 640 KB (68 %); the application itself 95.7 KB flash, 46.1 KB RAM |
| Retained blocks | 194 |
| **FRAM user image** | **13,600 bytes**: 33 % of the 40 KB copy on the 128 KB part (Q7, now the platform default); the 32 KB breakout still fits (90.2 % of 14,848) with `PLAT_FRAM_SIZE=32768u` in the CubeIDE preprocessor symbols |
| Alarms | 57 (IDs 100-173) |
| Modbus | 201 old entries + 6 platform alarm entries |

## 3. Changes made to the Stage 4 code on integration

| # | Change | Why |
|---|---|---|
| I-1 | Per-chunk typedefs removed; `vm_types.h` and `ul_system.h` included (`tools/strip_typedefs.py`) | PLATFORM.md section 6 rules 1-3 (4F2's `rtc_date_t` had another member order) |
| I-2 | Tag definitions removed from 4G, 4H, 4I; system tags are the platform's | one definition per tag |
| I-3 | `SingularInlet` is `char[19][11]` (STRING 10), not `[21]` | the tag export; found by the generator's type check |
| I-4 | `CsvBufferStatus` INT8, `CurrentModeMUX` INT16 (4E.h) | the tag export |
| I-5 | `treset` defined once; the static timer/edge models of every chunk compared and identical | duplicate symbol |
| I-6 | `FillNumbers` / `FillNumbersMUX` take no parameters and write the globals | the original (41g); the 4A test passed arrays |
| I-7 | `Function1` reads `Sequence1/2` directly; the unused `Sequence1TimerOut` removed | the 4F4 test used a view struct |
| I-8 | 4C's `TwoORFb_22` stub removed (4F1 is the real one); `SearchInArray_u8` adapter for 4E | stubs of the Stage 4 tests |
| I-9 | `#include` paths flattened | one include directory |
| I-10 | Call sites of `Email` (Function1 R27) and `Logging`/`LoadTable` (MUX R9) behind switches | section 4 |
| I-11 | DI rungs 9-10 and the analog scaling behind switches | section 4 |
| I-12 | Function1 R37 / R38 behind `VM_FIX_VM_MANUAL_HOLD` (4F4) | V21: VM manual mode could not hold a route (section 4) |
| I-13 | `DO_Assign` (4C) calls `vm_do_assign_hook` at its end (NULL in Stage 4) | the screens show the route DO_Assign used; the selections are cleared every scan (V21) |

After I-1 to I-9 every Stage 4 test passes with exactly its 18 Oct check count.

## 4. Switches (`include/vm_config.h`) and what replaced what

| Switch | Default | Decision |
|---|---|---|
| `VM_OLD_EMAIL` | 0 | Platform alarms + SMTP replace VM `Email` (handover section 2). Its side effects that mattered are kept: coil 91 acknowledges all alarms, `FlowValidationAlarm` clears when the latched platform alarm is acknowledged, coils 77-85 follow the platform alarms (`vm_compat.c`, `vm_alarms.c`) |
| `VM_OLD_LOGGING` | 0 | Platform data logger replaces the data table / SD file (70 section 8). Channels AI1, AI2, RTD1, RTD2, one sample per scan with quality OK/FLAG/BAD/N/A |
| `VM_FIX_DI_REAL_INPUTS` | 1 | Open item 8: DISABLED rungs 9-10 of DI_Assigning run, so DIG_IN_01..20 reach the logic |
| `VM_FIX_AI_SCALING` | 1 | Open item 9: Low point at raw 0, High point at full scale |
| `VM_FIX_MUX_VALVE_ASSIGN` | 1 | MUX S7/S8: an inlet used twice is on in both channels, and inlet 1 can be scheduled (needed for X looping channels) |
| `VM_FIX_VM_RESTART` | 1 | Finding F7: the VM version of F5 (a run ending inside its window stood still and never ended, so a daily run happened once only) |
| `VM_FIX_VM_MODBUS_NAMES` | 1 | 47b M10/M12: a blank VM valve name no longer overwrites inlet 1's Modbus name; no tails of longer names |
| `VM_FIX_MUX_MODBUS_NAMES` | 1 | 47 M8: the MUX valve-name registers carry the MUX inlet names (they were never written) |
| `VM_FIX_V7` | 1 | VM V7: a tube-2 sequence of 9+ periods no longer stalls at period 9 |
| `VM_FIX_VM_MANUAL_HOLD` | 1 | VM V21: Function1 R37 cleared the selections every scan, so a manual route drove its valves for one scan only (BAT 5.1 could never pass). The route is now held while the manual run is on; R38 (`PauseMUX`) still computes OFF as the original did (V22 not changed) |
| `VM_FIX_TIME_LEFT` | 1 | Finding F8: the VM time left (`ProcessTimer(2).Preset`, holding registers and the screen) is set from the running mode's own timer after the logic; the idle schedule step had zeroed it in manual and Modbus runs |
| `VM_MUX_CONTINUOUS` | 1 | Q8: the MUX loops its sequence until stopped (no calendar window) |
| `VM_FIX_MUX_RESTART` | 1 | Finding F5: reset the MUX period timers when a run ends, so a restart in the window begins at period 1 (Gary: the MUX is the automated loop) |
| `ENABLE_DISABLED_RUNGS` | off | every other DISABLED IN ORIGINAL rung, kept in full, off |
| `VM_MB_BYTE_FLAGS` / `VM_MB_WORD_FLAGS` | 0 | Modbus byte / word order: Modbus convention (Q3) |

Other replacements: password screens -> platform logins; SN -> platform serial (param 50); ToEmail1..3 ->
platform mail recipients; brightness -> platform UI; Modify Alarm Property texts -> `text_param` of
each alarm; the Modbus client test screen -> dropped (no logic used it).

## 5. Behaviour found on integration (F5, F7, F8, F9 fixed behind switches; the rest recorded, not changed)

| # | Finding |
|---|---|
| F1 | A MUX valve reaches its output one scan (10 ms) late: Function1 writes the outputs (DO_Control, rung 28) before it calls MUX (rung 36). Same in the original. |
| F2 | DI_Control L45 loops `i = 0..20` over the 20-element `tmpDO` / `DOList`: one read and one write out of bounds every scan. Skipped (A4.1/A4.8) and counted in `g_oob` (2 per scan). |
| F3 | A DI alarm is raised on the third scan after the input changes (DI_Control runs before DI_Assigning copies the inputs, as in the original; the platform evaluates on the next scan). 30 ms. |
| F5 | **A MUX sequence that ends inside its start window restarts at once (S3) and then stands still**: its dwell/sample timers were never unpowered, so they are all "done", and no valve opens until the schedule is stopped. Fixed behind `VM_FIX_MUX_RESTART` (default on). Test `t_mux_schedule_run` checks both settings. |
| F7 | **The VM schedule has the F5 fault too**: a run that ends inside its window restarts (V9), stands still with no valve open and its run flag stuck on, so no later window can start it: a daily calibration would run once. Fixed behind `VM_FIX_VM_RESTART` (default on); test `t_vm_schedule_repeat` checks both settings. |
| F6 | The old screens' buttons and number boxes write logic tags directly (StartSchedule, ...Entered bits). Without them nothing can be started; `vm_hmi.c` reproduces them so the new UI and web page can operate the machine. Tests drive a MUX manual run, a MUX schedule and a VM Modbus run through these commands. |
| F8 | **The VM time left reads 0 during manual and Modbus runs**: ManualMode R10 and ModbusMode write `ProcessTimer(2).Preset`, then `ScheduleModeSTValveAssign` (4F2 `step_of`) overwrites it in the same scan with the idle schedule's period-1 timer (0). The master's time-left registers and the old screen showed 0. Fixed behind `VM_FIX_TIME_LEFT`; checked in `t_vm_modbus_run_and_link_loss` and `t_panel`. |
| F9 | V21 (46i) confirmed on the integrated code by the screen test: a manual VM route lasted one scan. Fixed behind `VM_FIX_VM_MANUAL_HOLD` (I-12); `t_screen_values` and `t_panel` hold a route for 1 s and release it with the Manual button. V5 (a touched valve's bit stays set, so the same valve cannot be pressed again) is not changed. |
| F4 | With every analog/RTD channel `NOT_FITTED` on the bench, an *enabled* channel raises its sensor-fault alarm, as the handover asks; disabled channels are quiet. |

## 6. Owner decisions (Gary, 20 Oct) and how they are built

| # | Decision | Built as |
|---|---|---|
| Q1 | On loss of the Modbus link: a Valve Manager run started over Modbus closes all its valves after 30 min; the Multiplexor (the automated loop) carries on | `vm_app.c`: platform `remote_link_lost` (REMOTE mode, no Modbus/web request for `comms_timeout`, 10 s) for `vm_link_close_min` (new parameter 2901, default 30, 0 = never) ends the VM run through the original's own end-of-run path (ModbusMode rung 6). Events 100-102 in the event log. Test `t_q1_link_loss` |
| Q2 | No limits needed where the old panel had none | the type's range stays (`params_report.csv` NO_LIMIT) |
| Q3 | Choose the Modbus byte / word order | Modbus convention = platform default: 16-bit big-endian; 32-bit and floats high word first (ABCD); text UTF-8, two bytes per register, first byte in the high half, NUL padded. The 44 description buffers are now UTF-8. `VM_MB_*_FLAGS` remain for a master that needs the other order |
| Q4 | DI alarm text (VM or MUX): not needed now | VM text, as built |
| Q5 | Range-alarm enables and limits retained; temperature alarm delay and hysteresis | `tools/decisions.py`: AnalogEnable, TemperatureEnable, AnalogAlarmEnabled, TemperatureAlarmEnabled, RangeMin/Max, Min/MaxTemperatureRange are retained settings (K4). New parameters `rtd1/2_alarm_delay_s` (default 10 s, the old temperature-email delay) and `rtd1/2_alarm_hyst` (0.1 degree, default 0). Test `t_q5_q6_retained` |
| Q6 | Calibration values retained | `FirstCalibPoint` / `SecondCalibPoint` are factory class: separate FRAM image, never cleared by "restore defaults". Note: a new unit shows the platform's CALIBRATION INVALID alarm until a calibration value has been saved once |
| Q7 | 128 KB FRAM | passed to the core thread (R4) |
| Q8 | MUX: "X channels, a time each runs for, loops until stopped for service"; and a master enable for the whole MUX feature. The VM keeps its own sequence and schedule. | `VM_MUX_CONTINUOUS` (default 1): scheduler mode ignores the calendar window and repeats the sequence until the Scheduler button, the digital stop input, a pause or the analyser lock stops it (channel = period, time = dwell + sample; set dwell 0 for a single time). `mux_enable` (parameter 2906, admin, retained, default on, `vm_mux.c`): off holds the MUX at rest, closes its valves, clears its alarms and refuses its buttons; the VM is unaffected. Tests `t_mux_schedule_run`, `t_mux_master_enable` |

## 7. Requests to the core-platform thread

| # | Request |
|---|---|
| R1 | **Closed** (core `c45c39e`): `app_select()`; `app_vars.mk` + `src/vm_select.c` added; `make netsim APP=app_vmmux`; the define in `app.mk` is gone. |
| R2 | **Closed** (D26): records and the screen API. Built in `vm_ui.c` (seven tabs, five editors); the six records are `PARAM_RECORD`s and no longer retained blocks. |
| R3 | **Closed**: `Touched_Bit` = panel touched within the last second (`ui_last_touch_ms`, `vm_ui_pre_logic`). |
| R4 | **Closed**: 128 KB is the default map. |
| R5 | **Closed**: `check_target.sh` links the application; re-run here on 21 Oct with the screens (figures in section 2). |
| R6 | **New.** The touchscreen opens an editor only for a whole parameter: a `UW_VALUE` with `index` (one element of an array) is shown but cannot be changed there. The VM/MUX keeps many settings as arrays (`AnalogEnable[2]`, `RangeMin[2]`, `DOInlet[19]`, `Di_FunctionVM[22]` ...): editable on the web page's settings list today, read-only on the panel's Inputs tab. Please let a `UW_VALUE` with `index` edit that element (a one-element block write), with the usual level and mode checks. |
| R7 | **New, for information.** `PARAM_MAX` 512: the platform's 40 and the application's 422 entries leave 50. If the platform's own table grows by more than that, the application will not start (`param_init` refuses the table; `vm_tests` prints the count). A `#ifndef PARAM_MAX` would let a build raise it. |
