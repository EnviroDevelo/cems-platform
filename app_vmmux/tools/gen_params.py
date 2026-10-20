#!/usr/bin/env python3
"""Generate the VM/MUX parameter table and retained blocks (APP_INTEGRATION.md section 5).

  SETTING tags (data dictionary role)  -> param_def_t entries   IDs 1000..2999
  other retained tags                  -> RETAIN() blocks        IDs 3000..
  tags no chunk declares (UI / Modbus only) are defined here too

Outputs: src/vm_params.c, include/vm_params.h, tools/out/params_report.csv
IDs are kept in tools/param_ids.csv: the FRAM store finds records by ID, so an ID once
given is never reused or changed; new tags get the next free number.

Ranges (never guessed, Rule 0). Each entry's report row says where its range came from:
  HMI_CONST   Numeric Box with constant limits (#1..#20)
  HMI_TAG     Numeric Box whose limit is another tag: static range = the type's, plus a
              generated interlock that checks the tag at apply time (as the panel did)
  COMBO       Combo box with N items: 0..N. Whether item 1 stores 0 or 1 is open item 12,
              so the range admits both readings (0..N-1 and 1..N); nothing outside either
  CHECKBOX    bool
  NO_LIMIT    the original panel imposed no limit: the type's full range, as the original
              accepted. Listed for the owner to set real limits (INTEGRATION.md Q2)
Levels: the old panel had no user levels (50_HMI_Map U-list); everything except the
main-screen name boxes was behind the settings password -> ADMIN; names editable on the
main screens with no password -> OPERATOR.
Not in the table (replaced by platform parameters): Password (logins), SN (serial,
param 50), ToEmail1..3 (mail recipients, param 78), Brigthness (touchscreen, platform UI).
"""
import csv, os, re, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "..")
REPLACED = {"Password": "platform logins", "SN": "platform serial number (param 50)",
            "ToEmail1": "platform mail recipients (param 78)", "ToEmail2": "platform mail recipients (param 78)",
            "ToEmail3": "platform mail recipients (param 78)", "Brigthness": "touchscreen brightness (platform UI)",
            "Read Request": "old Modbus client test screen (HMI only, no logic; the new spec has no Modbus client)"}
OPERATOR_SCREENS = re.compile(r"^(MUX_Main1_10|Main)")

def cname_key(n):            # JSON / label key
    k = re.sub(r"[^A-Za-z0-9_]", "_", n)
    return re.sub(r"_+", "_", k).strip("_")

def macro_id(n):
    return "VMP_" + cname_key(n).upper()

def base_tag(t):
    return re.sub(r"\[.*", "", t).split(".")[0]

def type_range(t):
    return {"INT16": (-32768, 32767), "UINT8": (0, 255), "INT8": (-128, 127), "UINT16": (0, 65535),
            "UINT32": (0, 4294967295), "TIMER": (0, 4294967295), "REAL": (-3.4e38, 3.4e38)}[t]

def ct(t):
    return {"INT16": "CT_I16", "UINT8": "CT_U8", "INT8": "CT_I8", "UINT16": "CT_U16", "UINT32": "CT_U32",
            "TIMER": "CT_U32"}[t]

def num(v):
    v = v.strip()
    return float(v) if re.search(r"[.eE]", v) else int(v)

