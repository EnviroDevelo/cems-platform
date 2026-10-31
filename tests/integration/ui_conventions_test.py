#!/usr/bin/env python3
"""The written UI convention (docs/UI_CONVENTIONS.md, CIP-21.7) against the two interfaces that
follow it: the colour table (section 1) must match the touchscreen's #defines in
platform/ui/ui.c and the web page's CSS variables, and the quality words (section 4) must match
the core's names (platform/core/plat_state.c) and the web page's QTXT, with the same word
colours. Every button(...) in ui.c with literal sizes must be at least 56 x 56 px (the
convention's touch-target rule). A change to one side without the other, or to the page without the convention, fails
here. Reads files only: no build, no browser. make test runs it."""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
read = lambda p: open(os.path.join(ROOT, p), encoding="utf-8").read()
DOC, UI, PAGE, STATE = read("docs/UI_CONVENTIONS.md"), read("platform/ui/ui.c"), read("web/index.html"), read("platform/core/plat_state.c")
passed = failed = 0


def check(cond, what):
    global passed, failed
    print(("  ok   " if cond else "  FAIL ") + what)
    if cond: passed += 1
    else: failed += 1


def section(n):
    m = re.search(rf"^## {n}\..*?(?=^## |\Z)", DOC, re.M | re.S)
    return m.group(0) if m else ""


def rows(text):                                   # markdown table rows as lists of cells, header and rule skipped
    out = []
    for line in text.splitlines():
        if line.startswith("|") and not re.match(r"\|\s*-", line):
            out.append([c.strip() for c in line.strip().strip("|").split("|")])
    return out[1:]


# --- section 1: colours ---
palette = {}
for r in rows(section(1)):
    if len(r) >= 3 and re.fullmatch(r"#[0-9A-Fa-f]{6}", r[1]):
        palette[r[0]] = (r[1].lower(), r[2].strip("`"))
check(len(palette) >= 12, f"colour table read ({len(palette)} rows)")

defines = {m.group(1): "#" + m.group(2).lower() for m in re.finditer(r"#define\s+(C_[A-Z]+)\s+0x([0-9A-Fa-f]{6})u", UI)}
root = re.search(r":root\{([^}]*)\}", PAGE)
cssvars = {m.group(1): m.group(2).lower() for m in re.finditer(r"(--[a-z]+):(#[0-9a-fA-F]{3,6})", root.group(1) if root else "")}
cssvars = {k: ("#" + "".join(c * 2 for c in v[1:]) if len(v) == 4 else v) for k, v in cssvars.items()}

for tok, (val, web) in palette.items():
    if tok.startswith("C_"):
        check(defines.get(tok) == val, f"touchscreen {tok} = {val} (ui.c: {defines.get(tok)})")
    if web.startswith("--"):
        check(cssvars.get(web) == val, f"web {web} = {val} (page: {cssvars.get(web)})")
    elif web == "header":
        check(re.search(r"header\{[^}]*background:" + val, PAGE, re.I) is not None, f"web header background = {val}")
for tok, val in defines.items():
    check(tok in palette, f"ui.c {tok} is in the convention's colour table")
for var in cssvars:
    check(any(w == var for _, w in palette.values()), f"page variable {var} is in the convention's colour table")
grey = palette.get("C_GREY", ("",))[0]
check(re.search(r"\.q\{color:" + grey, PAGE, re.I) is not None, f"web greys a non-valid value with C_GREY ({grey})")
neutral = next((v for t, (v, _) in palette.items() if t.startswith("(neutral")), "")
check(neutral and neutral in PAGE.lower(), f"web uses the neutral {neutral} (LOCAL, Cancel)")

# --- section 4: quality words and their colours ---
qrows = [r for r in rows(section(4)) if len(r) == 3]
words = {r[0]: r[1].split(" (")[0] for r in qrows if r[0] != "valid"}
colour = {r[0]: r[2] for r in qrows if r[0] != "valid"}
check(len(words) == 7, f"quality table read ({len(words)} non-valid rows)")

core = re.search(r"\{\s*\"\",([^}]*)\}", STATE)
core_words = re.findall(r"\"([^\"]+)\"", core.group(1)) if core else []
check(core_words == list(words.values()), f"core quality names match the table ({core_words})")

qtxt = re.search(r"const QTXT = \{([^}]*)\}", PAGE)
page_words = dict(re.findall(r"(\w+): \"([^\"]+)\"", qtxt.group(1))) if qtxt else {}
check(sorted(page_words.values()) == sorted(words.values()), f"web quality words match the table ({sorted(page_words.values())})")

markq = re.search(r"function markQ\(.*?\n\}", PAGE, re.S)
mq = markq.group(0) if markq else ""
bad = set(re.findall(r"q === \"(\w+)\"", re.search(r"\"qbad\",([^;]*)", mq).group(1))) if "qbad" in mq else set()
greyq = set(re.findall(r"q === \"(\w+)\"", re.search(r"\"qgrey\",([^;]*)", mq).group(1))) if "qgrey" in mq else set()
key = {v: k for k, v in page_words.items()}                       # word -> the API's quality code
for q, c in colour.items():
    code = key.get(words[q])
    want = "qbad" if c == "C_BAD" else "qgrey" if c == "C_GREY" else "warn"
    got = "qbad" if code in bad else "qgrey" if code in greyq else "warn"
    check(want == got, f"web colours \"{words[q]}\" as {c} ({got})")
check("\"----\"" in mq and "unavailable" in mq, "web shows ---- for an unavailable value")

# --- touch targets: every button(...) in ui.c with literal sizes is at least 56 x 56 px ---
nb = 0
for m in re.finditer(r"\bbutton\(\s*[^,]+,\s*[^,]+,\s*[^,]+,\s*(\d+)\s*,\s*(\d+)\s*,", UI):
    nb += 1
    w, h = int(m.group(1)), int(m.group(2))
    if w < 56 or h < 56:
        check(False, f"ui.c line {UI[:m.start()].count(chr(10)) + 1}: a {w} x {h} px button (minimum 56 x 56)")
check(nb >= 50, f"touch targets: {nb} buttons with literal sizes checked")

print(f"UI CONVENTIONS (CIP-21.7): {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
