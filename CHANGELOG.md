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

Emissions calculation engine (29 Oct, before release; `docs/EMISSIONS_MATH.md`):
- `emis.h` / `emis.c`: EN 14181 calibration function, ppm to mg/Nm3, wet to dry, T/p
  normalisation, O2 or CO2 reference, mass emission, clock-aligned short-term and daily averages
  with the IED validity rules and confidence interval subtraction, the EN 14181 valid calibration
  range; quality from the eight PLAT_Q codes, worst of the inputs.
- Parameters 400-441 (settings, admin, persisted, not from Modbus), 450-469 (results), retained
  block 910. Off by default (400): a product that does not use it is unchanged. 4.4 KB RAM.
- `plat_meas_value()` in plat_state.h.

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
