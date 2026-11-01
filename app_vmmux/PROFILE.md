# CEMS Supervisor Valve Manager / Multiplexor (app_vmmux): product profile

The legacy Unitronics UniLogic VM/MUX program (Valve Manager + Multiplexor), translated to C
and run on the CEMS core platform. Sources and design: `INTEGRATION.md`. Spec section 9.2.

| | |
|---|---|
| Product | CEMS Supervisor Valve Manager / Multiplexor |
| Product version | 0.1.0-dev (`VM_APP_REVISION`) |
| Core version built on | core-0.10.0 (this tree, main f165dcc) |
| Core board revisions supported | any (no hardware range in development images) |
| Owner | VM/MUX application thread |

## Optional items fitted

| Requirement | Fitted? | Notes |
|---|---|---|
| CIP-02.6 Clock quality exposed | yes | platform default |
| CIP-05.4 Configuration checksum | yes | platform default |
| CIP-12.5 Configurable alarm actions (email, Modbus, relays) | yes | every VM/MUX alarm defaults to the Modbus common alarm only (the old program emailed only when ticked); email and relays per alarm on the Alarm setup page |
| CIP-13.4 Physical interfaces | plant Ethernet (Modbus TCP slave at the old addresses, HTTPS), I/O Ethernet (I/O boards) | 22 DI, 20 DO, 2 AI, 2 RTD in use (10_IO_Map); channels placed by each board's first_di / first_do / first_ai / first_temp |
| CIP-16.3 Log retention | platform log flash; 4 channels (AI1, AI2, RTD1, RTD2) at the platform log interval (parameter 30) | |
| CIP-17.7 Operator identity | login level only | the old password screens are the platform logins |
| CIP-18.7 Calibration procedure | analog scaling points `FirstCalibPoint` / `SecondCalibPoint` (factory data, Q6) | not on the platform's plat_cal records yet |
| CIP-20.7 Secure boot | no | waits on the bootloader |
| CIP-21.8 Local screen | touchscreen and web: seven tabs and five record editors (`vm_ui.c`, `docs/SCREENS.md`) | |
| CIP-24.5 Enclosure protection | site enclosure | not set by the firmware |

## Measurement channels

| Index | Parameter | Name | Unit | Source | Stale after | Notes |
|---|---|---|---|---|---|---|
| 0 | 4145 [0] | mb_AnalogSensor (AI1) | site units (`AnalogSensorUnits`) | analog input 1, scaled by the logic | 1000 ms | unavailable while `AnalogEnable[0]` is off, fault on an input fault |
| 1 | 4145 [1] | mb_AnalogSensor (AI2) | site units | analog input 2 | 1000 ms | as AI1 |
| 2 | 4146 [0] | mb_TemperatureSensor (RTD1) | 0.1 degC | RTD 1 | 1000 ms | unavailable while `TemperatureEnable[0]` is off |
| 3 | 4146 [1] | mb_TemperatureSensor (RTD2) | 0.1 degC | RTD 2 | 1000 ms | as RTD1 |

## States

| State | When | Outputs |
|---|---|---|
| WARMING_UP | not used: no warm-up | |
| READY | not used: in READY the platform marks every reading unavailable, which would hide the live flow and temperatures between runs | |
| MEASURING | from the first scan (decision 1 Nov 2026, `vm_app.c` report_state); a run being on is shown on the screens and Modbus as before | valves follow the running mode |
| MAINTENANCE | operator selects it | the logic keeps running: valves follow the running mode, readings carry the maintenance quality. Stop a run first if the plant must not be sampled |

A new unit is in FAULT (CALIBRATION INVALID) until a calibration value has been saved once (Q6).

## Product alarms and start-up checks

57 alarms, IDs 100-173, each with a `.help` (`FAULT_CODES.md`, `make fault-codes APP=app_vmmux`).
Tiers follow the platform default from severity (alarm = Medium, fault = High, warning = Low,
status = Info) and are set per alarm on the Alarm setup page (parameter 52). No start-up checks
of its own.

## Exceptions to mandatory requirements

| Requirement | Exception | Reason | Signed off (who, date) |
|---|---|---|---|
| - | - | - | - |
