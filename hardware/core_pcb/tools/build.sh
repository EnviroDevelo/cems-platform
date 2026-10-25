#!/bin/sh
# Regenerate the core PCB schematic, check KiCad's netlist against design.py, export the PDF.
# Needs KiCad 7 (kicad-cli, symbol libraries) and the Python package kiutils.
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_schematic.py
kicad-cli sch export netlist --format kicadxml -o cems_core.xml cems_core.kicad_sch >/dev/null
python3 tools/check_netlist.py cems_core.xml
kicad-cli sch export pdf -o cems_core.pdf cems_core.kicad_sch >/dev/null
rm -f cems_core.xml
echo "OK: cems_core.pdf, bom.csv"
