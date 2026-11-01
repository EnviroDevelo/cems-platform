#!/usr/bin/env python3
"""Prototype placement for the core board, by isolation domain.

gen_pcb.py builds the board (outline, footprints, nets, rules, net classes) and its block
plan; this script replaces the block plan with a floor plan that keeps each isolation
domain in one piece, so the isolation gaps are straight lines that only the isolators cross:

    +-------------------------------------------------------------------------------+
    | J1  J2  J3  J18 (logic)     | J13  J15  J17  J11  (field)                     |
    |                             |  field parts, J10 ribbon header                 |
    |  power + core (logic)       |=== F/L isolators: ISO2H823 ISO808A ISO1I813T ===|
    |                             |  logic strip: I/O logic parts, SPI4 bus         |
    |                     +-------+=== T/L and A/L isolators: ISOW7741, ITX2424 ===|
    |  J26                |  temperature island      |  analog island             |
    |                     | J19 J20 J21              | J25 J22 J23 J24            |
    +-------------------------------------------------------------------------------+

Inside each zone, every IC is packed together with the passives that hang on its pins
(a "cluster"); clusters are then shelf-packed. When a zone is full, its small passives go
to the bottom side under the same zone. On In1 each domain gets its own ground plane; a
copper keep-out strip runs along each domain boundary on every layer except under the
isolators' bodies.

    python3 tools/gen_pcb.py --force && python3 tools/autolayout.py
"""
import collections
import os
import sys

import numpy as np
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import design  # noqa: E402
import gen_pcb as g  # noqa: E402

BOARD = os.path.join(g.OUT, "cems_core.kicad_pcb")
W, H = g.OUTLINE
E = g.EDGE
GAP = 0.25           # between courtyards (each courtyard already has 0.25 mm)
ISO_GAP = 2.5        # isolation strip (docs/CORE_PCB.md 14)
DOM = {"FIELD": "F", "HICURRENT": "F", "OUT1A": "F", "ISO_T": "T", "ISO_AI": "A"}
GROUND = {"L": "GND", "F": "FIELD_0V", "T": "GND_T", "A": "GND_AI"}
TOP_L = ["J1", "J2", "J3", "J18"]
TOP_F = ["J13", "J15", "J17", "J11"]
BOT_T = ["J19", "J20", "J21"]
BOT_A = ["J25", "J22", "J23", "J24"]
RAILS = {"GND", "FIELD_0V", "GND_T", "GND_AI", "+3V3", "+5V", "+24V", "+3V3_T", "+3V3_TA", "+5V_AI", "+5V_AIA",
         "+24V_AI", "+24V_FA", "+24V_FB", "+24V_FP", "+5V_P", "+13V_AI"}


def dom(net):
    return DOM.get(g.net_class(net), "L")


def mm(v):
    return pcbnew.FromMM(v)


def pads_xy(fp, nets):
    out = collections.defaultdict(list)
    for pad in fp.Pads():
        n = pad.GetNetname()
        if n:
            p = pad.GetPosition()
            out[dom(n)].append((pcbnew.ToMM(p.x) - g.X0, pcbnew.ToMM(p.y) - g.Y0))
    return out


def centroid(pts):
    return sum(p[0] for p in pts) / len(pts), sum(p[1] for p in pts) / len(pts)


def bbox(fp):
    bb = g.size(fp)
    return (pcbnew.ToMM(bb.GetX()) - g.X0, pcbnew.ToMM(bb.GetY()) - g.Y0,
            pcbnew.ToMM(bb.GetRight()) - g.X0, pcbnew.ToMM(bb.GetBottom()) - g.Y0)


