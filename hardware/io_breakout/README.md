# I/O breakout board (KiCad 7)

A small DIN-rail board that takes one standard 26-way ribbon from the main board and
breaks it out to screw terminals. It is our own version of the generic IDC26P-B board
(Gary, 30 Oct 2026: the Phoenix module, about AUD 230, is not viable). Rev 0.1, draft
for review. The board-level decisions are in the doc
[One PCB for CORE and I/O board](https://claude.ai/artifact/5oGgPyMKgN8YEAyrkMxLkh).

## One design, two builds

| Build | Used for | Fitted per channel | Not fitted |
|---|---|---|---|
| **OUT** | Valve bank A: 8 × 0.6 A high-side outputs (ISO2H823V2.5 on the main board) | LED, R, DS, flyback diode D, RZ (0 Ω) | Z, RW |
| **OUT, fast release** | Valves whose closing time matters | As OUT, but Z instead of RZ | RZ, RW |
| **IN** | 8 digital inputs (ISO1I813T on the main board) | LED, R, DS | D, RZ, Z; RW only for wire-break on dry contacts |

The schematic's fitted/not-fitted flags are the OUT build. Mark the build on the
silkscreen box at assembly.

## Ribbon (Gary's rule: off the shelf, nothing special)

Standard 26-way 1.27 mm flat cable (28 AWG) with 2.54 mm IDC sockets, wired straight
through, pin 1 to pin 1. Both ends use a shrouded box header with ejector latches; the
shroud notch is the only keying. The main board's header uses **the same pinout**, so
any standard cable is correct.

| Pins | Signal | Why |
|---|---|---|
| 1, 3, 5, … 15 | CH1 … CH8 | Each channel conductor sits beside a 0 V conductor |
| 2, 4, 6, … 16 | 0V | Return beside each channel |
| 17, 19, 21, 23, 25 | +24V | Sensor supply for the inputs (5 conductors) |
| 18, 20, 22, 24, 26 | 0V | 13 × 0 V in all |

A 28 AWG conductor and an IDC contact are each good for about 1 A. Bank A's worst case,
8 × 0.6 A = 4.8 A, returns over 13 × 0 V conductors (about 0.4 A each). The main board
fuses the +24 V it sends down the ribbon (1 A).

## Terminals

Two 12-way 5.08 mm screw terminals, three positions per channel: **CHn, 0V, +24V**.
- Valve: CHn and 0V.
- Contact or 2-wire sensor: +24V and CHn.
- 3-wire sensor: all three.

J2 carries CH1-CH4 and J3 carries CH5-CH8, on the two long edges, about 61 mm each. The
silkscreen shows the channel numbers and has a blank line beside each channel for the
valve or signal name.

## Per channel

- **Status LED:** 10 kΩ + 1N4148W + green LED from CHn to 0 V. It draws 2.2 mA at
  24 V, so it is lit when an output is on or an input contact is closed. The series
  diode protects the LED's 5 V reverse rating from a coil's negative turn-off spike.
- **Flyback clamp (OUT):** S1M from CHn back to 0 V through RZ (0 Ω). This is a plain
  diode clamp: it keeps the spike at the valve end and off the ribbon, which helps EMC,
  and adds about 10-30 ms to the valve's release. The ISO2H823V2.5 clamps internally,
  so nothing fails without it.
- **Fast release (option):** remove RZ and fit Z, a 24 V 1.5 W Zener. It clamps at
  about Vz + 0.7 V below 0 V, so the coil's energy is dumped faster. The diode is
  reverse-biased while the output is on, so the main board's open-load check still
  works.
- **Wire-break bias (IN, option):** RW from +24V to CHn, fitted across a dry contact so
  the ISO1I813T can tell an open contact from a broken wire. Its value is set from the
  ISO1I813T datasheet.

Also on the board: a power LED on +24V, and four M3 holes for a DIN-rail PCB carrier or
clips.

## Board

- 51 × 85 mm, 2-layer, 1.6 mm FR-4, 2 oz copper.
- 0 V as a wide pour.
- Silkscreen: pin 1, the channel numbers, a valve-name line per channel, the build (OUT
  or IN) and the revision.

## Still to check (CHECK in the sheet notes)

- **Box header:** the maker's part number (Würth, Amphenol or TE), latched and 1 A per
  contact.
- **Screw terminals:** the Degson or Dinkle part number, rated UL 300 V, 8 A or more, and
  its footprint against the Phoenix MKDS 5.08 mm one used here.
- **Fast-release Zener:** Vz + 0.7 V must stay below the ISO2H823V2.5's own
  demagnetisation clamp, so the external clamp takes the energy.
- **RW value:** from the ISO1I813T wire-break threshold.
- **DIN carrier:** the carrier or clip part, which sets the hole positions.
- **SMC valves:** whether they come with a built-in surge suppressor. If they do, D is
  not needed.

## Files

| File | What |
|---|---|
| `cems_io_breakout.pdf` | Both sheets, for review without KiCad |
| `cems_io_breakout.kicad_pro`, `.kicad_sch`, `connectors.kicad_sch`, `channels.kicad_sch` | KiCad project |
| `bom.csv` | Grouped bill of materials (OUT build) |
| `tools/design.py` | **The circuit, as data: edit this, not the sheets** |
| `tools/gen_schematic.py`, `tools/check_netlist.py`, `tools/build.sh` | Same generator and check as `hardware/core_pcb` |

Run `tools/build.sh` (KiCad 7 and `pip install kiutils`). It currently reports 65 parts,
35 nets and 166 connected pins, with the check passing.
