# Core board: EMC and environmental design basis (CIP-24)

Rev 0.1, 26 Oct 2026. The standard and the conditions the core board is designed and tested
to. Agreed by Gary: **IEC 61326-1, industrial electromagnetic environment**. The circuit
measures referred to here are on the schematic (`hardware/core_pcb/`) and in
`CORE_PCB.md`. The levels are those of IEC 61326-1:2020 Table 2 **(check against the
purchased standard before the test plan is written)**.

## 1. Immunity (IEC 61326-1, Table 2, industrial)

| Port | Test | Level | Criterion | How the board meets it |
|---|---|---|---|---|
| Enclosure | ESD, IEC 61000-4-2 | ±4 kV contact, ±8 kV air | B | Chassis plane at the connectors; RJ45 shells and FE to chassis; ESD arrays (U5, U7) on both PHYs; chassis to logic ground through R11/C15. The display front is the exposed surface: see 1.1 |
| Enclosure | Radiated RF, IEC 61000-4-3 | 10 V/m 80 MHz-1 GHz; 3 V/m 1.4-2 GHz; 1 V/m 2-6 GHz; 80 % AM 1 kHz | A | 4-layer board with a solid ground plane; series resistors on fast lines; the FFC kept at 150 mm or less |
| Enclosure | Power-frequency magnetic field, IEC 61000-4-8 | 30 A/m | A | No magnetic sensors on the board |
| DC power (24 V) | Fast transients, IEC 61000-4-4 | ±2 kV, 5 kHz | B | Y-capacitors C16/C17 to chassis, common-mode choke L3, 47 µF bulk, TVS D2 |
| DC power (24 V) | Surge, IEC 61000-4-5 | ±0.5 kV line to line, ±1 kV line to earth | B | 3 kW TVS SMDJ33CA (D2) line to line ahead of the choke, 60 V-rated buck (U1) behind it, fuse F1, Schottky D1; line to earth through the Y-capacitors' 2 kV rating |
| DC power (24 V) | Conducted RF, IEC 61000-4-6 | 3 V, 150 kHz-80 MHz | A | L3 with C16/C17 |
| Ethernet (both) | Fast transients, IEC 61000-4-4 | ±1 kV | B | MagJack isolation, Bob Smith termination to chassis, shield to chassis |
| Ethernet (both) | Surge, IEC 61000-4-5 | ±1 kV line to earth (only for cables over 30 m or leaving the building) | B | 1500 Vrms magnetics isolation (2.1 kV peak) and the ESD arrays on the PHY side |
| Ethernet (both) | Conducted RF, IEC 61000-4-6 | 3 V, 150 kHz-80 MHz | A | Magnetics common-mode rejection, shield to chassis |
| Functional earth | Fast transients, conducted RF | ±1 kV; 3 V | B; A | FE goes only to the chassis plane |

### Performance criteria, as they apply to a CEMS

- **A:** normal operation within specification during and after the test: measurement,
  logging, the display and both networks carry on; no value outside its accuracy.
- **B:** a temporary effect allowed during the test, recovering by itself without an operator:
  a dropped Ethernet frame (Modbus TCP retries), a display flicker, a touch ignored. Not
  allowed even under B: a reset, a lost or changed setting, a lost or corrupted log record,
  a changed output on an I/O board, a false alarm that latches.
- Any reset during a test is recorded in the event log (the reset cause), so the test lab's
  logs and the instrument's agree.

### 1.1 Display front (ESD)

The RVT70's cover glass is the surface people touch, and it sits in the panel door away from
the core board. Its ESD path is Riverdi's design (the glass and the module frame); the core
board protects its end with 33 Ω series resistors on SCK and MOSI and decoupling at J4.
**Risk:** an air discharge to the glass edge can reach the FFC. Pre-compliance test it on the
first prototype in its door. If it resets, add a TVS array on the FFC lines at J4 (an
ESD5V3U4 class part; the footprint can be added at layout) and bond the display's metal
back to the door.

## 2. Emissions

| Test | Standard | Limit |
|---|---|---|
| Radiated, 30 MHz-1 GHz | CISPR 11, group 1 | Class A (industrial) |
| Conducted, DC power port | CISPR 11, group 1 | Class A |

The sources are the two bucks (about 600 kHz and 2.25 MHz), the 25 MHz crystals, the 50 MHz
RMII clock and the SPI buses up to 30 MHz. Measures: tight buck loops at layout, L3 and the
input capacitors, series resistors at the drivers, ground plane under every clock, no clock
on the board edge.

## 3. Environment

| Item | Design basis | Why |
|---|---|---|
| Operating temperature | -20 to +55 °C ambient inside the panel | The RVT70 display is rated -20 to +70 °C; 15 °C left for its own and the panel's heating. Every core-board part is -40 to +85 °C |
| Storage | -30 to +70 °C | Display -30 to +80 °C; CR2032 cell |
| Humidity | 5 to 95 % RH, non-condensing in operation | Tests: IEC 60068-2-78 damp heat steady state (40 °C, 93 % RH, 4 days), IEC 60068-2-30 damp heat cyclic (55 °C, 2 cycles) |
| Condensation | Occasional condensation inside the panel (stack sites, start-up in winter) | Conformal coating on the core board (acrylic, IPC-CC-830), keeping off connectors, the FFC socket, the battery holder and the RJ45s; no unprotected high-impedance node except the crystals, which are inside the coating |
| Altitude | Up to 2000 m | Standard for IEC 61010-1 |
| Pollution degree | 2 (inside the panel) | IEC 61010-1 |
| Vibration | IEC 60068-2-6, 10-150 Hz, 1 g | Panel-mounted equipment on plant; the coin cell is held by its holder, the RJ45s and J1 are through-hole, and the heavy SMD parts (L1, L3, C1) get adhesive at assembly |

### Ingress protection (IEC 60529)

| Item | IP | Note |
|---|---|---|
| Core board in its DIN housing | IP20 | Inside the instrument cabinet; finger-safe |
| Display front in the panel door | IP65 when mounted with the door gasket | **(check with Riverdi for the uxTouch front seal)** |
| The instrument as installed | Set by the cabinet (IP54 or IP65) | The cabinet is the instrument's enclosure, not this board's |

## 4. Safety context

The board runs from 24 VDC (SELV/PELV supply, provided by the cabinet's power supply) and
has no mains on it. The instrument as a whole falls under IEC 61010-1; the core board's
part in that is the 24 V input protection and the Ethernet isolation.

## 5. What happens next

1. Pre-compliance on the first prototype: ESD (including the display front), EFT and surge
   on the 24 V input, radiated emissions scan.
2. Fix at rev B whatever fails, then formal tests at an accredited lab.
3. The CHECK items in `CORE_PCB.md` that affect EMC: L3's current rating and pad mapping, the
   buck compensation, and the display-front ESD.
