#!/usr/bin/env python3
"""Write docs/MODBUS_MAP.md: the register map for integrators (PLC / SCADA / DAHS), from
faithful/modbus_map.csv (the old UniLogic slave map, kept at its addresses) and the platform
registers added in vm_modbus.c. Meanings come from 47_Modbus, 43, 45, 46 and 49."""
import csv, os, re
HERE = os.path.dirname(os.path.abspath(__file__)); APP = os.path.join(HERE, "..")

MEANING = {
    "CurrentMode": "VM mode: 0 none, 1 Modbus, 2 schedule, 3 manual",
    "NumInterconnections": "VM interconnections (1 or 2)",
    "FirstInlets": "VM inlets, interconnection 1", "FirstOutlets": "VM outlets, interconnection 1",
    "SecondInlets": "VM inlets, interconnection 2", "SecondOutlets": "VM outlets, interconnection 2",
    "SelectedFirstInletMb": "VM Modbus route: inlet, interconnection 1 (1-based)",
    "SelectedFirstOutletMb": "VM Modbus route: outlet, interconnection 1 (1-based)",
    "SelectedSecondInletMb": "VM Modbus route: inlet, interconnection 2 (1-based)",
    "SelectedSecondOutletMb": "VM Modbus route: outlet, interconnection 2 (1-based)",
    "ValveSelectionErrorModbus": "VM: the route written is not fitted (cleared by coil 2)",
    "StartModbusCoil": "VM: apply the route (Modbus mode must be on, started on the touchscreen)",
    "ResetMbError": "VM: clear the route error and 'not active' flags",
    "StartModbus": "VM Modbus mode is on",
    "BackFlushValveAvailable": "VM back-flush valve fitted",
    "FlushValveBitModbus": "VM: open the back-flush valve (Modbus mode)",
    "ModbusModeNotActive": "VM: coil 1 written while Modbus mode is off",
    "StartModbusCoilMUX": "MUX: apply the valve in holding 762 (MUX Modbus mode must be on)",
    "StartModbusMUX": "MUX Modbus mode is on",
    "ValveSelectionErrorModbusMUX": "MUX: the valve written is not fitted",
    "ResetMbErrorMUX": "MUX: clear the error and 'not active' flags",
    "PurgeInletActivateModbus": "MUX: open the purge valve",
    "ModbusNotActiveMUX": "MUX: coil 7 written while MUX Modbus mode is off",
    "AlarmsVM": "VM discrete-input alarm per input 1..22",
    "AlarmsMUX": "MUX discrete-input alarm per input 1..22",
    "FBAlarmsMUX": "MUX valve-feedback alarm per valve DO1..DO20",
    "AnalogTimer0": "AI1 out of range (the platform alarm AI1_RANGE)", "AnalogTimer1": "AI2 out of range",
    "AnalogTimer2": "AI3 out of range (no hardware: always 0)", "AnalogTimer3": "AI4 out of range (no hardware: always 0)",
    "TempTimer0": "RTD1 out of range", "TempTimer1": "RTD2 out of range",
    "TempTimer2": "RTD3 (no hardware: always 0)", "TempTimer3": "RTD4 (no hardware: always 0)",
    "EmailCantbeSent": "an alarm email could not be sent (platform NOTIFY_FAIL)",
    "ContactManufacturer1": "loading the valve names failed (old CSV lists; normally 0)",
    "StopSeqBit": "MUX digital stop input active", "AnalyzerOkBitAlarm": "MUX: analyser not OK",
    "FlowValidationAlarm": "MUX: no flow detected with a valve open (latched until acknowledged)",
    "AcknEmailSentError": "write 1: clear the email-failed flag",
    "Acknowledge Bit": "write 1: acknowledge every alarm",
    "Ackbit1": "write 1: clear coil 86", "Ackbit2": "write 1: (no effect, as the original)",
    "ContactManufacturer2": "old log: data not deleted (always 0)", "SaveDataFailedReserv": "old log save failed (always 0)",
    "AcknowledgeCSVReserv": "write 1: (old log; accepted, no effect)", "AlarmLoadingTable": "old log load error (always 0)",
    "AckowledgeLoadError": "write 1: (old log; accepted, no effect)",
    "ModbusTimer": "VM Modbus run time, interconnection 1 (ms)", "ModbusTimer2": "VM Modbus run time, interconnection 2 (ms, retained)",
    "ProcessTimer": "VM time left, interconnection 1 (ms)", "ProcessTimer2": "VM time left, interconnection 2 (ms)",
    "CurrentModeMUX": "MUX mode: 0 none, 1 Modbus, 2 scheduler (loop), 3 manual",
    "NumberOfMUXValves": "MUX valves configured (inlets + purge)",
    "SelectedValveMbMUX": "MUX Modbus valve (1-based)",
    "ModbusTimerMUX": "MUX Modbus run time (ms)", "ProcessTimerMUX": "MUX time left in the current step (ms)",
    "AnalogSensor": "AI1..AI4 scaled values, IEEE 754 float each (AI3/AI4 no hardware)",
    "TemperatureSensor": "RTD1..RTD4 in 0.1 degree, signed (RTD3/RTD4 no hardware)",
}

