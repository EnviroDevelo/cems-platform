#!/bin/sh
# Regenerate the I/O breakout schematic, check KiCad's netlist against design.py, export the PDF.
# Needs KiCad 7 (kicad-cli, symbol libraries) and the Python package kiutils.
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_schematic.py
kicad-cli sch export netlist --format kicadxml -o cems_io_breakout.xml cems_io_breakout.kicad_sch >/dev/null
python3 tools/check_netlist.py cems_io_breakout.xml
kicad-cli sch export pdf -o cems_io_breakout.pdf cems_io_breakout.kicad_sch >/dev/null
rm -f cems_io_breakout.xml
echo "OK: cems_io_breakout.pdf, bom.csv"
