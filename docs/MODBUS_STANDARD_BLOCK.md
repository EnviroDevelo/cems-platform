# Standard CEMS Modbus block (VDI 4201-3)

Rev A, 30 Oct 2026. Build specification for the core's fixed Modbus block at registers
7000-7999. Background, research and the decision trail are in the design doc
"CEMS Modbus standard map and Excel handover" (https://claude.ai/artifact/NiAsZJC428qVbjdHsoVVTR).

**Source of the structure.** VDI 4201 Part 3 (Modbus) with Part 1 (general requirements) is the
industry standard for the interface between an emission measuring system and a DAHS (cited by
EN 17255 and EN 15267-3). The guideline text is sold only by DIN Media and has not been read.
The structure below is taken from two free manufacturer implementations:

- ABB, Advance Optima AO2000 Modbus manual, section 5 "Modbus in accordance with VDI 4201 Part 3"
  (COM_AO2000_MODBUS_EN_G).
- SICK / Endress+Hauser, DUSTHUNTER SP30/SB30 Modbus protocol implementation, V2-1, 2026-02
  (8030782_1VBQ_TI).

Open until the guideline is bought (before QAL1 certification): whether BasisM values must be in
engineering units (ABB) or scaled to the range (SICK shows -10000..+10000), how long a DAHS
simulation may last, QAL3 test-cycle data (VDI 4201-3 section 6.5). This spec uses engineering
units (ABB).

## 1. Zones

| Zone | Tables | 0-based | Owner |
| --- | --- | --- | --- |
| Project area | all | 0-6999 | the application (`mb_map_t`) |
| Standard CEMS block (this document) | input registers; read-only mirror in holding registers | 7000-7999 | core |
| Reserved for Modbus test bits (none defined) | holding, coils | 8000-8199, 8000-8099 | core |
| Platform block | input registers | 9000-9199 | core (`modbus.h`) |

An application entry overlapping 7000-7999 in the input or holding registers stops start-up,
as for the platform block.

**Holding mirror rule.** Function 03 on holding registers 7000-7999 returns exactly what function
04 returns on the same input registers. A write (06, 16) anywhere in holding 7000-7999 answers
exception 02 (read-only), with no exceptions (BasisS included, section 4).

**Formats.** 0-based addresses. Floats IEEE 754, u32 and floats high word first (the lower
address holds the high word), unless the admin word-order setting (7944) says otherwise; the
setting applies to this whole block. u16 one register.

**Slots.** Slot n (0-31) is a fixed parameter (section 6) and is the emissions engine's channel
index (`emis.h`, PR #15). An unfitted slot reads NaN (0x7FC00000) with status 0 and quality
unavailable; no address in the block ever gives exception 02 for a read.

## 2. Map

| Registers | Content | Format | Source |
| --- | --- | --- | --- |
| 7000 + 4n | BasisM: slot n value as measured (bank 0) | float | param 450[n] |
| 7002 + 4n | BasisM: slot n status (section 3) | u32 | built from 460[n], 466[n], forcing |
| 7128 + 4n | BasisS: slot n simulation value | float | force in effect (section 4) |
| 7130 + 4n | BasisS: slot n simulation request | u32 | force in effect (section 4) |
| 7256 | BasisR: reference material status | u32 | 0 until defined |
| 7258 | BasisR: reference material initiate | u32 | reads 0; write = exception 02 until defined |
| 7260-7299 | reserved | | |
| 7300 + 64(k-1) + 2n, k = 1..7 | calculated bank k, slot n | float | param 450+k [n] |
| 7748 + n | quality, measured value | u16 PLAT_Q_* | 460[n] |
| 7780 + n | quality, instantaneous calculated (banks 1-3) | u16 | 461[n] (mass: worst of 461, 462) |
| 7812 + n | quality, short-term averages (banks 4-6) | u16 | worst of 463[n], 465[n] |
| 7844 + n | quality, daily average (bank 7) | u16 | 464[n] |
| 7876 + n | short-term averages discarded today | u16 | 467[n] |
| 7908 + n | days invalid this year | u16 | 468[n] |
| 7940 | block version (1) | u16 | |
| 7941 | slots fitted, bit n = slot n | u32 | measurement table |
| 7943 | values forced now (same as 9090) | u16 | 334 `mb_forced` |
| 7944 | float word order in this block: 0 high word first, 1 low word first | u16 | admin setting |
| 7945 | stream on line (0 = single-stream product) | u16 | application |
| 7946 | plant operating | u16 | 422 |
| 7947 | reference diluent: 0 none, 1 O2, 2 CO2 | u16 | 403 |
| 7948 | reference diluent, % | float | 404 |
| 7950 | normalisation temperature, K | float | 401 |
| 7952 | normalisation pressure, kPa | float | 402 |
| 7954 | short-term averaging period, s | u16 | 416 |
| 7955 | emissions engine enabled | u16 | 400 |
| 7956 | emissions engine configuration error | u16 | 423 |
| 7957-7999 | reserved | | |

Banks: 1 QAL2-calibrated, dry, normalised mg/Nm3 (273.15 K, 101.325 kPa), not O2-corrected
(O2, H2O, CO2 in % vol dry); 2 bank 1 corrected to the reference diluent; 3 mass emission kg/h;
4 running short-term average of bank 2; 5 last short-term average (not validated); 6 last
validated short-term average (confidence interval subtracted); 7 last validated daily average.
A bank that does not apply to a slot reads NaN with its quality unavailable.

Registers are only ever added, never moved; an addition raises 7940 (CIP-14.5).

## 3. Status word (BasisM, u32)

Bits 0-15 per VDI 4201-3 (NAMUR NE 107); bits 16-31 are left to the maker by VDI 4201-3.

| Bit | Meaning | Set when |
| --- | --- | --- |
| 0 | Failure | measured quality fault or unavailable |
| 1 | Maintenance (function check) | quality maintenance, calibration or warming up; reference material applied |
| 2 | Maintenance request | platform alarm 21 CAL_DUE, or bit 27 |
| 3 | Out of specification | quality out of range or stale; bit 18 or 19 |
| 4 | Test / simulation value transmitted | the slot is forced from the web page or touchscreen |
| 5-15 | reserved (0) | |
| 16 | Forced from the web page or touchscreen | `mb_force_active(id, index)` |
| 17-27 | Emissions engine status bits 5-15 (466[n] >> 5 << 17): 17 no QAL2, 18 above valid calibration range, 19 analyser over-range, 20 O2 too high to correct, 21 calculation input missing, 22 average incomplete, 23 day invalid, 24 confidence interval not applied, 25 fixed H2O used, 26 below range, 27 QAL2 review due | |
| 28-31 | reserved (0) | |

## 4. Simulation (BasisS)

**Read-only (Gary, 30 Oct: forcing from the web page and touchscreen only).** The DAHS cannot
write simulation values: a write anywhere in BasisS answers exception 02, like the rest of the
block. This is a known deviation from VDI 4201-3 and is listed as such in the PROFILE.md and the
Excel handover file.

Each slot reads the force in effect: 7128 + 4n the forced value (0 when none), 7130 + 4n the
request, bit 4 set while the slot is forced (and bits 0-3 as the forced status, normally 0).
So a VDI-capable DAHS sees the same thing it would after its own simulation write.

## 5. Device identification (function 43/14)

Conformity level 0x83 (extended, individual access). Existing basic and regular objects stay.

| Object | Name | Encoding | Value |
| --- | --- | --- | --- |
| 0x00 | VendorName | string | as now |
| 0x01 | ProductCode | string | as now |
| 0x02 | MajorMinorRevision | string | as now |
| 0x04 | ProductName | string | product name |
| 0x80 | SerialNumber | string | serial number (param 50) as decimal text |
| 0x81 | ComponentNumber | word | 30 |
| 0x82 | BasisM | word | 7000 |
| 0x83 | BasisS | word | 7128 |
| 0x84 | BasisR | word | 7256 |
| 0x85 + 4n | Component n+1 name | string | slot name (section 6), or "not fitted" |
| 0x86 + 4n | Component n+1 range start | float | measurement lo |
| 0x87 + 4n | Component n+1 range end | float | measurement hi |
| 0x88 + 4n | Component n+1 unit | string | unit text |

Words big-endian, floats IEEE 754 big-endian (4 bytes), strings ASCII without terminator.
ComponentNumber is 30, not 32: object IDs are one byte, and component 31 would need 0x101.
Slots 30 and 31 are spare, so nothing is lost; the BasisM stride still covers 32 slots.

## 6. Slots

| n | Name (43/14) | Default unit | n | Name (43/14) | Default unit |
| --- | --- | --- | --- | --- | --- |
| 0 | O2 dry | % vol | 16 | Hg | ug/m3 |
| 1 | O2 wet | % vol | 17 | Dust | mg/m3 |
| 2 | CO2 | % vol | 18 | Flue gas flow | m3/h |
| 3 | CO | mg/m3 | 19 | Flue gas velocity | m/s |
| 4 | NO | mg/m3 | 20 | Stack temperature | degC |
| 5 | NO2 | mg/m3 | 21 | Stack pressure abs | hPa |
| 6 | NOx as NO2 | mg/m3 | 22 | Sample flow | l/min |
| 7 | SO2 | mg/m3 | 23 | Probe/line temperature | degC |
| 8 | HCl | mg/m3 | 24 | Cooler temperature | degC |
| 9 | HF | mg/m3 | 25 | Analyser cell temperature | degC |
| 10 | NH3 | mg/m3 | 26 | Cabinet temperature | degC |
| 11 | H2O | % vol | 27 | Air/cal gas pressure | kPa |
| 12 | TOC as C | mg/m3 | 28 | Opacity | % |
| 13 | CH4 | mg/m3 | 29 | TRS | mg/m3 |
| 14 | N2O | mg/m3 | 30 | spare | |
| 15 | H2S | mg/m3 | 31 | spare | |

The unit is the default; a product may report another unit (for example CO in ppm), and the
43/14 unit object and the Excel handover file then say so. The slot always means the same
quantity.

Modicon addresses: input 30001 + address, holding 40001 + address. O2 dry value: 37001 (input)
or 47001 (holding).
