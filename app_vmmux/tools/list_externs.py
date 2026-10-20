#!/usr/bin/env python3
"""List every application tag declared extern in the chunk headers (via clang's AST),
as name<TAB>C type<TAB>header. Used to generate vm_tags.c and to check types against
stage2/data_dictionary.csv."""
import json, subprocess, sys, os
here = os.path.dirname(os.path.abspath(__file__))
fa = os.path.join(here, "..", "faithful")
hdrs = sorted(f for f in os.listdir(fa) if f.endswith(".h") and f != "ul_lf.h")
src = "".join(f'#include "{h}"\n' for h in hdrs)
cmd = ["clang", "-std=c11", "-x", "c", "-fsyntax-only", "-Xclang", "-ast-dump=json",
       "-I", fa, "-I", os.path.join(here, "..", "include"), "-I", os.path.join(here, "..", "..", "platform", "include"), "-"]
ast = json.loads(subprocess.run(cmd, input=src, capture_output=True, text=True, check=True).stdout)
seen = {}
cur = None
for d in ast["inner"]:
    f = d.get("loc", {}).get("file")
    if f: cur = f
    if d.get("kind") == "VarDecl" and d.get("storageClass") == "extern":
        if cur and os.path.basename(cur) in hdrs:
            n = d["name"]; t = d["type"]["qualType"]
            seen.setdefault(n, (t, os.path.basename(cur)))
for n, (t, h) in sorted(seen.items()):
    print(f"{n}\t{t}\t{h}")
