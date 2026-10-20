#!/usr/bin/env python3
"""Generate app_vmmux/src/vm_tags.c: the ONE definition of every application tag the
Stage 4 chunks declare extern, with the original power-up values.

Sources
  - the chunk headers (faithful/*.h), read through clang's AST (list_externs.py)
  - Analysis/stage2/data_dictionary.csv: type, power_up, element_power_up, retain

Rules (Rule 0: nothing guessed)
  - A header name that is a dictionary tag must have the dictionary's C type, or the
    script stops.
  - A header name that stands for numbered tags (DwellT[k-1] = SequenceMUXDwellTimer<k>)
    is listed in GROUPS below; every element must exist in the dictionary with the
    element type, or the script stops.
  - Names that are neither are listed in NOT_TAGS with the reason, or the script stops.
  - Power-up values come only from the dictionary. Timers get Preset = Current = power-up
    Preset (the Stage 4 definitions did the same, A4.10 reloads Current on start anyway).

Usage: tools/gen_tags.py <data_dictionary.csv>   (writes src/vm_tags.c and tools/out/tag_map.csv)
"""
import csv, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "..")

def rng(fmt, a, b):
    return [fmt.format(k) for k in range(a, b + 1)]

# header name -> dictionary names, element by element (row-major for 2-D strings)
GROUPS = {
    "AcknowledgeBit": ["Acknowledge Bit"],
    "AnalogTimer": rng("AnalogTimer{}", 0, 3),
    "TempTimer": rng("TempTimer{}", 0, 3),
    "DwellT": rng("SequenceMUXDwellTimer{}", 1, 20),
    "SampleT": rng("SequenceMUXSampleTimer{}", 1, 20),
    "FastLoopT": rng("SequenceMUXFastLoopTimer{}", 1, 20),
    "Timer1_7": rng("Timer{}", 1, 7),
    "SampleTimer1_7": rng("SampleTimer{}", 1, 7),
    "FastLoopTimer1_7": rng("FastLoopTimer{}", 1, 7),
    "EntryRow": rng("EntryRow{}", 1, 7),
    "EntryRowMUX": rng("EntryRowMUX{}", 1, 7),
    "Name1_7": rng("Name{}", 1, 7),
    "InletName": rng("InletName{}", 0, 18),
    "OutletName": rng("OutletName{}", 0, 18),
    "SingularInlet": rng("SingularInlet{}", 0, 18),
    "Par": rng("Par{}", 0, 19),
    "ParamColName": ["AI0ColNumberName", "AI1ColNumberName", "RTD0ColNumberName", "RTD1ColNumberName"]
                    + rng("ParameterName{}", 4, 19),
    "FirstInterconInletName": rng("FirstInterconInletName{}", 1, 19),
    "FirstInterconOutletName": rng("FirstInterconOutletName{}", 1, 19),
    "SecondInterconInletName": rng("SecondInterconInletName{}", 1, 18),
    "SecondInterconOutletName": rng("SecondInterconOutletName{}", 1, 18),
    "T1": rng("Sequence1Timer{}", 1, 20),
    "T2": rng("Sequence2Timer{}", 1, 20),
    "Stop1": rng("Sequence1Stop{}", 1, 19),
    "Stop2": rng("Sequence2Stop{}", 1, 19),
    "ManualTimer": ["Manual Timer"],
    "ManualTimer2": ["Manual Timer2"],
    "ManualTimerMUX": ["Manual TimerMUX"],
    "SelectAllDays": ["select all days"],
    "index_": ["index"],
    "MailStatus": ["Mail Status"],
    "MailStatusCode": ["Mail Status Code"],
}

