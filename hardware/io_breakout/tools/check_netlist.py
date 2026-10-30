#!/usr/bin/env python3
"""Check KiCad's own netlist of the schematic against design.py.

    kicad-cli sch export netlist --format kicadxml -o io.xml hardware/io_breakout/cems_io_breakout.kicad_sch
    python3 hardware/io_breakout/tools/check_netlist.py io.xml

Every pin listed in design.py must be on a net with exactly the design's members, every
other pin must be unconnected, and every part must carry its footprint. Also lists the
nets with a single pin, which are almost always a mistake.
"""
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import design  # noqa: E402


def main(path):
    root = ET.parse(path).getroot()
    errors = []

    comps = {c.get("ref"): c for c in root.iter("comp")}
    want_parts = {p["ref"]: p for p in design.PARTS}
    for ref in sorted(set(want_parts) ^ set(comps)):
        errors.append(f"part {ref}: in {'design only' if ref in want_parts else 'schematic only'}")
    for ref, p in want_parts.items():
        c = comps.get(ref)
        if c is None:
            continue
        fp = c.findtext("footprint") or ""
        if fp != p["footprint"]:
            errors.append(f"{ref}: footprint {fp!r}, want {p['footprint']!r}")
        if (c.findtext("value") or "") != p["value"]:
            errors.append(f"{ref}: value {c.findtext('value')!r}, want {p['value']!r}")

    # nets as KiCad sees them: name -> set of (ref, pin)
    got = {}
    for n in root.iter("net"):
        name = n.get("name")
        members = {(m.get("ref"), m.get("pin")) for m in n.iter("node")}
        got[name] = members
    pin_net = {}
    for name, members in got.items():
        for m in members:
            pin_net[m] = name

    want = {}
    for p in design.PARTS:
        for pin, net in p["pins"].items():
            want.setdefault(net, set()).add((p["ref"], pin))

    def short(name):
        return name.rsplit("/", 1)[-1]

    for net, members in sorted(want.items()):
        names = {pin_net.get(m) for m in members}
        if len(names) != 1 or None in names:
            errors.append(f"net {net}: split over {sorted(str(x) for x in names)}")
            continue
        kname = names.pop()
        if short(kname) != net:
            errors.append(f"net {net}: KiCad calls it {kname}")
        extra = got[kname] - members
        if extra:
            errors.append(f"net {net}: extra pins {sorted(extra)}")
        if len(members) == 1:
            errors.append(f"net {net}: only one pin {sorted(members)}")

    listed = {(p["ref"], pin) for p in design.PARTS for pin in p["pins"]}
    for name, members in got.items():
        for m in members - listed:
            if not name.startswith("unconnected-") and len(members) > 1:
                errors.append(f"pin {m}: on {name} but not in the design")

    n_pins = sum(len(p["pins"]) for p in design.PARTS)
    print(f"{len(comps)} parts, {len(want)} nets, {n_pins} connected pins checked")
    for e in errors:
        print("ERROR", e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
