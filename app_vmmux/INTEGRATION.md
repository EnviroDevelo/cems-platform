# app_vmmux: the Valve Manager / Multiplexor on the CEMS core platform

Rev A, 20 Oct 2026. Branch `app/vmmux` of `cems-platform`. Built against platform commit `a986362`
(APP_INTEGRATION.md handover). Nothing under `platform/` is changed.

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
| `stage4_tests/` | the Stage 4 tests, run against the integrated code with every switch at the original behaviour |
| `tests/vm_tests.c` | application tests on the real scan engine, parameter store, alarm manager and Modbus engine |
| `tools/param_ids.csv` | parameter / block IDs. **Never renumber**: FRAM records are found by ID |

Build and test (repository root): `make -f app_vmmux/app.mk vm-test` (12 Stage 4 suites, 110,000+ checks,
and 157 application checks, ASan + UBSan, platform warning flags). `make -f app_vmmux/app.mk vm-netsim`
runs the platform's networked simulation with this application (Modbus TCP 1502, HTTPS 8443).
Regenerate after an Analysis change: `sh app_vmmux/tools/gen_all.sh "<path>/Existing Design/Analysis"`.

## 2. Numbers

| | |
|---|---|
| Tags | 450 definitions covering 768 dictionary tags, + 117 UI/Modbus-only tags |
| Parameters | 337 of 512 (141 settings, 1 new, 195 Modbus-only) |
| Retained blocks | 194 |
| **FRAM user image** | **13,296 bytes**; 14,848 per copy on the 32 KB part (**89.5 %**) |
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

After I-1 to I-9 every Stage 4 test passes with exactly its 18 Oct check count.

## 4. Switches (`include/vm_config.h`) and what replaced what

| Switch | Default | Decision |
|---|---|---|
| `VM_OLD_EMAIL` | 0 | Platform alarms + SMTP replace VM `Email` (handover section 2). Its side effects that mattered are kept: coil 91 acknowledges all alarms, `FlowValidationAlarm` clears when the latched platform alarm is acknowledged, coils 77-85 follow the platform alarms (`vm_compat.c`, `vm_alarms.c`) |
| `VM_OLD_LOGGING` | 0 | Platform data logger replaces the data table / SD file (70 section 8). Channels AI1, AI2, RTD1, RTD2, one sample per scan with quality OK/FLAG/BAD/N/A |
| `VM_FIX_DI_REAL_INPUTS` | 1 | Open item 8: DISABLED rungs 9-10 of DI_Assigning run, so DIG_IN_01..20 reach the logic |
| `VM_FIX_AI_SCALING` | 1 | Open item 9: Low point at raw 0, High point at full scale |
| `ENABLE_DISABLED_RUNGS` | off | every other DISABLED IN ORIGINAL rung, kept in full, off |
| `VM_MB_BYTE_FLAGS` / `VM_MB_WORD_FLAGS` | 0 | Modbus BUFFER byte order / 32-bit word order (Q3) |

Other replacements: password screens -> platform logins; SN -> platform serial (param 50); ToEmail1..3 ->
platform mail recipients; brightness -> platform UI; Modify Alarm Property texts -> `text_param` of
each alarm; the Modbus client test screen -> dropped (no logic used it).

## 5. Behaviour found on integration (recorded, not changed)

| # | Finding |
|---|---|
| F1 | A MUX valve reaches its output one scan (10 ms) late: Function1 writes the outputs (DO_Control, rung 28) before it calls MUX (rung 36). Same in the original. |
| F2 | DI_Control L45 loops `i = 0..20` over the 20-element `tmpDO` / `DOList`: one read and one write out of bounds every scan. Skipped (A4.1/A4.8) and counted in `g_oob` (2 per scan). |
| F3 | A DI alarm is raised on the third scan after the input changes (DI_Control runs before DI_Assigning copies the inputs, as in the original; the platform evaluates on the next scan). 30 ms. |
| F4 | With every analog/RTD channel `NOT_FITTED` on the bench, an *enabled* channel raises its sensor-fault alarm, as the handover asks; disabled channels are quiet. |

## 6. Questions for Gary (Rule 0: not guessed)

| # | Question | Where it is parked |
|---|---|---|
| Q1 | **Safe state on remote-link loss.** The platform says when REMOTE mode loses its master; what should the VM/MUX do: stop a Modbus-mode run? close all valves? carry on? | `vm_app.c` logs the event only |
| Q2 | **Limits where the old panel had none**: `FirstCalibPoint[4]`, `SecondCalibPoint[4]` and the presets of `FlowValidationTimer`, `Manual Timer`, `Manual Timer2`, `Manual TimerMUX`. They take the type's full range today, as the original did. | `tools/out/params_report.csv` (NO_LIMIT) |
| Q3 | **Modbus byte / word order** UniLogic used for BUFFERs and 32-bit values (handover section 7). One register read from a running unit settles it. | `VM_MB_*_FLAGS` |
| Q4 | **DI alarm texts**: VM and MUX inputs are merged into one alarm per input; it shows the VM text "Alarm<i> Description". Keep, or show the MUX text when the MUX function is set? | `vm_alarms.c` |
| Q5 | **Range alarms**: the enables and limits are RAM in the original (K4: no range alarms after a power-up) and the RTD alarms have no delay or deadband (K5: chatter). Make them retained settings and give the RTD alarms a delay? | `vm_alarms.c` |
| Q6 | **Calibration class**: should `FirstCalibPoint` / `SecondCalibPoint` be factory (calibration) parameters, kept through "restore defaults" and gated by `calibration_valid`? | `gen_params.py` |
| Q7 | **FRAM part**: 13,296 bytes of 14,848 (89.5 %) leaves 1.5 KB for growth. Recommend the 128 KB part. | (also R4) |

## 7. Requests to the core-platform thread

| # | Request |
|---|---|
| R1 | A way to choose the application in `host/netsim` (and `host/sim_main.c`) without the `-Dapp_demo_plat=(*vmmux_app())` define in `app.mk`. |
| R2 | The application-screen API (handover section 9). The Stage 5 screen list goes with it. |
| R3 | A UI-activity flag for the old screen-saver logic (`Touched_Bit`), low priority. |
| R4 | FRAM size: the application's user image is **13,296 bytes** (D6). |
| R5 | `make target` was not run here (no ARM toolchain in this workspace); please include `app_vmmux` in the target compile check, or run it on the PC with `ext/`. |
