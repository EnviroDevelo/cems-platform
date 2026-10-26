# Common Instrument Platform Specification

Rev 0.2, 26 Oct 2026 (Rev 0.1: first draft. Rev 0.2: Gary's decisions; section 9, keeping the core common). Written from Gary's 26-item baseline (26 Oct).

This document sets out what every instrument in the CEMS / environmental monitoring family
gets from the common platform. Measurement-specific functions are added separately: pumps,
valves, sensors, the measurement and calibration procedures. Each requirement is marked as
mandatory for every instrument, optional by product, or service-only. Each one is also
checked against what the CORE platform (`PLATFORM.md`, `CORE_PCB.md`, `HARDWARE.md` and
open PRs #3 to #6) already does, so the gaps are visible.

## 1. How to read it

**Class**

| Mark | Meaning |
|---|---|
| **M** | Mandatory: every instrument in the family has it, whatever its size or interfaces |
| **O** | Optional by product: the platform provides it, and a product chooses whether to fit or enable it (a screen, a second network, the alarm email, log capacity) |
| **S** | Service-only: used by a technician or the factory, not by the site operator; must exist, but may sit behind the factory login or a service tool |

**CORE status**

| Mark | Meaning |
|---|---|
| **Done** | Built and tested in the CORE platform (host and simulation tests; see the hardware note below) |
| **Partial** | Some of the requirement is built; the gap is named |
| **Missing** | Not built yet |

**Hardware note.** Nothing has run on the STM32H573I-DK or a core board yet. "Done" means
built and host-tested (unit tests, simulated network, headless browser), and compiled and
linked for the H5. Every "Done" still has to be proved on hardware during bring-up and the
72-hour soak (`HARDWARE.md`, "To check on the DK").

**Requirement IDs** are `CIP-nn.m`: `nn` is Gary's item number, `m` the requirement within it.

## 2. The three principles

These hold across every item below and override any item where they conflict.

| # | Principle | How the CORE platform meets it today | Gap |
|---|---|---|---|
| P1 | **A reading always carries its quality status.** A plausible number from a stalled or faulted instrument must not silently appear valid. | Logged values carry OK / FLAG / BAD / N/A. Inputs carry `di_fault`, `ai_status`, `rtd_status`. A lost I/O board marks all its channels faulty. The web page greys values and shows "Connection lost" when polling stops. | **Live values on Modbus, the web API and the touchscreen have no per-value status.** A PLC reading a register cannot tell valid from stale. See CIP-10. |
| P2 | **Power loss is a normal design condition.** Settings, calibration and records survive it predictably. | FRAM dual copies with CRC and sequence, tested with a power cut at every byte offset of a save. NOR log records each have their own CRC, tested with 400 random power cuts. Firmware update tolerates one power cut during its trial. | Settings the logic changes in the last second before power-off may be lost (documented). The supply-sense input (`CORE_PCB.md`, PC0) is not used yet, so there is no early power-fail warning. See CIP-07. |
| P3 | **Updates preserve identity and calibration.** What is retained, migrated or erased is defined explicitly. | Firmware update keeps the whole FRAM and the logs. Settings migrate by record ID with a `migrate` hook. "Restore defaults" never touches the factory region. Backups never carry the serial or secrets. | **"Factory reset" is not defined as its own operation**, and the full table had not been written down until now. See section 5. |

## 3. Summary

| # | Core item | Class | CORE status | Main gap | Owner |
|---|---|---|---|---|---|
| 1 | Instrument identity | M | Partial | No standard identity block on Modbus; hardware revision straps not read | Firmware, PCB |
| 2 | Clock and timekeeping | M | Done | Local standard time kept by decision (D22, an exception to the baseline); clock quality not exposed (optional) | Firmware |
| 3 | Firmware updates | M | Done | Compatibility check covers the product only, not the hardware revision | Firmware |
| 4 | Bootloader and recovery | M (interface S) | Partial | No immutable first-stage bootloader; debug lock state undecided | Firmware, PCB |
| 5 | Configuration management | M | Done | No visible configuration revision or checksum | Firmware |
| 6 | Non-volatile storage | M | Done | Not yet proved on the real FRAM and NOR | Firmware |
| 7 | Power management | M | Partial | Supply sense unused; no defined shutdown behaviour | Firmware, PCB |
| 8 | Watchdog and fault recovery | M | Partial | No reboot-loop detection or safe mode | Firmware |
| 9 | Instrument operating states | M | Missing | No shared state model; only LOCAL/REMOTE and firmware trial | Firmware (+ HMI, HTTPS) |
| 10 | Measurement validity and quality flags | M | Partial | Live values carry no status (principle P1) | Firmware (+ HMI, HTTPS) |
| 11 | Diagnostics and self-tests | M | Partial | No explicit start-up self-test set or fault code list | Firmware |
| 12 | Alarm management | M | Done | None at platform level | Firmware |
| 13 | Communications framework | M (interfaces O) | Done | No written framework document for new interfaces | Firmware |
| 14 | Common data model | M | Partial | No versioned register map or API compatibility policy | Firmware, HTTPS |
| 15 | Communications health | M | Partial | No heartbeat register or last-good-communication time exposed | Firmware |
| 16 | Data logging | M (capacity O) | Done | Power-off gaps are implied, not marked in the data CSV | Firmware |
| 17 | Event and audit logging | M | Done | Calibration actions wait on item 18 | Firmware |
| 18 | Calibration framework | M | Partial | Storage only; no calibration record (dates, references, outcome, factory/field) | Firmware |
| 19 | User and service access | M | Partial | No calibration level; no credential recovery | Firmware, HMI, HTTPS |
| 20 | Cybersecurity | M | Partial | Debug lock, secure boot, short PINs | Firmware, PCB |
| 21 | User interface conventions | M (screen O) | Partial | No written convention; quality and comms health not on every value | HMI, HTTPS |
| 22 | Service and maintenance support | S | Partial | No diagnostic package; no operating-hours counter; no service records | Firmware, HTTPS |
| 23 | Factory provisioning and testing | S | Partial | No provisioning procedure, board test or stored test results | Firmware, PCB |
| 24 | Electrical and environmental robustness | M | Partial | No written design basis (EMC levels, IP, humidity) or test plan | PCB |
| 25 | Version control and traceability | M | Partial | No release notes, build ID or supported-combinations matrix | Firmware |
| 26 | Verification and documentation | M | Partial | Nothing run on hardware; no operator, service or comms manuals | All |

Score: 8 Done, 17 Partial, 1 Missing.

## 4. Requirements

Each table lists the requirements for one item, its class, the CORE status and where it is
(or will be) met.

### 4.1 Instrument identity

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-01.1 | Unique serial number, changeable only under developer authority, never copied by a backup restore | M | Done | D25, `plat_serial.c`, parameter 50 |
| CIP-01.2 | Product/model identifier | M | Done | `cfg.product` in `/api/info`; `PLAT_FW_PRODUCT` in the image header |
| CIP-01.3 | Hardware revision, read from the board | M | Partial | Parameter `hw_rev` is a factory setting. The board has revision straps (PE7, PE8 in `CORE_PCB.md`) but the firmware does not read them. **Gap:** read the straps at boot and refuse or warn when they disagree with `hw_rev` |
| CIP-01.4 | Firmware version | M | Done | `/api/info`, System screen, power-up event |
| CIP-01.5 | Identity readable locally (screen or service port) | M | Done | System screen; web page |
| CIP-01.6 | Identity readable remotely over every supervisory interface | M | Partial | `/api/info` has it all. **Gap:** Modbus has no fixed identity block. Add Modbus function 43/14 (Read Device Identification) or a fixed register block that every product keeps at the same address |
| CIP-01.7 | Chip unique ID as the root of identity (MAC, serial tokens) | M | Done | `bsp_chip_uid` |

### 4.2 Clock and timekeeping

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-02.1 | RTC with 32.768 kHz crystal and battery backup | M | Done | H1; CR2032 on VBAT in `CORE_PCB.md` |
| CIP-02.2 | Time synchronisation | M | Done | SNTP, PLC (parameter 85), web, touchscreen (D22) |
| CIP-02.3 | Detection of an invalid clock | M | Done | "clock not set"; the scan keeps the previous snapshot and counts it |
| CIP-02.4 | Timestamps stored in UTC; local time applied for display | M | Done (exception) | **Exception by decision (Gary, 26 Oct): keep D22.** The RTC and records hold local standard time (no daylight saving) plus a UTC offset setting. Records never jump, because there is no daylight saving. A change of the offset is logged, and records are read with the offset in force when they were written |
| CIP-02.5 | Every clock change logged with source and old/new time | M | Done | Event log, D22 |
| CIP-02.6 | Clock quality exposed (synchronised, last sync time, source) | O | Missing | Not exposed on Modbus or the web API |

### 4.3 Firmware updates

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-03.1 | Documented local update method | M | Partial | Web upload by factory login (`PLATFORM.md` 5b). **Gap:** no technician's procedure document |
| CIP-03.2 | Compatibility check before install | M | Partial | Product code, size, minimum version. **Gap:** add the supported hardware revision range and the settings layout version to the image header |
| CIP-03.3 | Integrity verification | M | Done | SHA-256 while streaming and again after flash read-back |
| CIP-03.4 | Authenticity verification | M | Done | ECDSA P-256 signature (D14). Production key still to be generated |
| CIP-03.5 | Clear update status | M | Done | Progress, staged, trial countdown on web and touchscreen; update history (last 6) |
| CIP-03.6 | Interrupted or failed update leaves a recovery path | M | Done | Previous bank never erased; trial boot with automatic rollback; FW_ROLLED_BACK alarm |
| CIP-03.7 | Update refused while the instrument is in an unsafe state | M | Done | `fw_safe()` gate (D23) |

### 4.4 Bootloader and recovery

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-04.1 | Protected bootloader that cannot be overwritten by an application update | M | Missing | Today the image is started by the ROM and `fw_boot()` in the application. An image too broken to reach `fw_boot()` cannot roll back (`PLATFORM.md` 5b limits). **Gap:** immutable first stage (ST OEMiRoT or MCUboot) |
| CIP-04.2 | Recovery of corrupted application firmware | M | Partial | Bank rollback covers a bad update. A corrupted running image needs the debugger |
| CIP-04.3 | Physical service/programming interface | S | Done | SWD 10-pin, BOOT0 jumper, service UART (`CORE_PCB.md` J5, J6) |
| CIP-04.4 | Recovery possible when normal communications fail | S | Partial | SWD and the ST ROM bootloader through BOOT0 work without Ethernet. **Gap:** a written recovery procedure, and the production debug lock (RDP / product state) chosen so this still works |

### 4.5 Configuration management

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-05.1 | Persistent settings with defaults | M | Done | Parameter table, `PF_PERSIST` |
| CIP-05.2 | Permitted ranges and validation on every route | M | Done | One check order for touchscreen, web and Modbus; re-checked in the scan |
| CIP-05.3 | Version identification of the settings structure | M | Done | `layout_version` in each FRAM image |
| CIP-05.4 | Version identification of a configuration (which settings a unit has) | O | Missing | **Gap:** a configuration checksum shown on the System screen and in exports, so two units can be compared |
| CIP-05.5 | Export and import | M | Done | Backup/restore with CRCs; factory part only at factory level |
| CIP-05.6 | Controlled migration when firmware changes the structure | M | Done | Records by ID; unknown skipped; missing defaulted; `migrate` hook; tested |

### 4.6 Non-volatile storage

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-06.1 | Defined storage areas for configuration, calibration, logs and identity | M | Done | FRAM factory/user regions and META; NOR event and data rings |
| CIP-06.2 | Corrupted records detected | M | Done | CRC-32 per image and per log slot |
| CIP-06.3 | Interrupted writes do not corrupt | M | Done | A/B copies; log slots written only into erased flash and verified |
| CIP-06.4 | Memory wear managed | M | Done | FRAM endurance; NOR rings wear evenly |
| CIP-06.5 | Defined behaviour when storage is full | M | Done | Oldest sector overwritten (D12); settings table too large stops start-up with outputs safe |
| CIP-06.6 | Storage faults raised as alarms | M | Done | PARAM_WRITE_FAIL, LOG_STORAGE_FAULT, CAL_INVALID |

### 4.7 Power management

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-07.1 | Defined supply requirements | M | Done | 24 VDC, 18-32 V (`CORE_PCB.md` 5) |
| CIP-07.2 | Protection against expected supply faults | M | Done (design) | Fuse, reverse-polarity diode, TVS, 60 V buck. Schematic in PR #5, not built |
| CIP-07.3 | Brownout detection | M | Partial | MCU BOR; brown-out reset recorded and alarmed. **Gap:** supply-sense ADC (PC0) not read by the firmware |
| CIP-07.4 | Predictable start-up | M | Done | Always LOCAL, outputs all off, settings loaded before the first scan |
| CIP-07.5 | Predictable shutdown | M | Missing | **Gap:** on falling supply, stop log and FRAM writes before the brown-out, log "power fail", show supply voltage on the System screen |
| CIP-07.6 | Automatic recovery after power restoration | M | Done | No operator action needed; network, I/O boards and logging resume |

### 4.8 Watchdog and fault recovery

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-08.1 | Hardware watchdog | M | Done | IWDG, kicked only if every task checked in |
| CIP-08.2 | Software task supervision | M | Done | `task_monitor` with per-task silence limits |
| CIP-08.3 | Recovery from stalled operations | M | Done | Outputs to safe state, then reset; display and I/O boards retried without a reset |
| CIP-08.4 | Reset cause recorded | M | Done | `plat_diag`, power-up event, UNEXPECTED_RESET; CPU fault handlers in PR #3 |
| CIP-08.5 | Repeated reboot failures detected | M | Partial | `boots` and `wdg_resets` are counted. **Gap:** after N resets within M minutes, start in a safe mode (no application logic, network and service only) and raise an alarm |

### 4.9 Instrument operating states

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-09.1 | One shared state model: STARTING, INITIALISING, WARMING_UP, READY, MEASURING, MAINTENANCE, CALIBRATION, FAULT (and SAFE_MODE from CIP-08.5) | M | Missing | The platform has LOCAL/REMOTE, firmware trial, CAL_INVALID and the application's own states, but no common state. **Gap:** a platform state variable the application sets through `plat_app_t`, with the platform forcing FAULT or SAFE_MODE |
| CIP-09.2 | Defined data permitted in each state (which quality flag readings get) | M | Missing | Ties to CIP-10: e.g. every reading is "maintenance" while in MAINTENANCE |
| CIP-09.3 | Defined actions permitted in each state | M | Partial | `fw_safe()` already refuses updates during a calibration. **Gap:** a state/action matrix (update, calibrate, restore defaults, mode change) |
| CIP-09.4 | State shown on every interface | M | Missing | Touchscreen header, web header, a Modbus register |
| CIP-09.5 | State changes logged | M | Missing | Event log |

The agreed state/action matrix is in section 7.

### 4.10 Measurement validity and quality flags

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-10.1 | Every reported measurement carries a status | M | Partial | Logged values: yes. Inputs: fault bits and analog status. **Gap:** live values on Modbus, `/api/values` and the touchscreen have none |
| CIP-10.2 | Status codes distinguish: valid, stale, unavailable, out-of-range, warming-up, maintenance, calibration (plus fault) | M | Partial | The log has OK / FLAG / BAD / N/A. **Gap:** one platform quality enum with these 8 values; the log keeps its 4 as a summary of it |
| CIP-10.3 | A value not updated within its expected period becomes "stale" automatically | M | Missing | **Gap:** the platform stamps each measured value with its update time and marks it stale itself, so a stalled application cannot leave a good-looking number |
| CIP-10.4 | Instrument state forces quality (CIP-09.2) | M | Missing | |
| CIP-10.5 | Status readable on Modbus next to the value | M | Missing | Proposal: a status register per measurement, at a fixed offset from the value |

### 4.11 Diagnostics and self-tests

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-11.1 | Start-up checks of essential functions | M | Partial | FRAM image CRCs and device ID, NOR position rebuild, display probe, font CRCs, I/O board identification. **Gap:** a named start-up self-test list (firmware image CRC, RAM test, RTC running, FRAM and NOR IDs, PHY link) with one pass/fail result each |
| CIP-11.2 | Ongoing checks | M | Done | Heap and stack margins, scan overruns, store and log status, display, I/O boards, IP conflict |
| CIP-11.3 | Consistent fault codes with descriptions | M | Partial | Platform alarms 1-18 have IDs and texts; `plat_fault_t` codes. **Gap:** one published fault/alarm code list for the family (IDs 1-99 platform, 100+ product) |
| CIP-11.4 | Service access to diagnostic information | S | Done | `/api/info` (health, scan, last fault, boots), System screen |

### 4.12 Alarm management

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-12.1 | Common priorities | M | Done | Status, warning, alarm, fault |
| CIP-12.2 | Activation and clearance rules | M | Done | On/off delays, delay parameter, range deadband, enable |
| CIP-12.3 | Acknowledgement and latching | M | Done | Three states; latch; acknowledge one or all from every interface |
| CIP-12.4 | Timestamped alarm history | M | Done | Every raise, clear and acknowledge in the event log |
| CIP-12.5 | Configurable alarm actions | O | Done | Email, Modbus common alarm, relays (D20) |
| CIP-12.6 | Alarm states over a power cut | M | Done | Re-evaluated at power-up (D19); history in the log |

### 4.13 Communications framework

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-13.1 | Shared architecture: every interface reads the published snapshot and writes through the parameter table | M | Done | `PLATFORM.md` 5 |
| CIP-13.2 | Device addressing and connection settings | M | Done | IP/DHCP, Modbus allowed client, I/O network addresses |
| CIP-13.3 | Timeouts and error handling | M | Done | Modbus exceptions, keepalive, client eviction, HTTPS limits, I/O bus retries |
| CIP-13.4 | Physical interfaces chosen per product with common behaviour | O | Done | Client Ethernet, I/O Ethernet (W5500), RS-485 setting kept in firmware |
| CIP-13.5 | Written framework for adding an interface | M | Missing | **Gap:** a short guide (where a new transport plugs in, which checks it must use) |

### 4.14 Common data model

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-14.1 | Consistent parameter names, units, types, ranges | M | Done | One `param_def_t` table with name, unit, type, range |
| CIP-14.2 | Scaling and byte/word order defined per value | M | Done | `mb_map_t` flags and `dec` |
| CIP-14.3 | Shared status definitions | M | Partial | Waits on CIP-09 and CIP-10 |
| CIP-14.4 | Versioned register map and API | M | Missing | **Gap:** a map version register and a `"api"` version in `/api/info`; a generated register map document per product release |
| CIP-14.5 | Compatibility policy | M | Missing | Proposal: addresses never move within a major version; new values only added; removed values read as "unavailable" for one major version |
| CIP-14.6 | Platform parameters 1-999 the same in every product; product parameters from 1000 | M | Done | Already the numbering rule |

### 4.15 Communications health

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-15.1 | Heartbeat or health indication | M | Partial | **Gap:** a heartbeat counter register the PLC can watch (the PLC's own watchdog then covers a stopped core, as decided for the system-OK relay) |
| CIP-15.2 | Last successful communication tracked | M | Partial | Tracked internally for REMOTE mode and per I/O board. **Gap:** expose "seconds since last supervisory request" and per-board last-good time |
| CIP-15.3 | Stale commands and data detected | M | Done | I/O outputs: sequence numbers, all-off after 250 ms of no scan output; boards marked lost after 1 s |
| CIP-15.4 | Defined behaviour on loss and restoration of the supervisory link | M | Done | REMOTE_LINK_LOST after `comms_timeout`, application safe state; recovers on traffic |

### 4.16 Data logging

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-16.1 | Timestamped measurement records with quality | M | Done | Interval means with quality |
| CIP-16.2 | Defined logging interval | M | Done | Parameter 30 |
| CIP-16.3 | Defined retention | O | Done | Set by NOR size (capacity table in `PLATFORM.md` 5a) |
| CIP-16.4 | Storage-full behaviour | M | Done | Overwrite oldest (D12) |
| CIP-16.5 | Export format | M | Done | CSV with schema headers; date range |
| CIP-16.6 | Gaps and clock changes handled | M | Partial | Clock steps close the interval. **Gap:** mark a power-off gap explicitly in the data CSV (a row with quality N/A or a comment line) rather than only in the event log |

### 4.17 Event and audit logging

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-17.1 | Power cycles and resets with cause | M | Done | |
| CIP-17.2 | Faults | M | Done | |
| CIP-17.3 | Firmware updates | M | Done | Every step, plus the update history |
| CIP-17.4 | Clock changes | M | Done | |
| CIP-17.5 | Configuration changes with old and new values and source | M | Done | Touchscreen/web/Modbus and login level |
| CIP-17.6 | Calibration actions | M | Missing | Comes with CIP-18 |
| CIP-17.7 | Operator identity where available | O | Partial | Login level only; no named users |

### 4.18 Calibration framework

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-18.1 | Common storage of calibration data | M | Done | FACTORY region, dual copies |
| CIP-18.2 | A calibration record: coefficients, date and time, reference values, outcome (pass/fail/aborted), who | M | Missing | **Gap:** a platform `cal_record_t` per calibrated channel, with a short history (e.g. last 8) |
| CIP-18.3 | Factory calibration distinguished from field calibration | M | Missing | Proposal: factory record kept separately and never overwritten in the field; field calibration stored as its own record; "revert to factory calibration" available at factory level |
| CIP-18.4 | Changes validated before use | M | Partial | Range checks on the values. **Gap:** a product hook to accept or reject a whole calibration (as `check_block` does for records) |
| CIP-18.5 | Calibration survives firmware updates and restore defaults | M | Done | Tested (`t_migration`, `t_restore_defaults_cmd`) |
| CIP-18.6 | Invalid calibration blocks measurement | M | Done | CAL_INVALID |
| CIP-18.7 | The procedure itself | O | n/a | Product-specific, out of platform scope |

### 4.19 User and service access

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-19.1 | Separate permission levels: view, operation, configuration, calibration, engineering | M | Partial | View, operator, admin, factory (+ developer token for the serial). **Gap:** a calibration level (or a calibration permission) between admin and factory |
| CIP-19.2 | Sensitive actions protected | M | Done | Levels, confirmations (`PF_CONFIRM`), touchscreen confirmation of web firmware restarts |
| CIP-19.3 | Controlled credential recovery | M | Missing | **Gap:** today a lost admin PIN needs the factory PIN. Proposal: a one-time reset token signed by the developer key and bound to the chip ID (the same mechanism as the serial), entered on the touchscreen |
| CIP-19.4 | Lockout against guessing | M | Done | 5 tries, 30 s |
| CIP-19.5 | Session expiry | M | Done | 10 minutes |

### 4.20 Cybersecurity

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-20.1 | Changes authenticated | M | Done | Logins on web; HttpOnly SameSite cookie; `X-CEMS` header |
| CIP-20.2 | Updates authenticated | M | Done | Signed images (D14) |
| CIP-20.3 | Credentials protected | M | Partial | Salted SHA-256, write-only secrets, not in backups. **Gap:** 4-digit PINs; a slow hash (PBKDF2) and longer passwords for web logins |
| CIP-20.4 | Unnecessary interfaces restricted | M | Partial | Separate I/O network; port 80 redirect can be closed; Modbus read-only and IP filter; no outbound connections by default. **Gap:** production debug lock (RDP / product state) |
| CIP-20.5 | External inputs validated | M | Done | One check path; strict JSON and Modbus parsing; CSP (PR #6) |
| CIP-20.6 | Secure network communications | M | Done | HTTPS only (D11); SMTP over TLS with pinning |
| CIP-20.7 | Secure boot | O | Missing | Comes with CIP-04.1 on the H573 |

### 4.21 User interface conventions

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-21.1 | Readings shown with units | M | Done | Units in the table; own fonts for ° ³ µ (D24) |
| CIP-21.2 | Instrument state always visible | M | Partial | LOCAL/REMOTE and alarms in the header. **Gap:** the CIP-09 state |
| CIP-21.3 | Alarms always visible | M | Done | Alarm pill / badge on every screen |
| CIP-21.4 | Quality shown on every value | M | Missing | **Gap:** a non-valid value is greyed or marked with its status on the touchscreen and web |
| CIP-21.5 | Communications health visible | M | Partial | Web "Connection lost" banner; LINK LOST in the header; I/O boards screen. **Gap:** a common indicator for the supervisory link on the touchscreen |
| CIP-21.6 | Version information | M | Done | System screen, web Administration |
| CIP-21.7 | Written UI convention (colours, severity, units, status wording) | M | Missing | **Gap:** one page every product's screens follow |
| CIP-21.8 | A local screen | O | Done | RVT70 touchscreen; a product may ship with web only |

### 4.22 Service and maintenance support

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-22.1 | Exportable diagnostic package | S | Missing | **Gap:** one download (zip or JSON) with `/api/info`, settings (no secrets), event log, last fault, firmware history, I/O board status |
| CIP-22.2 | Operating-hours counters | S | Missing | **Gap:** total hours and hours since last service, in FRAM META, kept over updates and restore defaults |
| CIP-22.3 | Fault history | S | Done | Event log; last fault; firmware history |
| CIP-22.4 | Service records | S | Missing | **Gap:** a short service log (date, technician, work done) entered at factory level |
| CIP-22.5 | Identify and diagnose without source code | S | Partial | Most information is on screen. Needs CIP-11.3 and a service manual |

### 4.23 Factory provisioning and testing

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-23.1 | Controlled assignment of identity | S | Done | Serial token tool (D25) |
| CIP-23.2 | Loading of firmware and defaults | S | Partial | SWD for first programming. **Gap:** a written provisioning sequence (program, sign, set serial, set hw_rev, calibrate, test) |
| CIP-23.3 | Hardware test procedures | S | Missing | **Gap:** a board test (FRAM, NOR, RTC, both PHYs, display, supply sense) runnable from the service UART or the web at factory level |
| CIP-23.4 | Retained manufacturing test results | S | Missing | **Gap:** a test result record in the FACTORY region (date, station, pass/fail per test) |
| CIP-23.5 | Factory-only settings protected | S | Done | `PF_FACTORY`, factory level, never restored from a user-level backup |

### 4.24 Electrical and environmental robustness

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-24.1 | Grounding and shielding basis | M | Partial | Chassis/logic ground scheme, shielded RJ45s (`CORE_PCB.md` 2, 7) |
| CIP-24.2 | ESD and surge protection | M | Partial | TVS on supply, ESD arrays on Ethernet. **Gap:** the target levels |
| CIP-24.3 | EMC design basis | M | Done (design, PR #5) | `docs/CORE_PCB_EMC.md`: IEC 61326-1 industrial immunity levels (61000-4-2/-3/-4/-5/-6/-8), CISPR 11 Class A; input gained a common-mode choke, Y-caps and a 3 kW TVS. Not yet tested |
| CIP-24.4 | Temperature and humidity | M | Done (design, PR #5) | -20 to +55 °C, 5-95 % RH with conformal coating (`docs/CORE_PCB_EMC.md`) |
| CIP-24.5 | Enclosure protection | O | Partial | Core board: IP20 DIN housing in a panel. The instrument enclosure IP rating is per product |

### 4.25 Version control and traceability

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-25.1 | Traceable firmware revisions | M | Partial | Git and a version number. **Gap:** build ID (git hash) in the image and on `/api/info` |
| CIP-25.2 | Traceable hardware revisions | M | Partial | `hw_rev`; schematic in git. Needs CIP-01.3 |
| CIP-25.3 | Traceable configuration and calibration revisions | M | Missing | CIP-05.4 and CIP-18.2 |
| CIP-25.4 | Release notes | M | Missing | **Gap:** `CHANGELOG.md` and a release note per version |
| CIP-25.5 | Supported combinations identified | M | Missing | **Gap:** a matrix of firmware × core hardware revision × I/O board register map version |

### 4.26 Verification and documentation

| ID | Requirement | Class | Status | Where / gap |
|---|---|---|---|---|
| CIP-26.1 | Shared test: start-up | M | Done (host) | `make sim`, `make rtos` |
| CIP-26.2 | Shared test: power loss | M | Done (host) | Every-byte FRAM cut; 400 random log cuts |
| CIP-26.3 | Shared test: corrupted settings | M | Done (host) | `t_blank_fram`, `t_both_factory_copies_bad` |
| CIP-26.4 | Shared test: communications loss | M | Done (host) | Remote-link watchdog, connection-lost banner, I/O board lost |
| CIP-26.5 | Shared test: update failure and recovery | M | Done (host) | `fw_test.py` rollback |
| CIP-26.6 | The same tests on hardware | M | Missing | DK bring-up, then the core board; 72-hour soak |
| CIP-26.7 | Operator, service and communications documentation with each release | M | Missing | **Gap:** operator manual, service manual, Modbus register map document. `PLATFORM.md` is a developer document |

## 5. What an update or reset keeps (principle P3)

Today's behaviour, read from the code. Rows marked *proposed* are not built yet.

| Data | Firmware update | Restore defaults | Backup restore (admin) | Factory reset *(proposed)* |
|---|---|---|---|---|
| Serial number, chip ID | Kept | Kept | Never changed | Kept |
| Hardware revision | Kept | Kept | Factory level only | Kept |
| Calibration (factory region) | Kept, migrated by ID | Kept | Factory level only | Kept |
| User settings | Kept, migrated by ID (unknown dropped, new ones default) | **Defaults** | Replaced from the file | **Defaults** |
| Network settings | Kept | Defaults (192.168.1.50) | Replaced | Defaults |
| Passwords | Kept | **Defaults** (D9) | Never changed (D21) | Defaults |
| Mail password, TLS key | Kept | Mail password: to confirm; TLS key kept | Never changed (D21) | Both erased (new TLS key at next start) |
| Event log | Kept | Kept | Kept | **Kept** (audit trail is never erased by a user action) |
| Data log | Kept | Kept | Kept | Kept |
| Firmware history | Kept | Kept | Kept | Kept |
| Previous firmware image | Kept in the spare bank | Kept | Kept | Kept |
| Operating hours *(proposed)* | Kept | Kept | Kept | Kept |

Proposal: "factory reset" is restore defaults plus erasing the TLS key and mail password,
at factory level only, logged. Nothing a user can do erases identity, calibration or the logs;
only a developer-signed token could clear the logs (for refurbishing a unit).

## 6. Decisions

All decided by Gary on 26 Oct 2026.

| # | Question | Decision |
|---|---|---|
| 1 | The baseline says store timestamps in UTC. D22 stores local standard time. | **Decided (Gary, 26 Oct): keep D22 local standard time.** CIP-02.4 is a documented exception |
| 2 | Adopt the state model in section 7 as the platform's? | **Yes (Gary, 26 Oct).** The application sets READY/MEASURING and requests MAINTENANCE and CALIBRATION; the platform forces FAULT and SAFE_MODE |
| 3 | Quality codes: 8 platform values (valid, stale, unavailable, out-of-range, warming-up, maintenance, calibration, fault) | **Yes (Gary, 26 Oct).** The data log keeps OK/FLAG/BAD/N/A as a summary so existing CSV readers still work |
| 4 | Credential recovery by developer-signed token | **Yes (Gary, 26 Oct).** It reuses the serial number mechanism |
| 5 | EMC basis IEC 61326-1 (industrial) | **Yes (Gary, 26 Oct)**, to be confirmed against each customer specification |

## 7. State model (agreed)

| State | Entered when | Readings get quality | Allowed | Refused |
|---|---|---|---|---|
| STARTING | Reset | unavailable | nothing | everything |
| INITIALISING | Settings loaded, self-tests running | unavailable | view | writes |
| WARMING_UP | Product says it is warming up | warming-up | view, settings, firmware restart | calibration |
| READY | Self-tests passed, not measuring | unavailable | everything | |
| MEASURING | Product measuring | valid (or the value's own status) | settings by level, firmware upload; restart only if `fw_safe()` | |
| MAINTENANCE | Operator selects it (touchscreen or web) | maintenance | settings, outputs by hand, firmware restart | |
| CALIBRATION | Calibration started | calibration | calibration writes | firmware restart, restore defaults |
| FAULT | CAL_INVALID, a fault-severity alarm the product marks as stopping measurement | unavailable or fault | view, acknowledge, service | measurement |
| SAFE_MODE | Reboot loop (CIP-08.5) | unavailable | view, firmware update, service | application logic |

## 8. Gaps by owning thread

Ordered by what protects the three principles first. Sent to the owning threads on 26 Oct
after Gary's review.

**Firmware (CORE)**
1. Quality status on live values, stale detection, quality enum (CIP-10) and the state model (CIP-09).
2. Supply sense and power-fail shutdown (CIP-07.3, 07.5).
3. Reboot-loop detection and safe mode (CIP-08.5).
4. Calibration record with factory/field split and history (CIP-18), and its events (CIP-17.6).
5. Clock quality: synchronised, last sync, source (CIP-02.6).
6. Modbus identity block, heartbeat and last-good-communication registers, map version (CIP-01.6, 15.1, 15.2, 14.4).
7. Hardware revision straps, build ID, image header with hardware range (CIP-01.3, 25.1, 03.2).
8. Operating hours, service log, test result record, configuration checksum (CIP-22.2, 22.4, 23.4, 05.4).
9. Credential recovery token, calibration permission, stronger web password hashing (CIP-19.1, 19.3, 20.3).
10. Start-up self-test list and published fault code list (CIP-11.1, 11.3).
11. First-stage bootloader and secure boot on the H573 (CIP-04.1, 20.7); production debug state (with PCB).
12. Power-off gap rows in the data CSV (CIP-16.6).

**HMI (touchscreen)**
1. Quality and state shown on values and in the header (CIP-21.2, 21.4).
2. Supervisory link indicator (CIP-21.5).
3. Maintenance mode and calibration screens once the firmware side exists.

**HTTPS browser**
1. Quality and state on values (CIP-21.2, 21.4); API version in `/api/info` (CIP-14.4).
2. Diagnostic package download (CIP-22.1).
3. Service log and factory test pages (CIP-22.4, 23.3).

**Core PCB**
1. Confirm the revision straps and supply-sense divider are on the schematic and documented for the firmware (CIP-01.3, 07.3).
2. EMC and environmental design basis with target levels (CIP-24).
3. Production debug lock and recovery route, with the firmware thread (CIP-04.4, 20.4).

**Platform governance (Firmware thread, section 9)**
1. `make conformance APP=<dir>`, start-up contract checks, requirement IDs in tests, interface snapshots.
2. Core version tags and the core version shown beside the product version; `PROFILE.md` template.

**Documentation (any thread)**
1. Operator manual, service manual, Modbus register map document (CIP-26.7).
2. `CHANGELOG.md`, release notes, supported-combinations matrix (CIP-25.4, 25.5).
3. Firmware update and recovery procedures (CIP-03.1, 04.4); interface framework guide (CIP-13.5); UI convention page (CIP-21.7).

## 9. Keeping the core common across the product family

Gary asked (26 Oct): once instrument-specific models exist, how do the core items stay true
for every instrument without slowing each product down? The answer is to keep the core as
one shared, versioned layer that products build on and never edit. Products then differ
only in what the core lets them choose. The rules below make that hold.

### 9.1 One core, many applications

| Rule | How |
|---|---|
| R1. The core is one code base, shared by every product | `platform/`, `port/`, `web/` and the core board stay in this repository. Each instrument is an application directory (`app_<product>/`, as `app_demo/` and `app_vmmux/` are today) built with `APP=<dir>`. A product in its own repository pulls the core in as a git submodule pinned to a core release tag |
| R2. Products never edit the core | Already integration rule 6 (`PLATFORM.md` 6, `APP_INTEGRATION.md`). A product that needs something new asks the core thread for it, and the core adds it once for every product. A product-side patch to `platform/` is refused at review |
| R3. The application contract is the only interface | `plat_app_t` / `scan_app_t`, `param.h`, `plat_alarm.h`, `plat_log.h`, `ui_app.h`, `iobus.h`. Product code reaches the hardware, FreeRTOS and the network only through them |
| R4. The core is released with semantic versions | `core-MAJOR.MINOR.PATCH` tags. MINOR adds features and keeps every product building and behaving the same. MAJOR may change the contract and comes with a migration note. Each product states which core version it is built on, shown on the System screen and `/api/info` next to its own version |
| R5. Reserved number ranges | Parameters 1-999, alarms 1-99, the Modbus identity, status and heartbeat block, and the platform's event codes belong to the core. They mean the same thing in every product. Products use parameters from 1000 and alarms from 100 |

### 9.2 What a product may choose, and what it may not

Each product has a short **product profile** (`app_<product>/PROFILE.md`). It lists:

- the **O** (optional) items it fits, such as the touchscreen, the I/O network, alarm email,
  relays and log flash size;
- its measurement channels, with the quality behaviour of each;
- its states (when it is warming up, measuring, in calibration);
- any **exception** to an **M** item, with the reason and Gary's sign-off. CIP-02.4 (local
  standard time) is the pattern.

**M** items cannot be switched off. The core builds them in, so a product gets them by
building on the core, and leaving one out takes deliberate work that review will see. The
product profile is the only place an exception is recorded.

### 9.3 Automatic checks

These checks keep the core items true without anyone re-reading this spec for every release.

| Check | What it catches | When |
|---|---|---|
| **Conformance suite** (`make conformance APP=<dir>`): the shared tests of CIP-26 (start-up, power cut, corrupted settings, comms loss, update failure) run against each product build, not only the demo | A product that breaks a core behaviour, such as an application that blocks the scan so the watchdog fires | Every pull request, for every product in the repository |
| **Start-up contract checks** (like `ui_app_check()` today): the core refuses to start a product whose tables break the rules, for example a parameter or alarm ID in the core's range, a measured value with no quality source, or a missing state hook | Contract mistakes, caught on the bench the first time the product runs | At every start-up; also in the host tests |
| **Requirement IDs in tests**: each test names the `CIP-nn.m` it proves. A script lists any **M** requirement with no test | An M item quietly losing its test | Every pull request |
| **Interface snapshots**: the Modbus register map and `/api/*` are generated from the tables and compared with the last release | A core address or field moved or removed by accident (CIP-14.5) | Every pull request; a difference needs a MINOR or MAJOR version bump |
| **Compatibility matrix** (CIP-25.5) | A product released on a core version or board revision that was never tested together | Every release |

### 9.4 Changing a core item

1. The change goes to this spec first: a new requirement, a change to one, or a new exception.
   The spec revision goes up.
2. The core thread builds it once, with a test that names the requirement.
3. The conformance suite runs it against every product. A product that breaks gets fixed in
   the same pull request, or the change waits.
4. The release notes say which products need anything done to take the new core version
   (usually nothing).

This keeps products agile. A product team only writes its measurement, its screens and its
profile, takes core improvements by changing a version tag, and gets told by the checks if it
has stepped outside the common platform.
