# Changelog

The core platform's releases (`plat_version.h`, git tags `core-MAJOR.MINOR.PATCH`; rules in
`docs/INSTRUMENT_PLATFORM_SPEC.md` section 9.1, R4). Each entry says what a product has to do to
take it (usually nothing). Product releases keep their own notes in their directory.

## core-0.9.0 (unreleased: first version, not yet run on hardware)

The CORE platform as built against the Common Instrument Platform Specification Rev 0.3:
scan engine, parameter table and store, event and data log, alarms, Modbus TCP/RTU with the
platform block (input registers 9000-9199, layout 3), HTTPS web interface (API version in
`/api/info`), touchscreen, I/O boards over Ethernet, firmware update with trial and rollback,
instrument state and quality, calibration records, clock quality, service history, start-up
self-test, PBKDF2 passwords with token recovery.

Products: build with `make conformance APP=<dir>`; write `PROFILE.md` and the interface
snapshots (`make snapshots APP=<dir>`).

One source for the CORE and the I/O node (30 Oct, Gary):
- The standard CEMS Modbus block (VDI 4201-3, `docs/MODBUS_STANDARD_BLOCK.md`): input registers
  7000-7999 mirrored read-only in the holding registers: BasisM value and NE 107 status per slot,
  BasisS showing the force in effect, BasisR, calculated banks 1-7, qualities, counters, globals,
  word order setting (param 335). Built from the emissions engine's parameters; without them
  every slot reads NaN / not fitted. 43/14 now answers conformity 0x83 with the VDI 4201-3
  extended objects (serial, ComponentNumber 30, BasisM/S/R addresses, name/range/unit per
  component) and streams codes 1-3 with "more follows". A product map entry in 7000-7999 of the
  input or holding registers stops start-up (none of the current maps uses it).
- I/O boards: a board that has never answered since power-up gets PLAT_IOB_START_MS (20 s)
  before it counts as "not responding", so a restart no longer raises IOB_LOST while the network
  comes up (outputs stay off until it is online, as before). Fault text "No reply: board switches
  off after hold". Bench builds: `PLAT_BENCH_SETTINGS` writes are logged as param events.
- Automatic 10 Mbit (net_link_mode 0): the port counts TCP data segments and retransmissions
  (lwIP hook, `lwip_hooks.h`); two 10 s windows in a row with 40+ segments and 3 % or more sent
  again at 100 Mbit drop the offer to 10 Mbit until a restart or a setting change. Event
  `eth_speed`, `/api/info` eth gains `fallback`, `fallbacks`, `tcp_segs`, `tcp_rexmits`, and the
  header badge reads "ETH 10 FD (auto)". Bench: the DK lost 14-45 % at 100 Mbit through Gary's
  switch and none at 10 Mbit (its board's 100BASE-TX transmit signal; the Nucleo was clean).
- Digital-input alarms (`docs/MODBUS_STANDARD_BLOCK.md` section 7): 24 slots (params 340-345,
  labels 350-373 `diaN_label`, alarm IDs 30-53, PLAT_ALARM_MAX 128). Each ties a digital input to
  an alarm with trigger level, delay, latch and severity; the label becomes the alarm text, the
  Modbus map description and the Excel label. Discrete inputs 7000+s active, 7032+s unacked,
  7064+s unreliable, 7100+i raw input. `/api/modbus/map` lists them and gains the codes
  `std_status_bits` and `input_alarm_status`. Labels are written whole (no NUL).
- Ethernet on any switch: the MAC follows the PHY when a switch renegotiates without the link
  looking down; parameter 15 `net_link_mode` (admin) limits what the port offers (0 auto, 1 100 Mbit
  only, 2 10 Mbit only for long or poor cable) with autonegotiation always on; `/api/info` "eth"
  shows the negotiated mode, the switch's offer (anar/anlpar/aner), link drops and error counters;
  first TCP retransmit after 1 s instead of 3 s. Found on the DK bench (30 Oct): a cheap switch
  lost 10-20 % of the DK's frames silently while a direct cable lost none.
- The io-node repository (node core, chip drivers, its tests and tools, the G0 port) now lives here:
  `node/`, `tests/node/`, `tools/node/`, `port/node_stm32g0/`, `docs/IO_NODE.md`. `make test` runs its
  tests and the platform's host suite against the real node core (`make master-real`).
- The role (`plat_role.h`): CORE, NODE or NONE, from the shared PCB's jumper J7 (ROLE_A PE5,
  ROLE_B PE6) or fixed with `PLAT_ROLE_FIXED`. A board without the jumper's labels is a CORE, so
  existing projects need nothing new except the four source files and the `node` include path.
