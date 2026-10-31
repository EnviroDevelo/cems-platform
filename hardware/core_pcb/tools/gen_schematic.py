#!/usr/bin/env python3
"""Generate the core PCB KiCad 7 schematic from design.py.

Every connected pin gets a net label at its end (a global label when the net is used on
more than one sheet), every unused pin a no-connect flag. Symbols come from the KiCad 7
libraries installed with KiCad (/usr/share/kicad/symbols) plus the project library
cems_core.kicad_sym. Run from anywhere:

    python3 hardware/core_pcb/tools/gen_schematic.py

then check_netlist.py to prove the result matches design.py.
"""
import copy
import os
import sys
import uuid as uuidlib

from kiutils.items.common import Effects, Font, Justify, PageSettings, Position, Property, TitleBlock
from kiutils.items.schitems import (GlobalLabel, HierarchicalSheet, HierarchicalSheetInstance,
                                    HierarchicalSheetProjectInstance, HierarchicalSheetProjectPath,
                                    LocalLabel, NoConnect, SchematicSymbol, SymbolProjectInstance,
                                    SymbolProjectPath, Text)
from kiutils.schematic import Schematic
from kiutils.symbol import SymbolLib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import design  # noqa: E402

KICAD_SYMBOLS = os.environ.get("KICAD7_SYMBOL_DIR", "/usr/share/kicad/symbols")
PROJECT = "cems_core"
REV = "0.2"
DATE = "2026-10-31"
GRID = 2.54

_ns = uuidlib.UUID("6f1b2c3e-0000-4000-8000-00000000c0de")


def uid(*parts):
    """Stable UUIDs so a regenerated schematic diffs cleanly."""
    return str(uuidlib.uuid5(_ns, "/".join(parts)))


def snap(v):
    return round(v / GRID) * GRID


# ---------------------------------------------------------------- library symbols
_libs = {}


def load_lib(nick):
    if nick not in _libs:
        path = (os.path.join(OUT, f"{PROJECT}.kicad_sym") if nick == PROJECT
                else os.path.join(KICAD_SYMBOLS, f"{nick}.kicad_sym"))
        _libs[nick] = SymbolLib.from_file(path)
    return _libs[nick]


def lib_symbol(lib_id):
    """The symbol, flattened (KiCad embeds derived symbols with the parent's units)."""
    nick, name = lib_id.split(":")
    lib = load_lib(nick)
    sym = copy.deepcopy(next(s for s in lib.symbols if s.entryName == name))
    if sym.extends:
        parent = copy.deepcopy(next(s for s in lib.symbols if s.entryName == sym.extends))
        parent.properties = sym.properties
        parent.extends = None
        parent.entryName = name
        sym = parent
    sym.libraryNickname = nick
    sym.entryName = name
    for u in sym.units:
        u.entryName = name
    return sym


def pins_of(sym):
    out = {}
    for u in sym.units:
        if u.unitId not in (0, 1):
            continue
        for p in u.pins:
            out[p.number] = p
    return out


def body_extent(sym):
    xs, ys = [0.0], [0.0]
    for p in pins_of(sym).values():
        xs.append(p.position.X)
        ys.append(p.position.Y)
    for u in sym.units:
        for g in u.graphicItems:
            for attr in ("start", "end"):
                pt = getattr(g, attr, None)
                if pt is not None:
                    xs.append(pt.X)
                    ys.append(pt.Y)
            for pt in getattr(g, "points", []) or []:
                xs.append(pt.X)
                ys.append(pt.Y)
    return min(xs), max(xs), min(ys), max(ys)


# ---------------------------------------------------------------- helpers
def font(size=1.27):
    return Font(width=size, height=size)


def kitext(text):
    """KiCad stores a line break in text as the two characters backslash, n."""
    return text.replace("\n", "\\n")


def label_len(text):
    return len(text) * 1.05 + 3.0


def label_angle(pin_angle):
    # pin angle = direction from its end towards the body; the label points away from it
    return {0: 180, 180: 0, 90: 270, 270: 90}[int(pin_angle) % 360]


