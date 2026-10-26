# CEMS Supervisor (demo app): product profile

The demonstration application that exercises every platform feature (see
`docs/PROFILE_TEMPLATE.md`, spec section 9.2). Not a shipped product.

| | |
|---|---|
| Product | CEMS Supervisor (demo app) |
| Product version | 0.3.0-dev |
| Core version built on | core-0.9.0 (this tree) |
| Core board revisions supported | any (no hardware range in development images) |
| Owner | Firmware (CORE) thread |

## Optional items fitted

| Requirement | Fitted? | Notes |
|---|---|---|
| CIP-02.6 Clock quality exposed | yes | platform default |
| CIP-05.4 Configuration checksum | yes | platform default |
| CIP-12.5 Configurable alarm actions (email, Modbus, relays) | yes | relays not wired in the demo |
| CIP-13.4 Physical interfaces | plant Ethernet, I/O Ethernet (simulated boards) | |
| CIP-16.3 Log retention | the simulation's file-backed log flash; channels in `app_demo_log` | |
| CIP-17.7 Operator identity | login level only | |
| CIP-18.7 Calibration procedure | the "Level" channel: two-point zero/span in `app_demo.c` | |
| CIP-20.7 Secure boot | no | waits on the bootloader |
| CIP-21.8 Local screen | touchscreen (simulated) and web | |
| CIP-24.5 Enclosure protection | n/a | not a product |

## Measurement channels

| Index | Parameter | Name | Unit | Source | Stale after | Notes |
|---|---|---|---|---|---|---|
| 0 | 1008 | demo_level | % | simulated ramp in `logic()` | 1000 ms | range 10-90 %; calibrated channel "Level" |

## States

| State | When | Outputs |
|---|---|---|
| WARMING_UP | not used | |
| READY | after start-up, sequence idle | valve off |
| MEASURING | the demo sequence is running (`DemoRunning`) | valve on |
| MAINTENANCE | operator selects it | outputs held as they are |

## Product alarms and start-up checks

`DEMO_FAULT` (100), `DEMO_LEVEL_RANGE` (101); start-up check `demo_counter`. See
`make fault-codes APP=...` output.

## Exceptions to mandatory requirements

| Requirement | Exception | Reason | Signed off (who, date) |
|---|---|---|---|
| - | - | - | - |
