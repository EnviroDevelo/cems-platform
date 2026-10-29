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

DK bring-up changes (28-29 Oct, before release):
- The I2C FRAM driver scans 0x50-0x57 at power-up and joins every Fujitsu MB85RC chip it finds,
  in address order, into one memory (sizes from each chip's Device ID). `/api/info` adds
  `"fram":{"bytes","chips"}`. The settings map is unchanged.
- An uncalibrated unit (factory region never set) keeps CAL_INVALID across power cuts while its
  service hours are saved (factory image flag HF_UNCAL).
- The HTTPS server keeps two browser connections open together (tls_use, HTTP_KEPT 2): a second
  viewer no longer forces the first to reconnect. TLS pool 44 -> 60 KB; the RAM stand-in for a
  missing FRAM shrinks to the used map (STORE_META_ADDR + 1 KB).