def make_label(text, x, y, angle, is_global):
    just = Justify(horizontally="right" if angle in (180, 270) else "left")
    eff = Effects(font=font(), justify=just)
    pos = Position(X=x, Y=y, angle=angle)
    if is_global:
        lab = GlobalLabel(text=text, shape="passive", position=pos, effects=eff, uuid=uid("gl", text, str(x), str(y)))
        lab.fieldsAutoplaced = True
        lab.properties = [Property(key="Intersheetrefs", value="${INTERSHEET_REFS}", id=0,
                                   position=Position(X=x, Y=y, angle=angle),
                                   effects=Effects(font=font(), justify=just, hide=True))]
        return lab
    return LocalLabel(text=text, position=pos, effects=eff, uuid=uid("ll", text, str(x), str(y)))


# ---------------------------------------------------------------- build
def net_sheets():
    used = {}
    for p in design.PARTS:
        for net in p["pins"].values():
            used.setdefault(net, set()).add(p["sheet"])
    return used


PAPERS = [("A4", 297, 210), ("A3", 420, 297), ("A2", 594, 420), ("A1", 841, 594)]


def label_room(p, sym, globals_):
    """Space the labels take on each side of the body: left, right, up, down (mm)."""
    left = right = up = down = 0.0
    for num, pin in pins_of(sym).items():
        net = p["pins"].get(num)
        ln = label_len(net) + (2 if net in globals_ else 0) if net else 2.0
        a = int(pin.position.angle) % 360
        if a == 0:
            left = max(left, ln)
        elif a == 180:
            right = max(right, ln)
        elif a == 90:
            down = max(down, ln)
        else:
            up = max(up, ln)
    return left, right, up, down


def layout(parts, syms, globals_):
    """Shelf-pack parts in design order. Returns (paper, {ref: (x, y)})."""
    boxes = []
    for p in parts:
        sym = syms[p["symbol"]]
        x0, x1, y0, y1 = body_extent(sym)
        left, right, up, down = label_room(p, sym, globals_)
        w = (x1 - x0) + left + right + 8
        h = (y1 - y0) + up + down + 12  # room for reference and value
        boxes.append((p, w, h, x0, y1, left, up))
    for paper, pw, ph in PAPERS:
        margin, usable_w, usable_h = 15, pw - 30, ph - 30 - 60  # title block and notes
        x = y = 0.0
        shelf = 0.0
        pos = {}
        ok = True
        for p, w, h, x0, y1, left, up in boxes:
            if x + w > usable_w and x > 0:
                x, y, shelf = 0.0, y + shelf, 0.0
            if w > usable_w:
                ok = False
                break
            # symbol origin: body's left edge at x+left, body top at y+up
            ox = snap(margin + x + left - x0 + 4)
            oy = snap(margin + y + up + y1 + 5)
            pos[p["ref"]] = (ox, oy)
            x += w
            shelf = max(shelf, h)
        if ok and y + shelf <= usable_h:
            return paper, pos
    raise SystemExit("sheet does not fit on A1")


def ic_symbol(name, desc, footprint, left, right, datasheet="", keywords=""):
    """A plain box symbol for a part missing from the KiCad libraries. left and right are
    lists of (number, name, type), drawn top to bottom; None leaves a gap."""
    from kiutils.items.common import Fill, Stroke
    from kiutils.symbol import Symbol, SymbolPin, SyRect
    rows = max(len(left), len(right))
    wl = max([len(p[1]) for p in left if p] + [2])
    wr = max([len(p[1]) for p in right if p] + [2])
    half = snap((wl + wr) * 0.7 + 5) / 2
    half = snap(half) if snap(half) >= half else snap(half) + GRID
    top = snap(GRID * (rows - 1) / 2 + 1.27) if rows % 2 else GRID * rows / 2
    top = snap(top)
    eff = lambda hide=False: Effects(font=font(), hide=hide)
    pins = []
    for side, lst in ((0, left), (180, right)):
        x = -half - GRID if side == 0 else half + GRID
        for i, p in enumerate(lst):
            if p is None:
                continue
            num, pname, ptype = p
            pins.append(SymbolPin(electricalType=ptype, graphicalStyle="line",
                                  position=Position(X=round(x, 2), Y=round(top - GRID * i, 2), angle=side), length=GRID,
                                  name=pname, nameEffects=eff(), number=str(num), numberEffects=eff()))
    bottom = top - GRID * (rows - 1)
    body = SyRect(start=Position(X=-half, Y=top + GRID), end=Position(X=half, Y=bottom - GRID),
                  stroke=Stroke(width=0.254, type="default"), fill=Fill(type="background"))
    sym = Symbol(entryName=name, inBom=True, onBoard=True)
    sym.libraryNickname = None
    props = [("Reference", "U", Position(X=-half, Y=top + GRID * 2), False),
             ("Value", name, Position(X=-half, Y=bottom - GRID * 2), False),
             ("Footprint", footprint, Position(X=0, Y=bottom - GRID * 3), True),
             ("Datasheet", datasheet, Position(X=0, Y=0), True),
             ("ki_keywords", keywords, Position(X=0, Y=0), True),
             ("ki_description", desc, Position(X=0, Y=0), True)]
    for pos in (p[2] for p in props):
        pos.angle = 0
    sym.properties = [Property(key=k, value=v, position=pos, id=i,
                               effects=Effects(font=font(), justify=Justify(horizontally="left") if k in ("Reference", "Value") else Justify(), hide=h))
                      for i, (k, v, pos, h) in enumerate(props)]
    u0 = Symbol(entryName=name, unitId=0, styleId=1, graphicItems=[body])
    u1 = Symbol(entryName=name, unitId=1, styleId=1, pins=pins)
    sym.units = [u0, u1]
    return sym


