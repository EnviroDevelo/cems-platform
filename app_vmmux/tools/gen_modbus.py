#!/usr/bin/env python3
"""Generate the Modbus map from the old UniLogic slave map (APP_INTEGRATION.md section 7).

Every old entry keeps its address, so an existing master / DAHS configuration keeps working:
  old "register" (FC3 read / FC16 write)  -> MB_HOLDING
  old "coil"     (FC1 read / FC15 write)  -> MB_COILS
Each entry needs a parameter-table ID. A tag that is already a parameter (gen_params.py)
uses that one; any other tag gets a new entry here (IDs 4000+, tools/param_ids.csv):
  "master reads"  -> PF_RO
  "master writes" -> writable at operator level (the platform's Modbus write level), through
                     the usual checks (REMOTE mode, Modbus writes enabled). The acknowledge
                     coils are PF_ANY_MODE, as alarm acknowledge is (D19). The original took
                     any value of the type, so the range is the type's (reported).
Byte and word order: UniLogic's packing of BUFFER bytes and 32-bit words is not known
(open Rule 0 item, APP_INTEGRATION.md section 7). The map uses the platform defaults
(high byte / high word first) and VM_MB_BYTE_FLAGS / VM_MB_WORD_FLAGS, one switch each,
so the answer is a one-line change.
Platform alarm registers (not in the old map, which never used input registers):
  input 0 active, 1 unacknowledged, 2 common alarm word, 10-25 active bits, 30-45 unack bits;
  holding 3000 alarm acknowledge (write an alarm ID, 65535 = all).

Outputs: src/vm_modbus.c, src/vm_params_modbus.inc, tools/out/modbus_report.csv
"""
import csv, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "..")
ACK_COILS = {"Acknowledge Bit", "AcknEmailSentError", "Ackbit1", "Ackbit2", "AcknowledgeCSVReserv", "AckowledgeLoadError"}

def key(n):
    k = re.sub(r"[^A-Za-z0-9_]", "_", n); return re.sub(r"_+", "_", k).strip("_")

