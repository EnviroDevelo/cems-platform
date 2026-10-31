#!/usr/bin/env python3
"""Project footprints missing from the KiCad 7 libraries, written to cems_core.pretty.

Infineon_PG-VQFN-70_12x12mm_P0.5mm is a PLACEHOLDER: 18/17/18/17 pads counter-clockwise
from pin 1 at the top of the left side, 0.5 mm pitch, no exposed pad. It reserves the
board area for placement only and MUST be redrawn from the ISO2H823V2.5 package drawing
(PG-VQFN-70) before any routing.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "cems_core.pretty")


def vqfn70():
    body, pitch, pl, pw = 12.0, 0.5, 0.8, 0.25
    sides = [18, 17, 18, 17]  # left, bottom, right, top
    lines = ['(footprint "Infineon_PG-VQFN-70_12x12mm_P0.5mm" (version 20221018) (generator cems_tools)',
             '  (layer "F.Cu")',
             '  (descr "PLACEHOLDER for Infineon PG-VQFN-70 (ISO2H823V2.5). Redraw from the package drawing before routing")',
             '  (attr smd)',
             '  (fp_text reference "REF**" (at 0 -7.5) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))',
             '  (fp_text value "Infineon_PG-VQFN-70_12x12mm_P0.5mm" (at 0 7.5) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))',
             '  (fp_text user "PLACEHOLDER" (at 0 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))']
    h = body / 2
    lines.append(f'  (fp_rect (start {-h} {-h}) (end {h} {h}) (stroke (width 0.1) (type default)) (fill none) (layer "F.Fab"))')
    lines.append(f'  (fp_rect (start {-h - 0.75} {-h - 0.75}) (end {h + 0.75} {h + 0.75}) (stroke (width 0.05) (type default)) (fill none) (layer "F.CrtYd"))')
    lines.append(f'  (fp_circle (center {-h - 0.6} {-h + 0.2}) (end {-h - 0.45} {-h + 0.2}) (stroke (width 0.12) (type default)) (fill solid) (layer "F.SilkS"))')
    n = 1
    cx = h - 0.35  # pad centre just inside the body edge
    for side, count in enumerate(sides):
        span = (count - 1) * pitch
        for i in range(count):
            t = -span / 2 + i * pitch
            if side == 0:
                x, y, w, hh = -cx, t, pl, pw
            elif side == 1:
                x, y, w, hh = t, cx, pw, pl
            elif side == 2:
                x, y, w, hh = cx, -t, pl, pw
            else:
                x, y, w, hh = -t, -cx, pw, pl
            lines.append(f'  (pad "{n}" smd roundrect (at {x:.3f} {y:.3f}) (size {w} {hh}) (layers "F.Cu" "F.Paste" "F.Mask") (roundrect_rratio 0.25))')
            n += 1
    lines.append(")")
    return "Infineon_PG-VQFN-70_12x12mm_P0.5mm", "\n".join(lines) + "\n"


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for name, text in (vqfn70(),):
        with open(os.path.join(OUT, f"{name}.kicad_mod"), "w") as f:
            f.write(text)
    print("footprints written to", OUT)