def group_key(tag):
    m = re.match(r"^(.*?)(\d+)(\D*)$", tag)
    return (m.group(1), m.group(3)) if m else None

def main():
    rows = list(csv.DictReader(open(os.path.join(APP, "faithful", "modbus_map.csv"))))
    out = []
    i = 0
    while i < len(rows):
        r = rows[i]
        tag, mem = r["tag"], r["member"]
        if r["type"] == "BUFFER":
            g = group_key(tag); j = i
            while j + 1 < len(rows) and rows[j + 1]["type"] == "BUFFER" and group_key(rows[j + 1]["tag"]) == g \
                    and rows[j + 1]["length"] == rows[i]["length"] \
                    and int(rows[j + 1]["address"]) == int(rows[j]["address"]) + int(rows[j]["length"]):
                j += 1
            first, last = rows[i], rows[j]
            span = f"{first['address']}-{int(last['address']) + int(last['length']) - 1}"
            what = f"{first['tag']} .. {last['tag']}" if j > i else first["tag"]
            n = int(first["length"])
            if "ValveNameMUX" in tag:
                meaning = "MUX inlet names (inlet = number in the tag)"
            elif "Intercon" in tag:
                meaning = "VM valve names of that interconnection, in order; a blank name reads as its number"
            elif tag.startswith("FlushValveMUX"):
                meaning = "purge valve name (not set: empty)"
            elif tag.startswith(("Analog", "Temperature")):
                meaning = "analog / temperature alarm descriptions (no text source yet: empty)"
            elif "Description" in tag:
                meaning = "discrete-input alarm descriptions, input 1..22"
            else:
                meaning = "text"
            out.append(("holding", span, "R", f"text, {n} registers each ({2*n} bytes UTF-8)", what, meaning))
            i = j + 1; continue
        space = "coil" if r["space"] == "coil" else "holding"
        rw = "W" if r["access"] == "master writes" else "R"
        a = int(r["address"]); n = int(r["length"])
        span = str(a) if n == 1 else f"{a}-{a + n - 1}"
        typ = {"BIT": "bit" if not r["dims"] else f"bits x{r['dims']}", "INT16": "int16" if not r["dims"] else f"int16 x{r['dims']}",
               "REAL": f"float32 x{r['dims']}", "TIMER": "uint32 (ms)" if mem == "Preset" else "bit"}[r["type"]]
        out.append((space, span, rw, typ, tag + (f".{mem}" if mem else ""), MEANING.get(tag, "")))
        i += 1

    with open(os.path.join(APP, "docs", "MODBUS_MAP.md"), "w") as f:
        f.write("""# CEMS Supervisor Valve Manager / Multiplexor: Modbus TCP map

GENERATED by `tools/gen_modbus_doc.py`. Port 502, unit ID any. For PLC, SCADA and DAHS integrators.

**Compatibility.** Every address of the previous Unitronics unit's slave map is kept, so an existing
master configuration keeps working. New: the alarm registers (input registers, section 2) and the
alarm acknowledge register (holding 3000).

**Formats** (Modbus convention): 16-bit values big-endian; 32-bit integers and floats as two registers,
high word first ("ABCD"); text two bytes per register, first byte in the high half, UTF-8, NUL padded.

**Function codes.** Coils: read 01, write 05 / 15. Holding registers: read 03, write 06 / 16. Input
registers: read 04. Device identification: 43/14 (vendor, product, firmware revision).

**Writes** are accepted only in REMOTE mode (selected on the touchscreen) and when Modbus writes are
enabled (platform setting), except the acknowledge coils and register, which work in any mode. A write
to a read-only address returns exception 02; a value out of range 03; the wrong mode 04.
Sequences and schedules are not on Modbus.

## 1. The previous unit's map

| Table | Address | R/W | Type | Tag | Meaning |
|---|---|---|---|---|---|
""")
        for space, span, rw, typ, tag, meaning in out:
            f.write(f"| {space} | {span} | {rw} | {typ} | `{tag}` | {meaning} |\n")
        f.write("""
## 2. Alarm registers (new)

| Table | Address | R/W | Type | Meaning |
|---|---|---|---|---|
| input | 0 | R | uint16 | alarms active |
| input | 1 | R | uint16 | alarms waiting for an acknowledge |
| input | 2 | R | uint16 | common alarm word (alarms routed to Modbus, alarm relay states) |
| input | 10-25 | R | 16 x uint16 | alarm active bits: alarm ID n is bit (n % 16) of register 10 + n / 16 |
| input | 30-45 | R | 16 x uint16 | alarm unacknowledged bits, same layout |
| holding | 3000 | W | uint16 | write an alarm ID to acknowledge it, 65535 = all |

Alarm IDs: 100-121 digital inputs 1-22, 130-149 valve feedback DO1-DO20, 160/162 AI1/AI2 range,
161/163 AI1/AI2 sensor fault, 164/166 RTD1/RTD2 range, 165/167 RTD1/RTD2 sensor fault,
170 flow validation, 171 digital stop, 172 analyser not OK, 173 sampling paused. 1-99: platform
(calibration invalid, storage, IP conflict, remote link lost, unexpected reset, ...).
""")
    print(f"gen_modbus_doc: {len(out)} rows")

if __name__ == "__main__":
    main()