def main():
    ddp, mapp = sys.argv[1], sys.argv[2]
    dd = {r["name"]: r for r in csv.DictReader(open(ddp, newline="", encoding="utf-8"))}
    tmap = {r["dictionary_name"]: r["c_name"] for r in csv.DictReader(open(os.path.join(HERE, "out", "tag_map.csv"))) if r["dictionary_name"]}
    rep = {r["dictionary_name"]: r for r in csv.DictReader(open(os.path.join(HERE, "out", "params_report.csv")))}
    regp = os.path.join(HERE, "param_ids.csv")
    reg = {r["dictionary_name"]: int(r["id"]) for r in csv.DictReader(open(regp))}
    used = set(reg.values())
    def new_id(name):
        if name in reg: return reg[name]
        i = 4000
        while i in used: i += 1
        reg[name] = i; used.add(i); return i
    def cvar(n):
        return tmap.get(n, "vmx_" + key(n))

    entries, params, report = [], [], []
    for r in csv.DictReader(open(mapp, newline="", encoding="utf-8")):
        tag, mem, typ, addr, acc = r["tag"], r["member"], r["type"], int(r["address"]), r["access"]
        if tag not in dd: sys.exit(f"gen_modbus: {tag} not in the data dictionary")
        writes = acc == "master writes"
        table = "MB_COILS" if r["space"] == "coil" else "MB_HOLDING"
        var = cvar(tag) + (f".{mem}" if mem else "")
        pname = tag + (f".{mem}" if mem else "")
        existing = rep.get(tag)
        if existing and not mem and existing["range_source"] not in ("RETAIN", "-") and existing["id"]:
            pid_expr = existing["id"]; how = "existing parameter"
            if writes: how += " (written by the master: checked at the parameter's own level)"
        else:
            pid = new_id("(modbus) " + pname); pid_expr = str(pid); how = "new"
            lvl = "PLAT_LVL_OPERATOR" if writes else "PLAT_LVL_VIEW"
            fl = "0" if writes else "PF_RO"
            if tag in ACK_COILS: fl = "PF_ANY_MODE"
            k = "mb_" + key(pname)
            if typ == "BIT" and not r["dims"]:
                params.append(f'PARAM_BIT({pid}, "{k}", {var}, {lvl}, {fl})')
            elif typ == "BIT":
                params.append(f'PARAM_ARRAY_BOOL({pid}, "{k}", {var}, {lvl}, {fl})')
            elif typ == "TIMER" and mem == "Out":
                params.append(f'PARAM_BIT({pid}, "{k}", {var}, {lvl}, {fl})')
            elif typ == "TIMER" and mem == "Preset":
                d = re.search(r"\.Preset=(\d+)", dd[tag]["element_power_up"])
                params.append(f'PARAM_UINT({pid}, "{k}", {var}, CT_U32, 0u, 4294967295u, {d.group(1) if d else 0}u, {lvl}, {fl}, "ms")')
            elif typ == "INT16" and not r["dims"]:
                params.append(f'PARAM_INT({pid}, "{k}", {var}, CT_I16, -32768, 32767, 0, {lvl}, {fl}, NULL)')
            elif typ == "INT16":
                params.append(f'PARAM_ARRAY_INT({pid}, "{k}", {var}, CT_I16, -32768, 32767, {lvl}, {fl})')
            elif typ == "REAL":
                params.append(f'PARAM_ARRAY_REAL({pid}, "{k}", {var}, -3.4e38f, 3.4e38f, {lvl}, {fl})')
            elif typ == "BUFFER":
                params.append(f'PARAM_BYTES({pid}, "{k}", {var}, {lvl}, {fl})')
            else:
                sys.exit(f"gen_modbus: {pname}: type {typ} not handled")
        if table == "MB_COILS":
            fmt, flags = ("MBF_ARRAY" if r["dims"] else "MBF_BIT"), "0"
        elif typ == "INT16" and not r["dims"]:
            fmt, flags = "MBF_I16", "0"
        elif typ == "TIMER":
            fmt, flags = "MBF_U32", "VM_MB_WORD_FLAGS"
        elif typ == "BUFFER":
            fmt, flags = "MBF_BYTES", "VM_MB_BYTE_FLAGS"
        elif typ == "REAL":
            fmt, flags = "MBF_ARRAY", "VM_MB_WORD_FLAGS"
        else:
            fmt, flags = "MBF_ARRAY", "0"
        entries.append(f"    MB_ENTRY_X({table}, {addr}, {pid_expr}, {fmt}, {flags}, 0),   /* {r['name']}: {pname} ({acc}) */")
        report.append([r["space"], addr, r["length"], acc, pname, pid_expr, how, fmt, flags])

    with open(regp, "w", newline="") as f:
        w = csv.writer(f); w.writerow(["id", "dictionary_name"])
        for n, i in sorted(reg.items(), key=lambda x: x[1]): w.writerow([i, n])
    with open(os.path.join(APP, "include", "vm_params_modbus.h"), "w") as f:
        f.write("/* vm_params_modbus.h - GENERATED by tools/gen_modbus.py: IDs of the Modbus-only parameters. */\n"
                "#ifndef VM_PARAMS_MODBUS_H\n#define VM_PARAMS_MODBUS_H\nenum {\n")
        for n, i in sorted(reg.items(), key=lambda x: x[1]):
            if n.startswith("(modbus) "):
                f.write(f"    VMMB_{key(n[9:]).upper():<40} = {i},\n")
        f.write("};\n#endif\n")
    with open(os.path.join(APP, "src", "vm_params_modbus.inc"), "w") as f:
        f.write("/* vm_params_modbus.inc - GENERATED by tools/gen_modbus.py: parameter entries for the tags the\n"
                " * old Modbus map exposes that are not settings. Included at the end of the table in vm_params.c. */\n")
        for p in params: f.write(f"{p},\n")
    with open(os.path.join(APP, "src", "vm_modbus.c"), "w") as f:
        f.write('''/* vm_modbus.c - GENERATED by tools/gen_modbus.py from stage4/4G/modbus_map.csv. Do not edit.
 * The old UniLogic slave map at its old addresses, plus the platform alarm registers.
 * Byte / word order: VM_MB_BYTE_FLAGS, VM_MB_WORD_FLAGS (open Rule 0 item: how UniLogic packs
 * BUFFER bytes and 32-bit words). 0 = the platform default, high byte / high word first. */
#include "vm_app.h"

#ifndef VM_MB_BYTE_FLAGS
#define VM_MB_BYTE_FLAGS 0
#endif
#ifndef VM_MB_WORD_FLAGS
#define VM_MB_WORD_FLAGS 0
#endif

static const mb_map_t map[] = {
''')
        for e in entries: f.write(e + "\n")
        f.write('''    /* platform alarm registers (not in the old map) */
    MB_ENTRY(MB_INPUT,    0, PARAM_ID_ALARM_ACTIVE,      MBF_U16),
    MB_ENTRY(MB_INPUT,    1, PARAM_ID_ALARM_UNACKED,     MBF_U16),
    MB_ENTRY(MB_INPUT,    2, PARAM_ID_ALARM_COMMON,      MBF_U16),
    MB_ENTRY(MB_INPUT,   10, PARAM_ID_ALARM_ACTIVE_BITS, MBF_ARRAY),
    MB_ENTRY(MB_INPUT,   30, PARAM_ID_ALARM_UNACK_BITS,  MBF_ARRAY),
    MB_ENTRY(MB_HOLDING, 3000, PARAM_ID_ALARM_ACK,       MBF_U16),
};
const mb_map_t *const vm_modbus = map;
const size_t vm_n_modbus = sizeof map / sizeof map[0];
''')
    with open(os.path.join(HERE, "out", "modbus_report.csv"), "w", newline="") as f:
        w = csv.writer(f); w.writerow(["space", "address", "length", "access", "tag", "param_id", "how", "format", "flags"])
        w.writerows(report)
    print(f"gen_modbus: {len(entries)} old entries, {len(params)} new parameters")

if __name__ == "__main__":
    main()
