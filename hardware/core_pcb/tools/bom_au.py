#!/usr/bin/env python3
"""Purchasing BOM for Australian suppliers: ../bom_au.csv from design.py.

Each line keeps the design's part number where it has one. Generic passives get a full
parametric description instead of a guessed part number. Every line carries a
search link at the preferred AU distributor: Digi-Key AU, or element14 AU for Phoenix and
Littelfuse. Both ship in AUD. Stock and price are not checked here: do that at order time.
"""
import collections
import csv
import os
import re
import sys
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import design  # noqa: E402

DK = "https://www.digikey.com.au/en/products/result?keywords="
E14 = "https://au.element14.com/search?st="
SIZE = re.compile(r"_(0402|0603|0805|1206|1210|1812|2010|2512)_")


def describe(p):
    fp = p["footprint"].split(":")[1]
    size = SIZE.search("_" + fp + "_")
    size = size.group(1) if size else ""
    v = p["value"]
    sym = p["symbol"].split(":")[1]
    if sym == "R":
        tol = "0.1 % thin film" if "0.1%" in v else "1 % thick film"
        return f"Resistor {v.split()[0]} {size} {tol}" + (f", {v.split()[1]}" if len(v.split()) > 1 and "%" not in v.split()[1] else "")
    if sym in ("C", "C_Polarized", "CP"):
        parts = v.split()
        volt = next((x for x in parts[1:] if x.endswith("V")), "50V" if size in ("0603", "0805") else "")
        diel = "C0G" if "C0G" in v else ("aluminium electrolytic" if fp.startswith("CP_") else "X7R")
        return f"Capacitor {parts[0]} {volt} {diel} {size}".strip()
    return ""


# Parts the design names only by value: a suggested AU-stocked part (CHECK at order time)
SUGGEST = {
    "24VDC IN": "Phoenix Contact MKDS 1,5/3-5,08 (1715734)",
    "600R@100MHz": "Murata BLM18KG601SN1D",
    "25MHz": "Abracon ABM8-25.000MHZ-B2-T (CHECK load capacitance vs the 33 pF caps)",
    "32.768kHz": "Abracon ABS07-32.768KHZ-T",
    "RESET": "Omron B3U-1000P",
    "BOOT0": "2.54 mm 1x2 pin header + jumper",
    "SWD": "Samtec FTSH-105-01-L-DV-K (1.27 mm 2x5 SMD)",
    "SERVICE UART": "2.54 mm 1x6 pin header",
    "ROLE": "2.54 mm 1x3 pin header + jumper",
    "TACH PULL-UP": "2.54 mm 1x2 pin header + jumper",
    "green RUN": "Kingbright APT1608SGC", "green FIELD A": "Kingbright APT1608SGC",
    "green FIELD B": "Kingbright APT1608SGC", "red FAULT": "Kingbright APT1608EC",
}
PN = re.compile(r"^[A-Z0-9][A-Z0-9\-.]*\d[A-Z0-9\-.]*$")


def main():
    groups = collections.OrderedDict()
    for p in design.PARTS:
        key = (p["value"], p["footprint"], p["mpn"], p["dnp"])
        groups.setdefault(key, []).append(p)
    rows = []
    for (value, fp, mpn, dnp), ps in groups.items():
        refs = sorted((x["ref"] for x in ps), key=lambda r: (re.sub(r"\d", "", r), int(re.sub(r"\D", "", r) or 0)))
        desc = (mpn or describe(ps[0]) or SUGGEST.get(value) or (value if PN.match(value) else "")
                or f"{value} ({fp.split(':')[1]}): choose part, CHECK")
        query = (mpn.split("(")[0].split(" + ")[0].split(";")[0].strip() if mpn else desc)
        vendor = "element14 AU" if re.search(r"Phoenix|Littelfuse|Keystone", desc + fp) else "Digi-Key AU"
        url = (E14 if vendor.startswith("element14") else DK) + urllib.parse.quote(query)
        rows.append([len(ps), " ".join(refs), value, fp.split(":")[1], desc, vendor, url,
                     "no (fit-optional)" if dnp else "yes", "CHECK" if "CHECK" in desc else ""])
    out = os.path.join(os.path.dirname(HERE), "bom_au.csv")
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Qty", "References", "Value", "Footprint", "Part / description", "AU supplier", "Search link",
                    "Fitted", "Open check"])
        w.writerows(rows)
    print(f"{len(rows)} lines, {sum(r[0] for r in rows)} parts -> {out}")


if __name__ == "__main__":
    main()