def main():
    ddp, hmip = sys.argv[1], sys.argv[2]
    dd = {r["name"]: r for r in csv.DictReader(open(ddp, newline="", encoding="utf-8"))}
    tmap = {}
    for r in csv.DictReader(open(os.path.join(HERE, "out", "tag_map.csv"))):
        if r["dictionary_name"]: tmap[r["dictionary_name"]] = r["c_name"]
    els = defaultdict(list)
    for e in csv.DictReader(open(hmip, newline="", encoding="utf-8")):
        if e["tag"]: els[base_tag(e["tag"])].append(e)

    # ---- ID registry ----
    regp = os.path.join(HERE, "param_ids.csv")
    reg = {}
    if os.path.exists(regp):
        for r in csv.DictReader(open(regp)): reg[r["dictionary_name"]] = int(r["id"])
    used = set(reg.values())
    def get_id(name, lo, hi):
        if name in reg: return reg[name]
        i = lo
        while i in used: i += 1
        if i > hi: sys.exit(f"gen_params: no free ID for {name}")
        reg[name] = i; used.add(i); return i

    params, blocks, extra_defs, report, interlocks, shadows = [], [], [], [], [], []
    def cexpr(n):
        """C lvalue of a dictionary tag: from the chunk tag map, or a new definition here."""
        if n in tmap: return tmap[n]
        c = "vmx_" + cname_key(n)
        r = dd[n]
        base, dims = re.match(r"^(\w+)((?:\[\d+\])*)$", r["c_type"]).groups()
        if r["dims"] and not dims: dims = "".join(f"[{d}]" for d in r["dims"].split("x"))
        init = ""
        if r["type"] == "STRING-UTF-16" and r["power_up"]:
            init = " = { " + ", ".join(str(ord(ch)) for ch in r["power_up"]) + " }"
        elif r["type"] == "STRING-ASCII" and r["power_up"]:
            init = ' = "' + r["power_up"].replace('"', '\\"') + '"'
        extra_defs.append((n, f"{base} {c}{dims}{init};", f"extern {base} {c}{dims};", r))
        tmap[n] = c
        return c

    settings = sorted((r for r in dd.values() if r["role"] == "SETTING"), key=lambda r: r["name"])
    for r in settings:
        n, t = r["name"], r["type"]
        if n in REPLACED:
            report.append([n, "", t, "-", "-", "-", f"REPLACED: {REPLACED[n]}"]); continue
        if t == "Schedule":                       # struct: retained block, edited by the schedule screen
            continue                              # handled with the retained blocks below
        var = cexpr(n)
        pid = get_id(n, 1000, 2999)
        es = [e for e in els.get(n, []) if e["read_only"] != "1"]
        lvl = "PLAT_LVL_OPERATOR" if es and all(OPERATOR_SCREENS.match(e["screen"]) for e in es) else "PLAT_LVL_ADMIN"
        key = cname_key(n)
        pu = r["power_up"]
        src, lo, hi, il = "", None, None, None
        if t == "BIT":
            src = "CHECKBOX"
            if r["dims"]:
                params.append(f'PARAM_ARRAY_BOOL({macro_id(n)}, "{key}", {var}, {lvl}, PF_PERSIST)')
            else:
                d = "true" if pu not in ("", "0") else "false"
                params.append(f'PARAM_BIT_D({macro_id(n)}, "{key}", {var}, {d}, {lvl}, PF_PERSIST)')
        elif t in ("STRING-ASCII", "STRING-UTF-16"):
            src = "TEXT"
            m = "PARAM_TEXT" if t == "STRING-ASCII" else "PARAM_TEXT16"
            dflt = "NULL"
            if pu:                                  # default = the power-up text (shadow copy, see below)
                shadows.append((n, var)); dflt = f"sh_{cname_key(n)}"
            params.append(f'{m}({macro_id(n)}, "{key}", {var}, {dflt}, {lvl}, PF_PERSIST)')
        else:
            tlo, thi = type_range(t)
            nb = [e for e in es if e["type"] == "Numeric Box"]
            cb = [e for e in es if e["type"] == "ComboBox"]
            tb = [e for e in es if e["type"] == "Timer Box"]
            if nb:
                mins = {e["min"] for e in nb}; maxs = {e["max"] for e in nb}
                if len(mins) > 1 or len(maxs) > 1: sys.exit(f"gen_params: {n}: different limits on different screens {mins} {maxs}")
                mn, mx = mins.pop(), maxs.pop()
                if mn.startswith("#") and mx.startswith("#"):
                    src, lo, hi = "HMI_CONST", num(mn[1:]), num(mx[1:])
                elif mn == "" and mx == "":
                    src, lo, hi = "NO_LIMIT", tlo, thi
                else:
                    lo = num(mn[1:]) if mn.startswith("#") else tlo
                    hi = num(mx[1:]) if mx.startswith("#") else thi
                    src = "HMI_TAG"
                    il = (mn if not mn.startswith("#") and mn else None, mx if not mx.startswith("#") and mx else None)
            elif cb:
                counts = {len([x for x in e["text"].split("|") if x.strip()]) for e in cb}
                src, lo, hi = "COMBO", 0, max(counts)
            elif tb or t == "TIMER":
                src, lo, hi = "NO_LIMIT", tlo, thi
            else:
                src, lo, hi = "NO_LIMIT", tlo, thi
            if t == "TIMER":
                var = f"{var}.Preset"
                dflt = int(re.search(r"\.Preset=(\d+)", r["element_power_up"]).group(1)) if "Preset=" in r["element_power_up"] else 0
                params.append(f'PARAM_UINT({macro_id(n)}, "{key}", {var}, CT_U32, {lo}u, {hi}u, {dflt}u, {lvl}, PF_PERSIST, "ms")')
            elif t == "REAL":
                if r["dims"]:
                    params.append(f'PARAM_ARRAY_REAL({macro_id(n)}, "{key}", {var}, {lo:g}f, {hi:g}f, {lvl}, PF_PERSIST)')
                else:
                    d = float(pu) if pu else 0.0
                    params.append(f'PARAM_REAL({macro_id(n)}, "{key}", {var}, {lo:g}f, {hi:g}f, {d:g}f, NULL, {lvl}, PF_PERSIST)')
            else:
                signed = t in ("INT16", "INT8")
                if r["dims"]:
                    m = "PARAM_ARRAY_INT" if signed else "PARAM_ARRAY_UINT"
                    params.append(f'{m}({macro_id(n)}, "{key}", {var}, {ct(t)}, {lo}, {hi}, {lvl}, PF_PERSIST)')
                else:
                    d = int(pu) if pu else 0
                    if d < lo or d > hi:
                        # The original's power-up value is outside the panel's limits (0 = "not configured
                        # yet", which sends the panel to System Config at power-up). Keep it as the default;
                        # the static range must contain the default (param_init), so the panel's own limit
                        # is enforced on every external write by the interlock instead.
                        src += f"+DEFAULT_OUTSIDE({d})"
                        plo, phi = lo, hi
                        lo = min(lo, d); hi = max(hi, d)
                        il = il or (None, None)
                        il = (il[0] if il[0] else (f"#{plo}" if plo != lo else None),
                              il[1] if il[1] else (f"#{phi}" if phi != hi else None))
                    m = "PARAM_INT" if signed else "PARAM_UINT"
                    sfx = "" if signed else "u"
                    params.append(f'{m}({macro_id(n)}, "{key}", {var}, {ct(t)}, {lo}{sfx}, {hi}{sfx}, {d}{sfx}, {lvl}, PF_PERSIST, NULL)')
            if il:
                if t not in ("INT16", "INT8", "UINT8") or r["dims"]: sys.exit(f"gen_params: {n}: interlock on an unexpected type")
                fn = "il_" + cname_key(n)
                lo_t, hi_t = il
                conds = []
                def lim(x): return x[1:] if x.startswith("#") else f"(int32_t){cexpr(x)}"
                if lo_t: conds.append(f"v.i < {lim(lo_t)}")
                if hi_t: conds.append(f"v.i > {lim(hi_t)}")
                interlocks.append((fn, n, " || ".join(conds), lo_t, hi_t))
                signed = t in ("INT16", "INT8")
                fld = "i" if signed else "u"
                pt = "PT_INT" if signed else "PT_UINT"
                params[-1] = (f"{{ .id = {macro_id(n)}, .ctype = {ct(t)}, .ptype = {pt}, .flags = PF_PERSIST, .level = {lvl}, "
                              f".ptr = &({var}), .vmin.{fld} = {lo}, .vmax.{fld} = {hi}, .vdef.{fld} = {d}, "
                              f'.name = "{key}", .unit = NULL, .interlock = {fn} }}')
        report.append([n, pid, t, src, lo if lo is not None else "", hi if hi is not None else "", lvl])

    # ---- retained blocks: every retained tag that is not a parameter ----
    pnames = {row[0] for row in report if not str(row[-1]).startswith("REPLACED")}
    for r in sorted(dd.values(), key=lambda r: r["name"]):
        n = r["name"]
        if r["retain"] != "1" or n in pnames or n in REPLACED: continue
        var = cexpr(n)
        bid = get_id(n, 3000, 8999)
        shadows.append((n, var))
        blocks.append(f'RETAIN({macro_id(n)}, "{cname_key(n)}", {var}, 0, sh_{cname_key(n)})')
        report.append([n, bid, r["type"], "RETAIN", "", "", ""])

    # ---- write ----
    with open(regp, "w", newline="") as f:
        w = csv.writer(f); w.writerow(["id", "dictionary_name"])
        for n, i in sorted(reg.items(), key=lambda x: x[1]): w.writerow([i, n])

    ids = sorted(((reg[row[0]], row[0]) for row in report if row[1] != ""), key=lambda x: x[0])
    with open(os.path.join(APP, "include", "vm_params.h"), "w") as f:
        f.write("/* vm_params.h - GENERATED by tools/gen_params.py. Parameter and retained-block IDs\n"
                " * (fixed for ever: tools/param_ids.csv) and the tags only the UI / Modbus use. */\n"
                "#ifndef VM_PARAMS_H\n#define VM_PARAMS_H\n#include \"vm_types.h\"\n\nenum {\n")
        for i, n in ids: f.write(f"    {macro_id(n):<44} = {i},   /* {n} */\n")
        f.write("};\n\nvoid vm_params_snapshot(void);   /* app init: capture power-up values as defaults */\n\n/* tags no chunk declares (UI, Modbus) */\n")
        for n, d, e, r in extra_defs: f.write(f"{e:<56} /* {n} */\n")
        f.write("\n#endif\n")

    with open(os.path.join(APP, "src", "vm_params.c"), "w") as f:
        f.write("/* vm_params.c - GENERATED by tools/gen_params.py from Analysis/stage2/data_dictionary.csv\n"
                " * and stage5/hmi_elements.csv. Do not edit; change the generator. Range sources and\n"
                " * levels per entry: tools/out/params_report.csv. */\n"
                '#include <string.h>\n#include "vm_app.h"\n#include "vm_tags.h"\n#include "vm_params.h"\n\n'
                "/* ---- tags no chunk declares (UI, Modbus), power-up values from the dictionary ---- */\n")
        for n, d, e, r in extra_defs: f.write(f"{d:<64} /* {n} */\n")
        f.write("\n/* ---- interlocks: limits the old panel took from another tag (HMI_TAG) ---- */\n")
        for fn, n, cond, lo_t, hi_t in interlocks:
            f.write(f"static int {fn}(uint16_t id, plat_val_t v) {{ (void)id; return ({cond}) ? 1 : 0; }}"
                    f"   /* {n}: {lo_t or ''}..{hi_t or ''} */\n")
        f.write("\n/* ---- defaults of text entries and retained blocks: copies of the tags' power-up values ----\n"
                " * The store resets a block (and restore-defaults a text) from .dflt, which must hold the full\n"
                " * size. vm_params_snapshot() fills these from the tags' static initialisers; it runs from the\n"
                " * application init hook, which the platform calls before param_init / the FRAM load. */\n")
        for n, var in shadows:
            f.write(f"static uint8_t sh_{cname_key(n)}[sizeof({var})];\n")
        f.write("\nvoid vm_params_snapshot(void)\n{\n    static bool done;\n    if (done) return;\n    done = true;\n")
        for n, var in shadows:
            f.write(f"    memcpy(sh_{cname_key(n)}, &({var}), sizeof sh_{cname_key(n)});\n")
        f.write("}\n")
        f.write("\nstatic const param_def_t defs[] = {\n")
        for p in params:
            f.write(f"    {p},\n")
        f.write("};\nconst param_def_t *const vm_params = defs;\nconst size_t vm_n_params = sizeof defs / sizeof defs[0];\n\n")
        f.write("static const retain_def_t blocks[] = {\n")
        for b in blocks: f.write(f"    {b},\n")
        f.write("};\nconst retain_def_t *const vm_retain = blocks;\nconst size_t vm_n_retain = sizeof blocks / sizeof blocks[0];\n")

    os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
    with open(os.path.join(HERE, "out", "params_report.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(["dictionary_name", "id", "type", "range_source", "min", "max", "level_or_note"])
        w.writerows(report)
    print(f"gen_params: {len(params)} parameters, {len(blocks)} retained blocks, {len(extra_defs)} new tags, "
          f"{len(interlocks)} interlocks")

if __name__ == "__main__":
    main()