def project_ics():
    import project_symbols
    return [ic_symbol(n, d["desc"], d["footprint"], d["left"], d["right"], d["datasheet"])
            for n, d in project_symbols.ICS.items()]


def build_project_files():
    """cems_core.kicad_sym (parts missing from the KiCad 7 libraries), sym-lib-table, .kicad_pro."""
    lib = SymbolLib(version="20220914", generator="kicad_symbol_editor")
    fram = lib_symbol("Memory_EEPROM:24LC02")
    fram.libraryNickname = None
    fram.entryName = "MB85RC1MT"
    for u in fram.units:
        u.entryName = "MB85RC1MT"
        for pin in u.pins:
            if pin.number == "1":
                pin.name, pin.electricalType = "NC", "no_connect"
    for prop in fram.properties:
        if prop.key == "Value":
            prop.value = "MB85RC1MT"
        elif prop.key == "Datasheet":
            prop.value = "https://www.fujitsu.com/uk/Images/MB85RC1MT-DS501-00028-1v0-E.pdf"
        elif prop.key in ("ki_description", "Description"):
            prop.value = "1 Mbit (128 KB) I2C FRAM, 3.3 V. Pin 1 NC; A1, A2 address pins"
        elif prop.key == "Footprint":
            prop.value = "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"
    psram = lib_symbol("Memory_RAM:ESP-PSRAM32")  # same SOIC-8 pinout as the APS6404L
    psram.libraryNickname = None
    psram.entryName = "APS6404L"
    pin_types = {"1": ("~{CE}", "input"), "2": ("SO/SIO1", "bidirectional"), "3": ("SIO2", "bidirectional"),
                 "5": ("SI/SIO0", "bidirectional"), "6": ("SCLK", "input"), "7": ("SIO3", "bidirectional")}
    for u in psram.units:
        u.entryName = "APS6404L"
        for pin in u.pins:
            if pin.number in pin_types:
                pin.name, pin.electricalType = pin_types[pin.number]
    for prop in psram.properties:
        if prop.key == "Value":
            prop.value = "APS6404L"
        elif prop.key == "Datasheet":
            prop.value = "https://www.apmemory.com/"
        elif prop.key in ("ki_description", "Description"):
            prop.value = "64 Mbit (8 MB) QSPI PSRAM, 3.3 V, SOIC-8"
        elif prop.key == "ki_keywords":
            prop.value = "PSRAM QSPI RAM"
    lib.symbols = [fram, psram] + project_ics()
    lib.to_file(os.path.join(OUT, f"{PROJECT}.kicad_sym"))
    _libs.pop(PROJECT, None)
    with open(os.path.join(OUT, "sym-lib-table"), "w") as f:
        f.write('(sym_lib_table\n  (version 7)\n'
                f'  (lib (name "{PROJECT}")(type "KiCad")(uri "${{KIPRJMOD}}/{PROJECT}.kicad_sym")(options "")(descr "CEMS core board parts"))\n)\n')
    pro = os.path.join(OUT, f"{PROJECT}.kicad_pro")
    if not os.path.exists(pro):
        with open(pro, "w") as f:
            f.write('{\n  "meta": {\n    "filename": "%s.kicad_pro",\n    "version": 1\n  },\n'
                    '  "sheets": []\n}\n' % PROJECT)


