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