# declared in the headers but not tags of the original project
NOT_TAGS = {
    # system tags: scan_engine.c owns them (ul_system.h)
    "g_dt_ms": "platform", "Ladder_Initial_Cycle": "platform", "RTC_Date": "platform", "RTC_Time": "platform",
    # physical I/O images: application tags filled by io_in / read by io_out (10_IO_Map)
    "TA30_DI": "io", "UID_DI": "io", "TA30_DO": "io", "UID_DO": "io",
    "TA30_AI": "io", "TA30_AI_Status": "io", "TA30_TEMP": "io",
    # General.* HMI bits (PLATFORM.md rule 4): the application defines them until the UI supplies them
    "Touched_Bit": "hmi", "VNC_Touched_Bit": "hmi",
    # application additions declared in a chunk header, defined in their own file
    "vm_mux_enable": "model",
    # diagnostics counters (vm_types.h)
    "g_oob": "diag", "g_div0": "diag", "g_hang": "diag", "g_out_of_range_writes": "diag",
    # assumption switches of the Stage 4 models: fixed at their default reading
    "g_tag2buf_whole": "assume:false", "g_dt_clear_sync": "assume:false", "g_setcol_whole": "assume:false",
    # models of UniLogic platform objects, defined in the model files (see faithful/README)
    "LoggingTable": "model", "LoggingTable_Status": "model", "g_reserv": "model",
    "Is_SD_Present": "model", "Is_SD_Locked": "model",
    "Mail": "model", "MailName": "model", "g_sent": "model", "g_mail_fail": "model",
    "g_csv": "model", "g_csv_lines": "model", "g_inlets_csv": "model", "g_inlets_lines": "model",
    "g_outlets_csv": "model", "g_outlets_lines": "model",
    # Stage 4 test-only counters
    "g_alarm_desc_calls": "model", "g_twoorfb22_calls": "diag",
}

def die(msg):
    sys.exit("gen_tags: " + msg)

def parse_type(t):
    t = t.replace("_Bool", "bool").replace(" [", "[")
    m = re.match(r"^(.*?)((?:\[\d+\])*)$", t.strip())
    return m.group(1).strip(), [int(x) for x in re.findall(r"\[(\d+)\]", m.group(2))]

def dd_ctype(row):
    base, dims = parse_type(row["c_type"])
    if row["dims"] and not dims:
        dims = [int(x) for x in row["dims"].split("x")]
    return base, dims

def c_number(base, v):
    v = v.strip()
    if base == "float":
        f = float(v); return repr(f) + "f" if "." in repr(f) else f"{f:.1f}f"
    if base == "bool":
        return "true" if v not in ("0", "", "false", "False") else "false"
    return str(int(float(v)))

def c_string(v):
    return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'

def elem_init(base, row):
    """Initialiser for one dictionary tag of C type base (scalar, string or timer), or None for zero."""
    pu, epu = row["power_up"], row["element_power_up"]
    if base == "ul_timer_t":
        m = re.search(r"\.Preset=(\d+)", epu)
        if not m: return None
        p = m.group(1)
        return "{ %su, %su, false, false }" % (p, p)
    if base == "char":
        return c_string(pu) if pu not in ("",) else None
    if pu in ("", "0"): return None
    return c_number(base, pu)

def array_inits(row, n):
    vals = {}
    for m in re.finditer(r"\[\w+?_(\d+)\]=([^;]+)", row["element_power_up"]):
        vals[int(m.group(1))] = m.group(2).strip()
    if row["power_up"] not in ("", "0"):
        die(f"{row['name']}: array with a scalar power-up value {row['power_up']!r}")
    return vals

