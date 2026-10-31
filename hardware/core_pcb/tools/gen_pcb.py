#!/usr/bin/env python3
"""Start the core board layout: cems_core.kicad_pcb from design.py.

What it does: 4-layer stack-up, the board outline, every footprint with its nets (the same
nets the schematic carries, so KiCad's "Update PCB from schematic" finds nothing to change),
connectors on the housing edges, every other part packed into its block, the net classes
and the isolation clearance rules (cems_core.kicad_dru). What it does not do: routing, or
placement down to the last part. The packing is a block plan for a person to refine.

OUTLINE IS AN ASSUMPTION: Phoenix Contact BC 215,6 (12 modules). The base PCB is taken as
211.5 x 85.1 mm, scaled from the BC 161,6 dimension list (157.5 mm) by the 54 mm width step.
The housing drawing has not been seen; replace OUTLINE once it is (docs/CORE_PCB.md 10).
Why 215,6: the parts' courtyards total about 12,100 mm2 and the edge connectors about
370 mm; the BC 161,6 base PCB (13,400 mm2, 2 x 157 mm of edge) cannot hold that.

    python3 hardware/core_pcb/tools/gen_pcb.py [--force]

Once a person starts placing by hand, the .kicad_pcb is the source and design changes go in
through KiCad's "Update PCB from schematic". So this script refuses to overwrite an
existing board unless given --force.
"""
import json
import os
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import design  # noqa: E402

FP_DIRS = {"cems_core": os.path.join(OUT, "cems_core.pretty")}
KICAD_FP = os.environ.get("KICAD7_FOOTPRINT_DIR", "/usr/share/kicad/footprints")
X0, Y0 = 100.0, 100.0            # board origin on the sheet (mm)
OUTLINE = (211.5, 85.1)          # ASSUMPTION, see above
EDGE = 1.0                       # keep-in from the board edge (mm)
GAP = 0.3                        # gap between courtyards when packing (mm); courtyards already carry 0.25 mm

# Connectors along the housing faces, left to right. Top face: power, Ethernet, USB, field
# supplies, pump, relays, bank B. Bottom face: the analog and temperature terminals, beside
# their isolated islands. IDC headers stand on the board face, set back from the top edge
# (second tier), so the ribbons leave through the top cover.
TOP_EDGE = ["J1", "J2", "J3", "J18", "J13", "J14", "J15", "J16", "J17", "J11"]
BOTTOM_EDGE = ["J19", "J20", "J21", "J25", "J22", "J23", "J24"]  # right-aligned, under io_ana
TIER2 = ["J10", "J12"]

# Blocks, left to right across the band between the two connector rows; each block's width
# is set from its parts' courtyard area at FILL. io_dig starts below the two IDC headers.
ORDER = {"power": ["power"], "core": ["eth_client", "eth_io", "mcu", "periph"], "io_dig": ["io_dig"],
         "io_pwr": ["io_pwr"], "io_ana": ["io_ana"]}
WEIGHT = {"io_ana": 1.12, "core": 1.05}  # extra width where the parts are small and many
BAND = (20.0, 76.0)              # y range between the connector rows (mm)
FILL = 0.62

ISO_CLASSES = {
    "FIELD": ("FIELD_0V", "+24V_FA", "+24V_FB", "+24V_FP", "FA_IN", "FB_IN", "+24V_RA", "+24V_RI", "+24V_REL",
              "+5V_P", "PUMP_PWR", "A_IADJ", "A_OLADJ", "A_VDDIO", "A_VCORE", "A_LEDX", "A_LEDY",
              "IN_TS", "IN_WB", "FA_LED_A", "FB_LED_A"),
    "ISO_T": ("GND_T", "+3V3_T", "+3V3_TA"),
    "ISO_AI": ("GND_AI", "+5V_AI", "+5V_AIA", "+24V_AI", "+13V_AI", "+5V_LVL", "+10V_FLOW", "REF10", "REF10_NR",
               "V10_MON", "LVL_SIG", "FLOW_SIG", "FLOW_ILIM"),
}
FIELD_PREFIXES = ("A_OUT", "B_OUT", "B_CL", "DI", "P_", "REL1_OUT", "REL2_OUT")
FIELD_IN = tuple(f"IN{n}_" for n in range(1, 9))
T_PREFIXES = ("T1_", "T2_", "T3_", "T4_", "T5_", "T6_", "T_SCLK", "T_DIN", "T_CS", "T_DOUT", "T_RST", "T_REF",
              "T_AINCOM", "T_RTN")
