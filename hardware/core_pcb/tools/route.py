#!/usr/bin/env python3
"""Route the placed core board with Freerouting, then fill zones and run DRC.

    python3 tools/route.py [passes]      (needs freerouting.jar: FREEROUTING_JAR=...)

Net classes come from gen_pcb.CLASSES (pcbnew's Python loader does not read them from the
.kicad_pro, so they are applied here before the Specctra export).
"""
import os
import subprocess
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_pcb as g  # noqa: E402

BOARD = os.path.join(g.OUT, "cems_core.kicad_pcb")
JAR = os.environ.get("FREEROUTING_JAR", "freerouting.jar")


def apply_netclasses(board):
    ns = board.GetDesignSettings().m_NetSettings
    made = {}
    for c in g.CLASSES:
        nc = ns.m_DefaultNetClass if c["name"] == "Default" else pcbnew.NETCLASS(c["name"])
        nc.SetClearance(pcbnew.FromMM(c["clearance"]))
        nc.SetTrackWidth(pcbnew.FromMM(c["track_width"]))
        nc.SetViaDiameter(pcbnew.FromMM(c["via_diameter"]))
        nc.SetViaDrill(pcbnew.FromMM(c["via_drill"]))
        if c["name"] != "Default":
            ns.m_NetClasses[c["name"]] = nc
        made[c["name"]] = nc
    for name, net in board.GetNetsByName().items():
        cls = g.net_class(str(name))
        net.SetNetClass(made[cls])
    ds = board.GetDesignSettings()
    ds.m_MinClearance = pcbnew.FromMM(0.15)
    ds.m_TrackMinWidth = pcbnew.FromMM(0.15)


def fill(board):
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


def drc(board, path):
    pcbnew.WriteDRCReport(board, path, pcbnew.EDA_UNITS_MILLIMETRES, True)
    counts = {}
    for line in open(path):
        if line.startswith("["):
            k = line.split("]")[0][1:]
            counts[k] = counts.get(k, 0) + 1
    return counts


def main():
    passes = sys.argv[1] if len(sys.argv) > 1 else "40"
    work = os.environ.get("ROUTE_WORK", "/tmp")
    board = pcbnew.LoadBoard(BOARD)
    apply_netclasses(board)
    for t in list(board.GetTracks()):
        board.Remove(t)
    fill(board)
    dsn, ses = os.path.join(work, "cems_core.dsn"), os.path.join(work, "cems_core.ses")
    if not pcbnew.ExportSpecctraDSN(board, dsn):
        raise SystemExit("DSN export failed")
    cmd = ["java", "-Djava.awt.headless=true", "-jar", JAR, "-de", dsn, "-do", ses, "-mp", passes, "-mt", "4"]
    print(" ".join(cmd), flush=True)
    subprocess.run(cmd, check=False)
    if not os.path.exists(ses):
        raise SystemExit("no session file from Freerouting")
    pcbnew.ImportSpecctraSES(board, ses)
    fill(board)
    pcbnew.SaveBoard(BOARD, board)
    print(drc(board, os.path.join(work, "drc.rpt")))


if __name__ == "__main__":
    main()
