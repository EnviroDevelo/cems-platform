#!/usr/bin/env python3
"""cip_coverage.py - which requirements of the Common Instrument Platform Specification have a
test that names them (spec section 9.3, "requirement IDs in tests").

Reads the requirement table in docs/INSTRUMENT_PLATFORM_SPEC.md and every test (tests/*.c,
tests/integration/*.py) for "CIP-nn.m" (a list such as "CIP-22.2, 22.4, 23.4" counts each).
An M (mandatory) requirement must be named by a test, unless:
  - its status says it is not the firmware's to test: "n/a", "Done (design...)",
    "Done (exception)"; or
  - it is listed in tools/cip_untested.txt with the reason (known gaps, so the list can only
    shrink).
Exit status 1 if an M requirement has no test and no entry, or an entry names a requirement
that now has a test (take it off the list). make test runs it.

  tools/cip_coverage.py            check, one line per problem
  tools/cip_coverage.py --report   every requirement with the tests that name it
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPEC = os.path.join(ROOT, "docs", "INSTRUMENT_PLATFORM_SPEC.md")
ALLOW = os.path.join(ROOT, "tools", "cip_untested.txt")


def requirements():
    reqs = {}
    for line in open(SPEC, encoding="utf-8"):
        m = re.match(r"\|\s*(CIP-\d\d\.\d+)\s*\|\s*([^|]*)\|\s*([MSO])\s*\|\s*([^|]*)\|", line)
        if m:
            reqs[m.group(1)] = (m.group(2).strip(), m.group(3), m.group(4).strip())
    return reqs


def named_in_tests():
    hits = {}
    files = glob.glob(os.path.join(ROOT, "tests", "*.c")) + glob.glob(os.path.join(ROOT, "tests", "integration", "*.py"))
    for f in sorted(files):
        text = open(f, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"CIP-(\d\d)\.(\d+)((?:\s*(?:,|and|/)\s*(?:CIP-)?\d\d\.\d+)*)", text):
            ids = ["CIP-%s.%s" % (m.group(1), m.group(2))]
            ids += ["CIP-" + x for x in re.findall(r"(\d\d\.\d+)", m.group(3))]
            for i in ids:
                hits.setdefault(i, set()).add(os.path.relpath(f, ROOT))
    return hits


def exempt(status):
    s = status.lower()
    return s.startswith("n/a") or s.startswith("done (design") or s.startswith("done (exception")


def allowlist():
    out = {}
    if os.path.exists(ALLOW):
        for line in open(ALLOW, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line:
                rid, _, why = line.partition(" ")
                out[rid] = why.strip()
    return out


def main():
    reqs, hits, allow = requirements(), named_in_tests(), allowlist()
    if "--report" in sys.argv:
        for rid, (title, lvl, status) in reqs.items():
            print(f"{rid} {lvl} {status:<12} {', '.join(sorted(hits.get(rid, []))) or '-'}  {title}")
    problems = []
    for rid, (title, lvl, status) in reqs.items():
        if lvl != "M" or exempt(status):
            continue
        if rid not in hits and rid not in allow:
            problems.append(f"{rid} ({title}) is mandatory and no test names it: add the ID to the test that proves it, "
                            f"or list it in tools/cip_untested.txt with the reason")
        if rid in hits and rid in allow:
            problems.append(f"{rid} now has a test ({', '.join(sorted(hits[rid]))}): take it off tools/cip_untested.txt")
    for rid in allow:
        if rid not in reqs:
            problems.append(f"tools/cip_untested.txt names {rid}, which is not in the spec")
    for p in problems:
        print(p)
    m_total = sum(1 for r in reqs.values() if r[1] == "M" and not exempt(r[2]))
    m_tested = sum(1 for rid, r in reqs.items() if r[1] == "M" and not exempt(r[2]) and rid in hits)
    print(f"requirement IDs in tests: {m_tested} of {m_total} mandatory requirements named by a test, "
          f"{len(allow)} known gaps listed")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