AI_PREFIXES = ("AI_SCLK", "AI_DIN", "AI_CS", "AI_DOUT", "AI_ERR", "AI_REG", "AI_REF", "AI_ADJ", "LOOP",
               "AI1_", "AI2_", "AI3_", "AI4_", "AI5_", "AI6_")
POWER = ("+24V", "+24V_IN", "+24V_F", "+24V_D", "0V_IN", "+5V", "+3V3", "GND", "BUCK5_SW", "BUCK3_SW")


def net_class(net):
    for cls, names in ISO_CLASSES.items():
        if net in names:
            return cls
    if net.startswith(FIELD_PREFIXES + FIELD_IN):
        return "FIELD"
    if net.startswith(T_PREFIXES):
        return "ISO_T"
    if net.startswith(AI_PREFIXES):
        return "ISO_AI"
    if net in POWER:
        return "Power"
    return "Default"


def mm(v):
    return pcbnew.FromMM(v)


def load(fpid):
    lib, name = fpid.split(":")
    path = FP_DIRS.get(lib, os.path.join(KICAD_FP, f"{lib}.pretty"))
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        raise SystemExit(f"footprint {fpid} not found")
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def size(fp):
    fp.BuildCourtyardCaches()
    bb = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if bb.GetWidth() == 0:
        bb = fp.GetBoundingBox(False, False)
    return bb


def place_at(fp, x, y):
    """Put the courtyard's top-left corner at (x, y), board mm."""
    bb = size(fp)
    pos = fp.GetPosition()
    fp.SetPosition(pcbnew.VECTOR2I(pos.x + mm(X0 + x) - bb.GetX(), pos.y + mm(Y0 + y) - bb.GetY()))


def wh(fp):
    bb = size(fp)
    return pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())


def edge_row(fps, y_of, report):
    x = EDGE
    for fp in fps:
        w, h = wh(fp)
        place_at(fp, x, y_of(h))
        x += w + GAP
    report.append(x - GAP)


def pack(fps, region):
    """Shelf-pack, tallest first. Returns the height used (may exceed the region)."""
    rx, ry, rw, rh = region
    x = y = shelf = 0.0
    for fp in sorted(fps, key=lambda f: -wh(f)[1]):
        w, h = wh(fp)
        if x + w > rw and x > 0:
            x, y, shelf = 0.0, y + shelf + GAP, 0.0
        place_at(fp, rx + x, ry + y)
        x += w + GAP
        shelf = max(shelf, h)
    return y + shelf


def outline(board):
    w, h = OUTLINE
    rect = pcbnew.PCB_SHAPE(board)
    rect.SetShape(pcbnew.SHAPE_T_RECT)
    rect.SetStart(pcbnew.VECTOR2I(mm(X0), mm(Y0)))
    rect.SetEnd(pcbnew.VECTOR2I(mm(X0 + w), mm(Y0 + h)))
    rect.SetLayer(pcbnew.Edge_Cuts)
    rect.SetWidth(mm(0.1))
    board.Add(rect)
    txt = pcbnew.PCB_TEXT(board)
    txt.SetText("OUTLINE ASSUMED: Phoenix BC 215,6 base PCB 211.5 x 85.1 mm; replace from the housing drawing")
    txt.SetPosition(pcbnew.VECTOR2I(mm(X0), mm(Y0 + h + 4)))
    txt.SetLayer(pcbnew.Cmts_User)
    txt.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    board.Add(txt)


