# Supported combinations (CIP-25.5)

Which core firmware runs on which core hardware with which I/O boards, and what each one has
actually been tested on. Update this page at every core release (CHANGELOG.md), and when a board
revision or an I/O board map version is first tested.

"Tested" means the hardware test plan (`docs/SERVICE_PROCEDURES.md` 4 board test, the DK bench
plan, `tools/dk_soak.py`) passed on that combination. "Built" means it compiles, links and passes
the host tests, but has not run on that hardware.

## Core firmware × core hardware

| Core firmware | STM32H573I-DK (no straps) | CEMS core board rev 0 |
|---|---|---|
| core-0.9.0 (unreleased) | Partly tested: start-up, scan, FRAM (1 chip), Ethernet, HTTPS with two browsers, Modbus TCP, web tabs. Pending: network update, bootloader, board test, soak | Built only (board not made yet) |

Board revision is read from the straps on PE7/PE8 (CORE_PCB.md 12, parameter 319). An update file
states the revisions it runs on (`mkfwimage.py --hw`), and a board outside that range refuses it.

## Core firmware × I/O board register map

| Core firmware | I/O map 1 | I/O map 2 (`IOB_MAP_VERSION` 2: temperature inputs, pump PWM) |
|---|---|---|
| core-0.9.0 | Runs, without the map 2 features (host-tested) | Host-tested against the simulated node (`iob_node_sim.c`); not yet with a real I/O board |

The I/O board reports its map version in register 65 (`IOB_R_MAP_VERSION`) and its firmware in 67.

## Interfaces in this core version

| Interface | Version | Where it is read |
|---|---|---|
| Modbus platform block layout | 3 | input register 9000 |
| Application map version | per product | input register 9001 |
| Web API | 2 | `/api/info` `"api"` |
| Bootloader | 1 | `/api/fw` `"bootloader"."version"` |
| Firmware image header | 2 | `tools/mkfwimage.py`, `fw_header_t` |

Compatibility rules for all of these: `docs/INTERFACES.md` 2.