def orient(fp, other, other_up):
    """Rotate an isolator so its `other`-domain pads face up (other_up) or down."""
    best = None
    for k in range(4):
        pd = pads_xy(fp, None)
        lx, ly = centroid(pd["L"])
        ox, oy = centroid(pd[other])
        score = (ly - oy) if other_up else (oy - ly)
        if best is None or score > best[0]:
            best = (score, k)
        fp.Rotate(fp.GetPosition(), pcbnew.EDA_ANGLE(90, pcbnew.DEGREES_T))
    for _ in range(best[1]):
        fp.Rotate(fp.GetPosition(), pcbnew.EDA_ANGLE(90, pcbnew.DEGREES_T))


def split_y(fp, other, other_up):
    """y of the line between the logic pads and the other-domain pads (pad edges)."""
    lo, hi = [], []
    for pad in fp.Pads():
        n = pad.GetNetname()
        if not n:
            continue
        bb = pad.GetBoundingBox()
        y0, y1 = pcbnew.ToMM(bb.GetY()) - g.Y0, pcbnew.ToMM(bb.GetBottom()) - g.Y0
        (hi if dom(n) == other else lo).append((y0, y1))
    if other_up:   # other domain above: line between max bottom of other pads and min top of logic pads
        return (max(y1 for _, y1 in hi) + min(y0 for y0, _ in lo)) / 2
    return (max(y1 for _, y1 in lo) + min(y0 for y0, _ in hi)) / 2


def move_by(fp, dx, dy):
    fp.Move(pcbnew.VECTOR2I(mm(dx), mm(dy)))


def clusters(parts, fps):
    """IC (4+ pads) plus the small parts that share a signal net with it."""
    big = [p for p in parts if len(fps[p["ref"]].Pads()) >= 4]
    small = [p for p in parts if len(fps[p["ref"]].Pads()) < 4]
    owner = {}
    for s in small:
        nets = set(s["pins"].values())
        sig = nets - RAILS
        best, score = None, 0
        for b in big:
            bn = set(b["pins"].values())
            sc = 3 * len(sig & bn) + (1 if b["sheet"] == s["sheet"] else 0) + 0.5 * len((nets & RAILS) & bn)
            if sc > score:
                best, score = b["ref"], sc
        owner[s["ref"]] = best if score >= 3 else None
    cl = collections.OrderedDict((b["ref"], [b["ref"]]) for b in big)
    loose = []
    for s in small:
        (cl[owner[s["ref"]]] if owner[s["ref"]] else loose).append(s["ref"])
    return list(cl.values()) + [[r] for r in loose]


def pack_block(refs, fps, width):
    """Pack one cluster (IC first, then passives tallest first) into a block of the given width."""
    order = refs[:1] + sorted(refs[1:], key=lambda r: -g.wh(fps[r])[1])
    x = y = shelf = 0.0
    offs = {}
    wmax = 0.0
    for r in order:
        w, h = g.wh(fps[r])
        if x + w > width and x > 0:
            x, y, shelf = 0.0, y + shelf + GAP, 0.0
        offs[r] = (x, y)
        x += w + GAP
        wmax = max(wmax, x - GAP)
        shelf = max(shelf, h)
    return wmax, y + shelf, offs


RES = 0.25   # placement grid (mm)


class Grid:
    """Occupancy of one board side plus the domain masks, at RES mm."""

    def __init__(self, masks):
        self.nx, self.ny = int(W / RES) + 1, int(H / RES) + 1
        self.occ = np.zeros((self.ny, self.nx), bool)
        self.masks = masks

    def mark(self, x0, y0, x1, y1):
        i0, j0 = max(0, int((y0 - GAP / 2) / RES)), max(0, int((x0 - GAP / 2) / RES))
        i1, j1 = int(np.ceil((y1 + GAP / 2) / RES)), int(np.ceil((x1 + GAP / 2) / RES))
        self.occ[i0:i1, j0:j1] = True

    def find(self, d, w, h, target):
        """Top-left (x, y) for a w x h block in domain d, nearest to target; None if no room."""
        blocked = self.occ | ~self.masks[d]
        hw, hh = int(np.ceil((w + GAP) / RES)), int(np.ceil((h + GAP) / RES))
        if hh >= self.ny or hw >= self.nx:
            return None
        S = np.zeros((self.ny + 1, self.nx + 1), np.int32)
        S[1:, 1:] = blocked.cumsum(0).cumsum(1)
        win = S[hh:, hw:] - S[:-hh, hw:] - S[hh:, :-hw] + S[:-hh, :-hw]
        ii, jj = np.nonzero(win == 0)
        if len(ii) == 0:
            return None
        tx, ty = target
        cx, cy = jj * RES + w / 2, ii * RES + h / 2
        k = np.argmin((cx - tx) ** 2 + (cy - ty) ** 2 + 0.01 * (ii * RES))
        return float(jj[k] * RES + GAP / 2), float(ii[k] * RES + GAP / 2)


