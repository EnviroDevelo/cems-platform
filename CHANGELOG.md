# Changelog

The core platform's releases (`plat_version.h`, git tags `core-MAJOR.MINOR.PATCH`; rules in
`docs/INSTRUMENT_PLATFORM_SPEC.md` section 9.1, R4). Each entry says what a product has to do to
take it (usually nothing). Product releases keep their own notes in their directory.

## core-0.9.0 (unreleased: first version, not yet run on hardware)

The CORE platform as built against the Common Instrument Platform Specification Rev 0.3:
scan engine, parameter table and store, event and data log, alarms, Modbus TCP/RTU with the
platform block (input registers 9000-9199, layout 2), HTTPS web interface (API version in
`/api/info`), touchscreen, I/O boards over Ethernet, firmware update with trial and rollback,
instrument state and quality, calibration records, clock quality, service history, start-up
self-test, PBKDF2 passwords with token recovery.

Products: build with `make conformance APP=<dir>`; write `PROFILE.md` and the interface
snapshots (`make snapshots APP=<dir>`).

Third-party instruments (29 Oct, before release; docs/MODBUS_ROLES.md, `inst.h`): the CORE as
Modbus TCP master for gas analysers and other devices on the I/O port (W5500 sockets 4-7), up to 8
instruments and 32 points with type, word order, scaling, range and quality from the instrument's
status word; parameters 500-621; `/api/inst` and the register explorer (`/api/inst/explore`).
Products: nothing to do (off until set up).

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
