# Core PCB (KiCad 7)

The CEMS core board schematic, rev 0.1 (draft for review). Design spec, pin map and
decisions: [`docs/CORE_PCB.md`](../../docs/CORE_PCB.md).

| File | What |
|---|---|
| `cems_core.pdf` | All sheets, for review without KiCad |
| `cems_core.kicad_pro`, `cems_core.kicad_sch` | KiCad project and root sheet |
| `power`, `mcu`, `eth_client`, `eth_io`, `periph`, `io_dig`, `io_pwr`, `io_ana` `.kicad_sch` | The eight sheets (rev 0.2: 421 parts, 305 nets) |
| `cems_core.kicad_sym`, `sym-lib-table` | Project symbols (the MB85RC1MT FRAM) |
| `bom.csv` | Grouped bill of materials |
| `tools/design.py` | **The circuit, as data: edit this, not the sheets** |
| `tools/gen_schematic.py` | Writes the sheets and `bom.csv` from `design.py` |
| `cems_core.kicad_pcb`, `cems_core.kicad_dru`, `cems_core.pretty/` | Layout start (block plan, unrouted), isolation rules, project footprints. CORE_PCB.md 10a |
| `tools/gen_pcb.py`, `tools/gen_footprints.py` | Write the board and the project footprints from `design.py` |
| `tools/project_symbols.py` | Pin tables of the six I/O chips missing from KiCad (all CHECK) |
| `tools/check_netlist.py` | Checks KiCad's exported netlist against `design.py`, pin by pin |
| `tools/build.sh` | Runs all three and exports the PDF |

Needs KiCad 7 (`kicad-cli` and its symbol libraries; Ubuntu: `apt install kicad
kicad-symbols kicad-footprints`) and `pip install kiutils`. Footprints are the KiCad 7
library names, so the netlist imports straight into Pcbnew for layout.

Once layout starts, the schematic may be tidied by hand in KiCad (wires in place of
labels). From then on it is the source, and `design.py` is kept only as a record.