def rules(board):
    ds = board.GetDesignSettings()
    ds.m_TrackMinWidth = mm(0.15)
    ds.m_MinClearance = mm(0.15)
    ds.m_ViasMinSize = mm(0.5)
    ds.m_MinThroughDrill = mm(0.2)    # LAN8742A thermal-pad vias
    ds.m_HoleClearance = mm(0.15)     # USB-C shell holes (footprint as drawn)
    ds.m_ViasMinAnnularWidth = mm(0.125)
    ds.m_CopperEdgeClearance = mm(0.5)
    with open(os.path.join(OUT, "cems_core.kicad_dru"), "w") as f:
        f.write("""(version 1)
# Isolation between the four ground domains (docs/CORE_PCB.md 14). 24 V SELV, functional
# isolation, pollution degree 2: 2.5 mm creepage on the board surface between domains. The
# isolators' own packages bridge the gap; keep the slot under each one clear of copper.
(rule "logic to field"
  (constraint clearance (min 2.5mm))
  (condition "A.NetClass == 'FIELD' && B.NetClass != 'FIELD' && B.Type == 'track'"))
(rule "logic to temperature island"
  (constraint clearance (min 2.5mm))
  (condition "A.NetClass == 'ISO_T' && B.NetClass != 'ISO_T' && B.Type == 'track'"))
(rule "logic to analog island"
  (constraint clearance (min 2.5mm))
  (condition "A.NetClass == 'ISO_AI' && B.NetClass != 'ISO_AI' && B.Type == 'track'"))
(rule "field current"
  (constraint track_width (min 0.5mm))
  (condition "A.NetName == 'PUMP_PWR' || A.NetName == '+24V_FP' || A.NetName == '+24V_FA' || A.NetName == '+24V_FB' || A.NetName == 'FA_IN' || A.NetName == 'FB_IN'"))
""")


def project_netclasses(nets):
    pro = os.path.join(OUT, "cems_core.kicad_pro")
    with open(pro) as f:
        data = json.load(f)
    classes = [
        {"name": "Default", "clearance": 0.15, "track_width": 0.2, "via_diameter": 0.6, "via_drill": 0.3},
        {"name": "Power", "clearance": 0.2, "track_width": 0.5, "via_diameter": 0.8, "via_drill": 0.4},
        {"name": "FIELD", "clearance": 0.3, "track_width": 0.3, "via_diameter": 0.8, "via_drill": 0.4},
        {"name": "ISO_T", "clearance": 0.2, "track_width": 0.2, "via_diameter": 0.6, "via_drill": 0.3},
        {"name": "ISO_AI", "clearance": 0.2, "track_width": 0.25, "via_diameter": 0.6, "via_drill": 0.3},
    ]
    for c in classes:
        c.update({"bus_width": 12, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2,
                  "line_style": 0, "microvia_diameter": 0.3, "microvia_drill": 0.1, "pcb_color": "rgba(0, 0, 0, 0.000)",
                  "schematic_color": "rgba(0, 0, 0, 0.000)", "wire_width": 6})
    patterns = [{"netclass": net_class(n), "pattern": n} for n in sorted(nets) if net_class(n) != "Default"]
    data["net_settings"] = {"classes": classes, "meta": {"version": 3}, "netclass_patterns": patterns}
    with open(pro, "w") as f:
        json.dump(data, f, indent=2)
    return patterns