def main():
    ddp = sys.argv[1]
    dd = {r["name"]: r for r in csv.DictReader(open(ddp, newline="", encoding="utf-8"))}
    lines = subprocess.run([sys.executable, os.path.join(HERE, "list_externs.py")],
                           capture_output=True, text=True, check=True).stdout.splitlines()
    out, rows = [], []
    for ln in lines:
        name, ctype, hdr = ln.split("\t")
        base, dims = parse_type(ctype)
        decl_dims = "".join(f"[{d}]" for d in dims)
        if name in NOT_TAGS:
            kind = NOT_TAGS[name]
            rows.append((name, ctype, hdr, kind, ""))
            if kind in ("io", "hmi", "diag") or kind.startswith("assume"):
                init = " = false" if kind == "assume:false" else ""
                out.append((hdr, f"{base} {name}{decl_dims}{init};", f"{kind}"))
            continue
        if name in dd:
            row = dd[name]
            dbase, ddims = dd_ctype(row)
            if (dbase, ddims) != (base, dims):
                die(f"{name}: header type {ctype} but dictionary {row['c_type']} dims {row['dims']!r}")
            if dims and base != "char" or len(dims) > 1:
                vals = array_inits(row, dims[0])
                if vals:
                    items = ", ".join(f"[{i}] = {c_number(base, v)}" for i, v in sorted(vals.items()))
                    init = " = { " + items + " }"
                else:
                    init = ""
            else:
                e = elem_init(base, row)
                init = f" = {e}" if e else ""
            store = "FRAM" if row["retain"] == "1" else "RAM"
            out.append((hdr, f"{base} {name}{decl_dims}{init};", f"{store}"))
            rows.append((name, ctype, hdr, store, name))
            continue
        if name in GROUPS:
            members = GROUPS[name]
            # element type: header type minus the first dimension (for 1-D char arrays of strings, minus 1st dim)
            n0 = dims[0] if dims else 1
            edims = dims[1:] if dims else []
            if len(members) != n0:
                die(f"{name}: {n0} elements in the header, {len(members)} in GROUPS")
            inits, stores = [], set()
            for i, m in enumerate(members):
                if m not in dd: die(f"{name}[{i}]: dictionary has no tag {m!r}")
                row = dd[m]
                mb, md = dd_ctype(row)
                if (mb, md) != (base, edims):
                    die(f"{name}[{i}] = {m}: dictionary {row['c_type']}, header element {base}{edims}")
                e = elem_init(base, row)
                if e: inits.append(f"[{i}] = {e}")
                stores.add("FRAM" if row["retain"] == "1" else "RAM")
            if len(stores) != 1: die(f"{name}: elements mix retained and not retained")
            store = stores.pop()
            init = (" = { " + ", ".join(inits) + " }") if (inits and dims) else (f" = {inits[0].split(' = ',1)[1]}" if inits else "")
            out.append((hdr, f"{base} {name}{decl_dims}{init};", f"{store}: {members[0]}" + (f" .. {members[-1]}" if len(members) > 1 else "")))
            for i, m in enumerate(members):
                rows.append((f"{name}[{i}]" if dims else name, ctype, hdr, store, m))
            continue
        die(f"{name} ({ctype}, {hdr}): not in the data dictionary, GROUPS or NOT_TAGS")

    src = os.path.join(APP, "src", "vm_tags.c")
    with open(src, "w") as f:
        f.write("/* vm_tags.c - GENERATED by tools/gen_tags.py from the chunk headers and\n"
                " * Analysis/stage2/data_dictionary.csv. Do not edit; change the generator.\n *\n"
                " * The one definition of every application tag, with the original power-up values.\n"
                " * FRAM = retained in the original (the parameter store loads the saved value over the\n"
                " * power-up value); RAM = power-up value after every start. */\n"
                '#include "vm_tags.h"\n\n')
        cur = None
        for hdr, decl, note in sorted(out, key=lambda x: (x[0], x[1].split()[1])):
            if hdr != cur:
                f.write(f"\n/* ---- {hdr} ---- */\n"); cur = hdr
            f.write(f"{decl:<72} /* {note} */\n" if len(decl) < 72 else f"{decl} /* {note} */\n")
    os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
    with open(os.path.join(HERE, "out", "tag_map.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(["c_name", "c_type", "header", "storage_or_kind", "dictionary_name"])
        w.writerows(rows)
    print(f"gen_tags: {len(out)} definitions, {sum(1 for r in rows if r[4])} dictionary tags mapped")

if __name__ == "__main__":
    main()