def place_iso_row(fps, refs, other, other_up, x0, x1, yline):
    x = x0
    for r in sorted(refs, key=lambda r: -g.wh(fps[r])[0]):
        fp = fps[r]
        w, h = g.wh(fp)
        g.place_at(fp, x, 0)
        move_by(fp, 0, yline - split_y(fp, other, other_up))
        x += w + 1.0
        if x > x1 + 1:
            raise SystemExit(f"isolator row from {x0:.0f} to {x1:.0f} mm too short for {refs}")


def poly(board, pts, layer_set, net=None, keepout=False, name="", priority=0):
    z = pcbnew.ZONE(board)
    z.SetLayerSet(layer_set)
    ch = pcbnew.SHAPE_LINE_CHAIN()
    for x, y in pts:
        ch.Append(mm(g.X0 + x), mm(g.Y0 + y))
    ch.SetClosed(True)
    z.Outline().AddOutline(ch)
    if keepout:
        z.SetIsRuleArea(True)
        z.SetDoNotAllowTracks(True)
        z.SetDoNotAllowVias(True)
        z.SetDoNotAllowCopperPour(True)
        z.SetDoNotAllowPads(False)
        z.SetDoNotAllowFootprints(False)
    else:
        z.SetNetCode(board.GetNetsByName()[net].GetNetCode())
        z.SetLocalClearance(mm(0.3))
        z.SetMinThickness(mm(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetThermalReliefGap(mm(0.3))
        z.SetThermalReliefSpokeWidth(mm(0.4))
        z.SetAssignedPriority(priority)
    if name:
        z.SetZoneName(name)
    board.Add(z)
    return z


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def planes_and_gaps(board, fps, b, isolators):
    """In1: one ground plane per domain. Keep-out strips (ISO_GAP wide) along every domain
    boundary on all copper layers; on F.Cu the strip is opened under each isolator body."""
    for z in list(board.Zones()):
        board.Remove(z)
    xl, xt, xa, yF, yI = b["xl"], b["xt"], b["xa"], b["yF"], b["yI"]
    hg = ISO_GAP / 2
    in1 = pcbnew.LSET()
    in1.AddLayer(pcbnew.In1_Cu)
    regions = {
        "L": [(E, E), (xl - hg, E), (xl - hg, yF + hg), (W - E, yF + hg), (W - E, yI - hg), (xt - hg, yI - hg),
              (xt - hg, H - E), (E, H - E)],
        "F": rect(xl + hg, E, W - E, yF - hg),
        "T": rect(xt + hg, yI + hg, xa - hg, H - E),
        "A": rect(xa + hg, yI + hg, W - E, H - E),
    }
    for d, pts in regions.items():
        poly(board, pts, in1, net=GROUND[d], name=f"{GROUND[d]} plane")
    in2 = pcbnew.LSET()
    in2.AddLayer(pcbnew.In2_Cu)
    poly(board, regions["F"], in2, net="+24V_FA", name="+24V_FA plane")   # bank A, inputs, ribbon: up to 6 A
    # boundary segments: (orientation, fixed coord, from, to)
    segs = [("v", xl, 0, yF + hg), ("h", yF, xl - hg, W), ("h", yI, xt - hg, W), ("v", xt, yI - hg, H),
            ("v", xa, yI, H)]
    allcu = pcbnew.LSET.AllCuMask(4)
    inner = pcbnew.LSET.AllCuMask(4)
    inner.RemoveLayer(pcbnew.F_Cu)
    holes = [bbox(fps[r]) for r in isolators]
    for o, c, a0, a1 in segs:
        # full strip on the inner and bottom layers
        r_ = rect(c - hg, a0, c + hg, a1) if o == "v" else rect(a0, c - hg, a1, c + hg)
        poly(board, r_, inner, keepout=True, name="isolation gap")
        # F.Cu: same strip with the isolator bodies cut out
        cuts = sorted((x0, x1) if o == "h" else (y0, y1) for x0, y0, x1, y1 in holes
                      if (o == "h" and y0 < c < y1) or (o == "v" and x0 < c < x1))
        top = pcbnew.LSET()
        top.AddLayer(pcbnew.F_Cu)
        start = a0
        for k0, k1 in cuts + [(a1, a1)]:
            if k0 > start:
                r_ = rect(c - hg, start, c + hg, k0) if o == "v" else rect(start, c - hg, k0, c + hg)
                poly(board, r_, top, keepout=True, name="isolation gap")
            start = max(start, k1)
    # under each isolator: a keep-out across its body on F.Cu too, between its two pad groups,
    # so no track can run under the package from one domain to the other
    for r in isolators:
        fp = fps[r]
        x0, y0, x1, y1 = bbox(fp)
        mid = None
        for o, c, a0, a1 in segs:
            if o == "h" and y0 < c < y1 and a0 <= (x0 + x1) / 2 <= a1:
                mid = c
        if mid is None:
            continue
        pads = [p.GetBoundingBox() for p in fp.Pads()]
        top = pcbnew.LSET()
        top.AddLayer(pcbnew.F_Cu)
        lo = max([pcbnew.ToMM(p.GetBottom()) - g.Y0 for p in pads if pcbnew.ToMM(p.GetBottom()) - g.Y0 < mid] + [mid - 0.5])
        hi = min([pcbnew.ToMM(p.GetY()) - g.Y0 for p in pads if pcbnew.ToMM(p.GetY()) - g.Y0 > mid] + [mid + 0.5])
        if hi - lo > 0.6:
            poly(board, rect(x0, lo + 0.2, x1, hi - 0.2), top, keepout=True, name=f"{r} isolation")


def main():
    board = pcbnew.LoadBoard(BOARD)
    fps = {fp.GetReference(): fp for fp in board.GetFootprints()}
    for fp in fps.values():   # start clean: all on top, unrotated
        if fp.IsFlipped():
            fp.Flip(fp.GetPosition(), False)
        fp.SetOrientationDegrees(0)
    part = {p["ref"]: p for p in design.PARTS}
    pdom = {r: "".join(sorted({dom(n) for n in p["pins"].values()})) for r, p in part.items()}

    def area(refs):
        return sum(g.wh(fps[r])[0] * g.wh(fps[r])[1] for r in refs)

    # --- edge connectors
    x = E
    for r in TOP_L + TOP_F:
        w, h = g.wh(fps[r])
        if r == TOP_F[0]:
            xl = x + 0.5               # logic/field boundary
            x = xl + ISO_GAP / 2 + 0.5
        g.place_at(fps[r], x, E)
        x += w + GAP
    top_l_bot = max(bbox(fps[r])[3] for r in TOP_L)
    top_f_bot = max(bbox(fps[r])[3] for r in TOP_F)
    bot = BOT_T + BOT_A
    run = sum(g.wh(fps[r])[0] for r in bot) + GAP * (len(bot) - 1) + ISO_GAP + 1
    x = W - E - run
    xt = x - 0.5 - ISO_GAP / 2        # logic/temperature boundary
    for r in bot:
        w, h = g.wh(fps[r])
        if r == BOT_A[0]:
            xa = x + 0.5              # temperature/analog boundary
            x = xa + ISO_GAP / 2 + 0.5
        g.place_at(fps[r], x, H - E - h)
        x += w + GAP
    bot_y = min(bbox(fps[r])[1] for r in bot)

    iso_f = [r for r, d in pdom.items() if d == "FL"]
    iso_t = [r for r, d in pdom.items() if d == "LT"]
    iso_a = [r for r, d in pdom.items() if d == "AL"]
    for r in iso_f:
        orient(fps[r], "F", True)
    for r in iso_t:
        orient(fps[r], "T", False)
    for r in iso_a:
        orient(fps[r], "A", False)
    fixed = set(TOP_L + TOP_F + bot + iso_f + iso_t + iso_a)
    F = [r for r, d in pdom.items() if d == "F" and r not in fixed]
    T = [r for r, d in pdom.items() if d == "T" and r not in fixed]
    A = [r for r, d in pdom.items() if d == "A" and r not in fixed]
    L = [r for r, d in pdom.items() if d == "L" and r not in fixed]

    # --- boundary lines from areas
    eff = float(os.environ.get("PLACE_EFF", "0.62"))
    wr = W - E - xl
    hF = (area(F) + area(iso_f) / 2) / float(os.environ.get("FIELD_EFF", "0.80")) / wr
    yF = top_f_bot + hF
    wi = W - E - xt
    hI = max(16.0, (area(T) + area(A) + area(iso_t + iso_a) / 2) / (eff * 0.75) / wi)
    yI = bot_y - hI
    print(f"areas F {area(F):.0f} T {area(T):.0f} A {area(A):.0f} L {area(L):.0f} iso {area(iso_f + iso_t + iso_a):.0f}")
    print(f"xl {xl:.1f} xt {xt:.1f} xa {xa:.1f} yF {yF:.1f} yI {yI:.1f} bottom row {bot_y:.1f}")
    place_iso_row(fps, iso_f, "F", True, xl + ISO_GAP, W - E, yF)
    place_iso_row(fps, iso_t, "T", False, xt + ISO_GAP, xa, yI)
    place_iso_row(fps, iso_a, "A", False, xa + ISO_GAP, W - E, yI)

    # --- domain masks (cell centres), the strip of ISO_GAP between domains belongs to nobody
    ny, nx = int(H / RES) + 1, int(W / RES) + 1
    yy, xx = np.mgrid[0:ny, 0:nx] * RES
    inside = (xx >= E) & (xx <= W - E) & (yy >= E) & (yy <= H - E)
    hg = ISO_GAP / 2
    field = (xx >= xl + hg) & (yy <= yF - hg)
    island = (xx >= xt + hg) & (yy >= yI + hg)
    temp = island & (xx <= xa - hg)
    ana = island & (xx >= xa + hg)
    logic = ~((xx > xl - hg) & (yy < yF + hg)) & ~((xx > xt - hg) & (yy > yI - hg))
    masks = {"F": field & inside, "T": temp & inside, "A": ana & inside, "L": logic & inside}
    top = Grid(masks)
    botg = Grid(masks)
    for r in fixed:
        x0, y0, x1, y1 = bbox(fps[r])
        top.mark(x0, y0, x1, y1)
        if any(pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for pad in fps[r].Pads()):
            botg.mark(x0, y0, x1, y1)

    placed = set(fixed)
    net_pads = collections.defaultdict(list)

    def pad_centres(r):
        for pad in fps[r].Pads():
            n = pad.GetNetname()
            if n and n not in RAILS:
                p = pad.GetPosition()
                net_pads[n].append((pcbnew.ToMM(p.x) - g.X0, pcbnew.ToMM(p.y) - g.Y0))

    for r in fixed:
        pad_centres(r)

    def target(refs, d):
        pts = [pt for r in refs for n in set(part[r]["pins"].values()) - RAILS for pt in net_pads.get(n, [])]
        if pts:
            return centroid(pts)
        # rail-only parts (bulk capacitors, TVSs): near the placed parts of the same sheet that share a rail
        rails = {n for r in refs for n in part[r]["pins"].values()} - {GROUND[d]}
        pts = [centroid([(pcbnew.ToMM(p.GetPosition().x) - g.X0, pcbnew.ToMM(p.GetPosition().y) - g.Y0)
                         for p in fps[q].Pads()])
               for q in placed if part[q]["sheet"] == part[refs[0]]["sheet"]
               and rails & set(part[q]["pins"].values())]
        if pts:
            return centroid(pts)
        m = masks[d]
        ii, jj = np.nonzero(m)
        return jj.mean() * RES, ii.mean() * RES

    report = collections.Counter()
    bottom_parts = []
    for d, refs in (("F", F), ("T", T), ("A", A), ("L", L)):
        cls = clusters([part[r] for r in refs], fps)
        # most-connected-to-placed first, big clusters first
        pending = list(cls)
        while pending:
            def conn(c):
                nets = {n for r in c for n in part[r]["pins"].values()} - RAILS
                return sum(1 for n in nets if net_pads.get(n)) * 10 + area(c) / 100
            pending.sort(key=lambda c: -conn(c))
            c = pending.pop(0)
            tgt = target(c, d)
            iw = g.wh(fps[c[0]])[0]
            width = max(iw, (area(c) * 1.5) ** 0.5)
            w, h, offs = pack_block(c, fps, width)
            pos = top.find(d, w, h, tgt)
            if pos:
                for r in c:
                    g.place_at(fps[r], pos[0] + offs[r][0], pos[1] + offs[r][1])
                top.mark(pos[0], pos[1], pos[0] + w, pos[1] + h)
                if any(pad.GetAttribute() == pcbnew.PAD_ATTRIB_PTH for r in c for pad in fps[r].Pads()):
                    botg.mark(pos[0], pos[1], pos[0] + w, pos[1] + h)
                for r in c:
                    pad_centres(r)
                placed.update(c)
                report[d + " clusters"] += 1
                continue
            if len(c) > 1:              # split the cluster: IC alone, passives one by one
                pending = [[c[0]]] + [[r] for r in c[1:]] + pending
                report[d + " split"] += 1
                continue
            r = c[0]
            fp = fps[r]
            if not getattr(fp, "_turned", False):
                fp.Rotate(fp.GetPosition(), pcbnew.EDA_ANGLE(90, pcbnew.DEGREES_T))
                fp._turned = True
                pending.insert(0, c)
                continue
            smd = all(pad.GetAttribute() == pcbnew.PAD_ATTRIB_SMD for pad in fp.Pads())
            if smd and len(fp.Pads()) <= 6:
                fp.Flip(fp.GetPosition(), False)
                w, h = g.wh(fp)
                pos = botg.find(d, w, h, tgt)
                if pos:
                    g.place_at(fp, *pos)
                    botg.mark(pos[0], pos[1], pos[0] + w, pos[1] + h)
                    pad_centres(r)
                    placed.add(r)
                    bottom_parts.append(r)
                    continue
            raise SystemExit(f"no room for {r} ({part[r]['value']}) in domain {d}")
    print(dict(report), f"bottom side: {len(bottom_parts)} parts")
    planes_and_gaps(board, fps, dict(xl=xl, xt=xt, xa=xa, yF=yF, yI=yI), iso_f + iso_t + iso_a)
    pcbnew.SaveBoard(BOARD, board)
    return dict(xl=xl, xt=xt, xa=xa, yF=yF, yI=yI, bot_y=bot_y, top_f_bot=top_f_bot)


if __name__ == "__main__":
    main()