def build():
    build_project_files()
    used = net_sheets()
    globals_ = {n for n, s in used.items() if len(s) > 1}
    syms = {p["symbol"]: lib_symbol(p["symbol"]) for p in design.PARTS}

    root_uuid = uid("root")
    root = Schematic(version="20230121", generator="eeschema", uuid=root_uuid)
    root.paper = PageSettings(paperSize="A4")
    root.titleBlock = TitleBlock(title="CEMS core board", date=DATE, revision=REV, company="EnviroDevelo",
                                 comments={1: "Generated from hardware/core_pcb/tools/design.py; do not edit by hand",
                                           2: "Design spec: docs/CORE_PCB.md"})
    root.sheetInstances = [HierarchicalSheetInstance(instancePath="/", page="1")]

    for page, (key, title, desc) in enumerate(design.SHEETS, start=2):
        sheet_uuid = uid("sheet", key)
        path = f"/{root_uuid}/{sheet_uuid}"
        parts = [p for p in design.PARTS if p["sheet"] == key]
        paper, pos = layout(parts, syms, globals_)
        sch = Schematic(version="20230121", generator="eeschema", uuid=uid("sch", key))
        sch.paper = PageSettings(paperSize=paper)
        sch.titleBlock = TitleBlock(title=f"CEMS core board: {title}", date=DATE, revision=REV,
                                    company="EnviroDevelo", comments={1: desc, 2: "Generated from design.py"})
        lib_ids = []
        notes = []
        for p in parts:
            sym = syms[p["symbol"]]
            if p["symbol"] not in lib_ids:
                lib_ids.append(p["symbol"])
            X, Y = pos[p["ref"]]
            x0, x1, y0, y1 = body_extent(sym)
            _, _, up, down = label_room(p, sym, globals_)
            nick, name = p["symbol"].split(":")
            pins = pins_of(sym)
            unknown = set(p["pins"]) - set(pins)
            if unknown:
                raise SystemExit(f"{p['ref']}: no pins {sorted(unknown)} on {p['symbol']}")
            props = [
                Property(key="Reference", value=p["ref"], id=0, position=Position(X=X, Y=snap(Y - y1 - up - 2.54), angle=0),
                         effects=Effects(font=font())),
                Property(key="Value", value=p["value"], id=1, position=Position(X=X, Y=snap(Y - y0 + down + 2.54), angle=0),
                         effects=Effects(font=font())),
                Property(key="Footprint", value=p["footprint"], id=2, position=Position(X=X, Y=Y, angle=0),
                         effects=Effects(font=font(), hide=True)),
                Property(key="Datasheet", value=next((q.value for q in sym.properties if q.key == "Datasheet"), "~"),
                         id=3, position=Position(X=X, Y=Y, angle=0), effects=Effects(font=font(), hide=True)),
                Property(key="MPN", value=p["mpn"] or p["value"], id=4, position=Position(X=X, Y=Y, angle=0),
                         effects=Effects(font=font(), hide=True)),
            ]
            ss = SchematicSymbol(libraryNickname=nick, entryName=name, position=Position(X=X, Y=Y, angle=0),
                                 unit=1, inBom=True, onBoard=True, dnp=p["dnp"], uuid=uid("sym", p["ref"]),
                                 properties=props, pins={n: uid("pin", p["ref"], n) for n in pins},
                                 instances=[SymbolProjectInstance(name=PROJECT, paths=[
                                     SymbolProjectPath(sheetInstancePath=path, reference=p["ref"], unit=1)])])
            sch.schematicSymbols.append(ss)
            for num, pin in pins.items():
                ex, ey = X + pin.position.X, Y - pin.position.Y
                net = p["pins"].get(num)
                if net is None:
                    sch.noConnects.append(NoConnect(position=Position(X=ex, Y=ey), uuid=uid("nc", p["ref"], num)))
                else:
                    sch_lab = make_label(net, ex, ey, label_angle(pin.position.angle), net in globals_)
                    (sch.globalLabels if net in globals_ else sch.labels).append(sch_lab)
            if p["note"] or p["dnp"]:
                notes.append(f"{p['ref']}: {'NOT FITTED. ' if p['dnp'] else ''}{p['note']}")
        sch.libSymbols = [syms[i] for i in lib_ids]
        if notes:
            pw = dict((a, b) for a, b, c in PAPERS)[paper]
            ph = dict((a, c) for a, b, c in PAPERS)[paper]
            import textwrap
            cols = 2 if paper in ("A4", "A3") else 3
            width = int(((pw - 30) / cols - 4) / 0.9)
            notes = [w for n in notes for w in textwrap.wrap(n, width, subsequent_indent="    ")]
            per = -(-len(notes) // cols)
            for ci in range(cols):
                chunk = notes[ci * per:(ci + 1) * per]
                if not chunk:
                    continue
                sch.texts.append(Text(text=kitext(("Notes\n" if ci == 0 else "\n") + "\n".join(chunk)),
                                      position=Position(X=snap(15 + ci * (pw - 30) / cols), Y=snap(ph - 72), angle=0),
                                      effects=Effects(font=font(1.0), justify=Justify(horizontally="left", vertically="top")),
                                      uuid=uid("notes", key, str(ci))))
        sch.to_file(os.path.join(OUT, f"{key}.kicad_sch"))

        sx, sy = 20 + ((page - 2) % 5) * 52, 40 + ((page - 2) // 5) * 45
        hs = HierarchicalSheet(position=Position(X=sx, Y=sy), width=45, height=25,
                               uuid=sheet_uuid,
                               sheetName=Property(key="Sheetname", value=title, id=0,
                                                  position=Position(X=sx, Y=sy - 0.7, angle=0),
                                                  effects=Effects(font=font(), justify=Justify(horizontally="left", vertically="bottom"))),
                               fileName=Property(key="Sheetfile", value=f"{key}.kicad_sch", id=1,
                                                 position=Position(X=sx, Y=sy + 25.6, angle=0),
                                                 effects=Effects(font=font(), justify=Justify(horizontally="left", vertically="top"))),
                               instances=[HierarchicalSheetProjectInstance(name=PROJECT, paths=[
                                   HierarchicalSheetProjectPath(sheetInstancePath=f"/{root_uuid}", page=str(page))])])
        hs.fieldsAutoplaced = True
        root.sheets.append(hs)

    root.texts.append(Text(
        text=kitext("CEMS core board, rev 0.1 schematic (draft for review)\n"
              "Two separate Ethernet networks: client port = STM32H573 MAC + LAN8742A (J2); "
              "I/O port = W5500 (J3), I/O boards only.\n"
              "24 VDC in; DIN-rail enclosure behind the RVT70 display (FFC J4).\n"
              "Pin map and design notes: docs/CORE_PCB.md. Parts marked CHECK in the sheet notes "
              "need datasheet confirmation before layout."),
        position=Position(X=20, Y=90, angle=0),
        effects=Effects(font=font(), justify=Justify(horizontally="left", vertically="top")),
        uuid=uid("roottext")))
    root.to_file(os.path.join(OUT, f"{PROJECT}.kicad_sch"))
    write_bom()
    print(f"{len(design.PARTS)} parts on {len(design.SHEETS)} sheets, {len(used)} nets "
          f"({len(globals_)} across sheets)")


def write_bom():
    """bom.csv: one line per distinct part (value, footprint, MPN), fitted parts first."""
    import csv
    groups = {}
    for p in design.PARTS:
        key = (p["dnp"], p["value"], p["footprint"], p["mpn"])
        groups.setdefault(key, []).append(p["ref"])

    def refkey(ref):
        head = ref.rstrip("0123456789")
        return head, int(ref[len(head):] or 0)

    with open(os.path.join(OUT, "bom.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Qty", "References", "Value", "Footprint", "MPN / note", "Fitted"])
        for (dnp, value, fp, mpn), refs in sorted(groups.items(), key=lambda kv: (kv[0][0], refkey(sorted(kv[1], key=refkey)[0]))):
            refs = sorted(refs, key=refkey)
            w.writerow([len(refs), " ".join(refs), value, fp.split(":")[-1], mpn, "no" if dnp else "yes"])


if __name__ == "__main__":
    build()
