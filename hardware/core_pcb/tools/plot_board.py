#!/usr/bin/env python3
"""Draw the board's placement (courtyards by domain, tracks if any) to a PNG: plot_board.py out.png"""
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mp
import pcbnew
import gen_pcb as g
import autolayout as al

b = pcbnew.LoadBoard(al.BOARD)
col = {"L": "#4c78a8", "F": "#e45756", "T": "#54a24b", "A": "#b279a2"}
fig, ax = plt.subplots(figsize=(21, 9))
ax.add_patch(mp.Rectangle((0, 0), g.OUTLINE[0], g.OUTLINE[1], fill=False, lw=1.5))
for t in b.GetTracks():
    if t.GetClass() == "PCB_TRACK":
        s, e = t.GetStart(), t.GetEnd()
        c = {pcbnew.F_Cu: "#d62728", pcbnew.B_Cu: "#1f77b4", pcbnew.In2_Cu: "#ff7f0e", pcbnew.In1_Cu: "#2ca02c"}.get(t.GetLayer(), "k")
        ax.plot([pcbnew.ToMM(s.x) - g.X0, pcbnew.ToMM(e.x) - g.X0], [pcbnew.ToMM(s.y) - g.Y0, pcbnew.ToMM(e.y) - g.Y0],
                color=c, lw=0.4, alpha=0.6)
for fp in b.GetFootprints():
    x0, y0, x1, y1 = al.bbox(fp)
    ds = {al.dom(p.GetNetname()) for p in fp.Pads() if p.GetNetname()}
    c = col[ds.pop()] if len(ds) == 1 else "#f2cf5b"
    ax.add_patch(mp.Rectangle((x0, y0), x1 - x0, y1 - y0, fc=c, ec="k", lw=0.3,
                              alpha=0.25 if fp.IsFlipped() else 0.6, hatch="//" if fp.IsFlipped() else None))
    if (x1 - x0) * (y1 - y0) > 40:
        ax.text((x0 + x1) / 2, (y0 + y1) / 2, fp.GetReference(), ha="center", va="center", fontsize=6)
ax.set_xlim(-2, g.OUTLINE[0] + 2)
ax.set_ylim(g.OUTLINE[1] + 2, -2)
ax.set_aspect("equal")
ax.set_title("CEMS core board: blue logic, red field, green temperature island, purple analog island, "
             "yellow isolators; hatched = bottom side")
fig.savefig(sys.argv[1], dpi=110, bbox_inches="tight")