- The NODE role (`plat_node.h`): the node core over the platform's sockets in the iobus task;
  no display, web, plant Modbus or application. `build/nodesim` runs it on a PC;
  `tests/integration/node_role_test.py` runs a CORE against it.
- The NUCLEO-H563ZI as a node: `docs/CUBEMX_NUCLEO_NODE.md`; `tools/check_target.sh` links it (CHIP=h563).

DK bring-up changes (28-29 Oct, before release):
- The I2C FRAM driver scans 0x50-0x57 at power-up and joins every Fujitsu MB85RC chip it finds,
  in address order, into one memory (sizes from each chip's Device ID). `/api/info` adds
  `"fram":{"bytes","chips"}`. The settings map is unchanged.
- An uncalibrated unit (factory region never set) keeps CAL_INVALID across power cuts while its
  service hours are saved (factory image flag HF_UNCAL).
- The HTTPS server keeps two browser connections open together (TLS_CONNS 3: two kept and a
  spare, tls_use): a second viewer no longer forces the first to reconnect. A new connection
  takes a kept place only if one is free or idle over 1 s; otherwise it is served once on the
  spare and closed, so a browser's extra sockets never push out another's. TLS pool 44 -> 80 KB;
  the RAM stand-in for a missing FRAM shrinks to the used map (STORE_META_ADDR + 1 KB).
- Ethernet receive no longer depends on the RX interrupt alone: the receive task drains the ring
  every 50 ms and restarts a DMA suspended for lack of buffers (RBU); transmit frees sent
  descriptors before each send. Meant to stop the port going deaf after a debugger halt.
- First-stage bootloader (docs/BOOTLOADER.md, `make boot`, about 6 KB of 32 KB): checks the
  mapped image (header, SHA-256, signature with the product key), swaps to the other bank if it
  is bad, and rolls a new image back after 3 starts without confirmation (TAMP trial register).
  `fw_update` writes the image at `fw_flash_t.image_off` (from the link address) and sets and
  clears the trial mark. `mkfwimage.py --factory` writes a whole bank for factory programming.
  Products: none until the application is linked at 0x08008000 (BOOTLOADER.md 4).
- A rollback made by the bootloader is no longer undone: the previous image used to read its
  record as a failed trial and swap back to the image that had just failed.
- `/api/fw` gains `"bootloader"` (null when none ran; otherwise version, what it found in the
  running bank, whether it swapped or rolled back) from TAMP backup register 30.
- Factory board test (CIP-23.3, `plat_boardtest.h`): 12 checks, started by
  `POST /api/service/boardtest` at factory login and shown in `/api/service` `"boardtest"`. It
  saves itself as the test record (station "board"; that record also gets `"names"`).
- `docs/SERVICE_PROCEDURES.md`: the technician's update, recovery and factory provisioning.
- `docs/INTERFACES.md`: how to add an interface (CIP-13.5) and the compatibility policy (CIP-14.5).
- `tools/dk_soak.py`: read-only soak test against a real unit (HTTPS + Modbus load, heap/stack/
  restart checks from `/api/info`, CSV), standard-library Python only (CIP-26.6).
- `docs/SUPPORTED.md`: the supported-combinations matrix (CIP-25.5).
- Modbus forced values for commissioning (`modbus.h` `mb_force_*`): an admin forces a value the
  PLC reads for 30 s from the touchscreen or web (`/api/modbus/force`); Modbus only, logged
  (event `force`), counted at input register 9090 (block layout 3) and common alarm bit 4.
- `tools/iob_sim.py`: a pretend I/O board (register map 2, Modbus TCP and discovery) on a PC, for the
  DK bench before a board exists; `tests/integration/iob_sim_test.py` runs it against netsim's real master.
- `GET /api/modbus/map`: the register map in force as JSON, for the integrator's handover file.
- `/api/info` `"modbus"`: server status for the web Service tab. `docs/MODBUS.md`: the
  integrator's guide (CIP-26.7).
