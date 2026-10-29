#!/usr/bin/env python3
"""ui_conventions_test.py - the touchscreen and the web page still follow docs/UI_CONVENTIONS.md
(CIP-21.7, 21.4). No build or network needed; make test runs it.

Checks:
  1. Palette: every token in the convention's colour table has the same value in
     platform/ui/ui.c (#define C_...) and, where the table names one, in the web page's CSS
     variables (web/index.html).
  2. Quality words: the words in the convention's quality table are the ones
     plat_quality_name() returns (platform/core/plat_state.c), in the same order.
  3. Touch targets: every button(...) in ui.c with literal sizes is at least 56 x 56 px.
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(*p):
    return open(os.path.join(ROOT, *p), encoding="utf-8").read()


def main():
    doc, ui, web, state = read("docs", "UI_CONVENTIONS.md"), read("platform", "ui", "ui.c"), read("web", "index.html"), read("platform", "core", "plat_state.c")
    bad = []

    # 1. palette
    defines = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"#define\s+(C_[A-Z]+)\s+0x([0-9A-Fa-f]{6})u", ui)}
    css = {}
    for m in re.finditer(r"--([a-z]+)\s*:\s*#([0-9A-Fa-f]{3,6})\b", web):
        h = m.group(2)
        if len(h) == 3: h = "".join(c * 2 for c in h)
        css.setdefault(m.group(1), int(h, 16))         # the first (light theme) value
    rows = re.findall(r"^\|\s*(C_[A-Z]+)\s*\|\s*#([0-9A-Fa-f]{6})\s*\|\s*([^|]*)\|", doc, re.M)
    if len(rows) < 10: bad.append("colour table: only %d rows found in UI_CONVENTIONS.md" % len(rows))
    for tok, val, webcol in rows:
        v = int(val, 16)
        if tok not in defines: bad.append("%s is in the convention but not defined in ui.c" % tok)
        elif defines[tok] != v: bad.append("%s: ui.c has #%06X, the convention #%06X" % (tok, defines[tok], v))
        w = re.search(r"--([a-z]+)", webcol)
        if w:
            if w.group(1) not in css: bad.append("%s: web CSS variable --%s is missing" % (tok, w.group(1)))
            elif css[w.group(1)] != v: bad.append("%s: web --%s is #%06X, the convention #%06X" % (tok, w.group(1), css[w.group(1)], v))
    print("palette: %d tokens checked" % len(rows))

    # 2. quality words
    m = re.search(r"q_name\[PLAT_Q_COUNT\]\s*=\s*\{([^}]*)\}", state)
    names = re.findall(r'"([^"]*)"', m.group(1)) if m else []
    table = re.search(r"\| Quality \| Word \|.*?\n((?:\|.*\n)+)", doc)
    words = []
    for line in (table.group(1).splitlines() if table else []):
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 2 or set(cells[0]) <= set("-"): continue
        w = cells[1]
        words.append("" if w.startswith("(none)") else re.sub(r"\s*\(.*\)$", "", w))
    if not names: bad.append("plat_state.c: q_name table not found")
    elif words != names: bad.append("quality words: convention %r, firmware %r" % (words, names))
    print("quality words: %d checked" % len(words))

    # 3. touch targets
    n = 0
    for m in re.finditer(r"\bbutton\(\s*[^,]+,\s*[^,]+,\s*[^,]+,\s*(\d+)\s*,\s*(\d+)\s*,", ui):
        n += 1
        w, h = int(m.group(1)), int(m.group(2))
        if w < 56 or h < 56:
            bad.append("ui.c line %d: a %d x %d px button (minimum 56 x 56)" % (ui[:m.start()].count("\n") + 1, w, h))
    print("touch targets: %d buttons with literal sizes checked" % n)

    for b in bad: print("FAIL: " + b)
    print("ui conventions: %s" % ("FAILED" if bad else "ok"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