def main():
    target = os.path.join(OUT, "cems_core.kicad_pcb")
    if os.path.exists(target) and "--force" not in sys.argv:
        raise SystemExit(f"{target} exists; it may hold hand placement. Use --force to regenerate it.")
    board = pcbnew.BOARD()
    board.SetCopperLayerCount(4)
    nets = {}
    for p in design.PARTS:
        for n in p["pins"].values():
            if n not in nets:
                nets[n] = pcbnew.NETINFO_ITEM(board, n)
                board.Add(nets[n])

    fps = {}
    for p in design.PARTS:
        fp = load(p["footprint"])
        fp.SetReference(p["ref"])
        fp.SetValue(p["value"])
        if p["dnp"]:
            fp.SetAttributes(fp.GetAttributes() | pcbnew.FP_EXCLUDE_FROM_BOM | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
        for pad in fp.Pads():
            net = p["pins"].get(pad.GetNumber())
            if net:
                pad.SetNet(nets[net])
        board.Add(fp)
        fps[p["ref"]] = fp

    outline(board)
    w, h = OUTLINE
    report = []
    edge_row([fps[r] for r in TOP_EDGE], lambda fh: EDGE, report)
    bottom = [fps[r] for r in BOTTOM_EDGE]
    run = sum(wh(f)[0] for f in bottom) + GAP * (len(bottom) - 1)
    edge_row_from = w - EDGE - run
    x = edge_row_from
    for fp in bottom:
        fw, fh = wh(fp)
        place_at(fp, x, h - EDGE - fh)
        x += fw + GAP
    report.append(run)
    fixed = set(TOP_EDGE + BOTTOM_EDGE + TIER2)
    groups = {k: [fps[p["ref"]] for p in design.PARTS if p["sheet"] in v and p["ref"] not in fixed] for k, v in ORDER.items()}
    band_h = BAND[1] - BAND[0]
    area = {s: WEIGHT.get(s, 1.0) * sum(wh(f)[0] * wh(f)[1] for f in g) for s, g in groups.items()}
    area["io_dig"] += sum(wh(fps[r])[0] * wh(fps[r])[1] for r in TIER2)
    scale = (w - 2 * EDGE) / sum(area.values())
    def rows_free(x0, x1):
        """Free y range for a region spanning x0..x1: below the top connectors that overlap it,
        above the bottom ones."""
        y0, y1 = BAND[0] - 8, h - EDGE
        for refs, top in ((TOP_EDGE, True), (BOTTOM_EDGE, False)):
            for r in refs:
                bb = size(fps[r])
                bx0, bx1 = pcbnew.ToMM(bb.GetX()) - X0, pcbnew.ToMM(bb.GetRight()) - X0
                if bx1 > x0 and bx0 < x1:
                    if top:
                        y0 = max(y0, pcbnew.ToMM(bb.GetBottom()) - Y0 + GAP)
                    else:
                        y1 = min(y1, pcbnew.ToMM(bb.GetY()) - Y0 - GAP)
        return y0, y1

    used = {}
    x = EDGE
    for s in ORDER:
        rw = area[s] * scale
        y0, y1 = rows_free(x, x + rw)
        region = (x, y0, rw - GAP, y1 - y0)
        if s == "io_dig":  # IDC headers side by side, standing in the band, parts beside them
            hx = x
            for r in TIER2:
                fw, fh = wh(fps[r])
                place_at(fps[r], hx, y0)
                hx += fw + 2
            region = (hx, y0, rw - (hx - x) - GAP, y1 - y0)
        used[s] = (pack(groups[s], region), y1 - y0, rw)
        x += rw

    rules(board)
    patterns = project_netclasses(nets)
    pcbnew.SaveBoard(os.path.join(OUT, "cems_core.kicad_pcb"), board)
    with open(os.path.join(OUT, "fp-lib-table"), "w") as f:
        f.write('(fp_lib_table\n  (version 7)\n'
                '  (lib (name "cems_core")(type "KiCad")(uri "${KIPRJMOD}/cems_core.pretty")(options "")'
                '(descr "CEMS core board footprints"))\n)\n')
    print(f"{len(fps)} footprints, {len(nets)} nets, {len(patterns)} nets in iso/power classes")
    print(f"edge rows: top {report[0]:.1f} mm, bottom {report[1]:.1f} mm of {w} mm")
    for sheet, (got, room, rw) in used.items():
        flag = "" if got <= room else "  OVERFLOW"
        print(f"  {sheet:11s} {rw:5.1f} mm wide, packed height {got:5.1f} of {room:4.1f} mm{flag}")


if __name__ == "__main__":
    main()
