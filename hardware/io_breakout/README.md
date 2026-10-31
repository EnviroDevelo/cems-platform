# I/O breakout board (KiCad 7)

A small DIN-rail board that takes the main board's **one 40-way ribbon** and breaks out
all the digital I/O to pluggable terminals: the 8 valve outputs (bank A) and the 8 digital
inputs. It is rev 0.2, a draft for review.

Rev 0.2 (Gary, 31 Oct 2026) replaces the two 26-way ribbons and two boards of rev 0.1.
- With only one header, no ribbon can go into the wrong slot.
- The main board saves a header and the field needs one breakout instead of two.

The board-level decisions are in the doc
[One PCB for CORE and I/O board](https://claude.ai/artifact/5oGgPyMKgN8YEAyrkMxLkh).

## Ribbon (Gary's rule: off the shelf, nothing special)

- **Cable:** standard 40-way 1.27 mm flat cable (28 AWG) with 2.54 mm IDC sockets, wired straight through, pin 1 to pin 1.
- **Headers:** both ends use a shrouded box header with ejector latches. The shroud notch is the only keying.
- **Pinout:** the main board's J10 has the same pinout, so any standard cable is correct.

| Pins | Signal | Why |
|---|---|---|
| 1, 3, … 15 | OUT1 … OUT8 (valves, 24 V, 0.6 A) | Each output conductor sits beside its own 0 V return |
| 2, 4, … 16 | 0V | |
| 17, 19, … 31 | IN1 … IN8 (24 V inputs) | Each input beside a 0 V |
| 18, 20, … 32 | 0V | |
| 33, 35, 37, 39 | +24V | Sensor supply and LEDs, fused 0.75 A on the main board |
| 34, 36, 38, 40 | 0V | 20 × 0 V in all |

A 28 AWG conductor and an IDC contact are each good for about 1 A. Bank A's worst case
is 8 × 0.6 A = 4.8 A, which returns over 20 × 0 V conductors (about 0.25 A each).

## Terminals: two-tier pluggable, 3.81 mm

Each channel is one column, with the upper tier over the lower tier.

| Ref | Upper tier | Lower tier |
|---|---|---|
| J2 VALVES 1-8 | OUT1 … OUT8 | 0V |
| J3 INPUTS 1-8 | IN1 … IN8 | +24V |
| J4 SENSOR 0V (single tier) | 0V × 8, for 3-wire sensors | |

How each field device wires in:
- **Valve:** both terminals of its column on J2.
- **Contact or 2-wire sensor:** both terminals of its column on J3.
- **3-wire sensor:** as a 2-wire sensor, plus 0 V from J4.

## Per channel

- **Valve LED:** 10 kΩ + 1N4148W + green LED from OUTn to 0 V. The series diode protects the LED from the coil's negative turn-off spike.
- **Flyback clamp:** S1M from OUTn back to 0 V through RZ (0 Ω).
  - It keeps the spike at the valve end and off the ribbon, which helps EMC.
  - It adds about 10-30 ms to the valve's release.
  - The ISO2H823V2.5 clamps internally, so nothing fails without it.
- **Fast release (option):** remove RZ and fit Z, a 24 V 1.5 W Zener. It clamps at about Vz + 0.7 V below 0 V.
- **Input LED:** 10 kΩ + yellow LED from INn to 0 V. It is lit while the contact is closed.
- **Wire-break bias:** the bias resistor goes across the contact **at the sensor**. The silkscreen says so.

Also on the board:
- a power LED on +24V
- four M3 holes for a standard 50 × 80 mm DIN-rail PCB holder

## Board

- 50 × 80 mm, 2-layer, 1.6 mm FR-4, 2 oz copper.
- J2 sits on one long edge, and J3 + J4 on the other.
- 0 V is a wide pour.

## Still to check (CHECK in the sheet notes)

- **Two-tier header:** the Phoenix MCDV 1,5/8-G-3,81 (or the Degson/Dinkle equivalent) part number. Its footprint in `cems_io_breakout.pretty` is a **placeholder**, to redraw from the maker's drawing.
- **40-way box header:** confirm Würth 61204022821 (WR-BHD, long latches).
- **Fast-release Zener:** Vz + 0.7 V must stay below the ISO2H823V2.5's own demagnetisation clamp.
- **Input LED and wire-break:** each input LED draws about 2 mA in parallel with the input, so check that it doesn't mask the ISO1I813T wire-break threshold. If it does, fit a larger LED resistor.
- **DIN holder:** the hole positions.
- **SMC valves:** whether they come with a built-in surge suppressor. If they do, D is not needed.

## Files

| File | What |
|---|---|
| `cems_io_breakout.pdf` | Both sheets, for review without KiCad |
| `cems_io_breakout.kicad_pro`, `.kicad_sch`, `connectors.kicad_sch`, `channels.kicad_sch` | KiCad project |
| `cems_io_breakout.pretty/` | Project footprint (two-tier header, placeholder) |
| `bom.csv` | Grouped bill of materials |
| `tools/design.py` | **The circuit, as data: edit this, not the sheets** |
| `tools/gen_schematic.py`, `tools/check_netlist.py`, `tools/build.sh` | Same generator and check as `hardware/core_pcb` |

Run `tools/build.sh` (KiCad 7 and `pip install kiutils`). It currently reports 74 parts,
51 nets and 212 connected pins, with the check passing.
