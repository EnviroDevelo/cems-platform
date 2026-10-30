"""I/O breakout board circuit, as data. gen_schematic.py turns this into the KiCad schematic
and check_netlist.py checks KiCad's exported netlist against it.

Each part: ref, symbol (KiCad library "Lib:Name"), value, footprint, sheet, pins
{pin number: net}. A pin not listed gets a no-connect flag. Nets are plain names; a net
used on more than one sheet becomes a global label.

One design, two builds (README.md): OUT for valve bank A (ISO2H823V2.5 high-side outputs)
and IN for the digital inputs (ISO1I813T). The dnp flags below are the OUT build; the IN
build also leaves out D1-D8 and RZ1-RZ8. Values marked CHECK in the "note" field must be
confirmed against the datasheet before layout.
"""

SHEETS = [
    ("connectors", "Connectors", "26-way latched IDC header, 2 x 12 screw terminals, power LED, mounting"),
    ("channels", "Channels", "Per channel: status LED, flyback diode with optional fast-release Zener, wire-break resistor"),
]

PARTS = []


def part(ref, symbol, value, footprint, sheet, pins, mpn="", note="", dnp=False):
    PARTS.append(dict(ref=ref, symbol=symbol, value=value, footprint=footprint,
                      sheet=sheet, pins={str(k): v for k, v in pins.items()},
                      mpn=mpn, note=note, dnp=dnp))


R0805 = "Resistor_SMD:R_0805_2012Metric"
R1206 = "Resistor_SMD:R_1206_3216Metric"
LED0805 = "LED_SMD:LED_0805_2012Metric"
TERM12 = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-12-5.08_1x12_P5.08mm_Horizontal"
CH = [f"CH{n}" for n in range(1, 9)]


def r(ref, value, sheet, a, b, fp=R0805, **kw):
    part(ref, "Device:R", value, fp, sheet, {1: a, 2: b}, **kw)


# ---------------------------------------------------------------- connectors
S = "connectors"
# Ribbon pinout, the same on the main board's header: standard straight-through cable, pin 1
# to pin 1. Each channel conductor sits beside a 0 V conductor (pins 1-16), then five
# +24 V / 0 V pairs (pins 17-26): 13 x 0 V carry bank A's 8 x 0.6 A return, 5 x +24 V the
# sensor supply. A 28 AWG conductor and an IDC contact are good for about 1 A each.
idc = {}
for i, ch in enumerate(CH):
    idc[2 * i + 1] = ch
    idc[2 * i + 2] = "0V"
for pin in range(17, 27):
    idc[pin] = "+24V" if pin % 2 else "0V"
part("J1", "Connector_Generic:Conn_02x13_Odd_Even", "IDC26", "Connector_IDC:IDC-Header_2x13_P2.54mm_Latch_Vertical",
     S, idc, mpn="26-way 2.54 mm shrouded box header with ejector latches (Wurth, Amphenol or TE), 1 A per contact. CHECK MPN",
     note="pin 1 marked on the silkscreen; keyed by the shroud notch only, standard straight-through ribbon")
# Terminals: three per channel, CHn, 0V, +24V. Valves use CHn and 0V; inputs use +24V and CHn
# (plus 0V for a 3-wire sensor).
for ref, chans in (("J2", CH[:4]), ("J3", CH[4:])):
    pins = {}
    for i, ch in enumerate(chans):
        pins[3 * i + 1], pins[3 * i + 2], pins[3 * i + 3] = ch, "0V", "+24V"
    part(ref, "Connector:Screw_Terminal_01x12", "12-way 5.08", TERM12, S, pins,
         mpn="Degson or Dinkle 5.08 mm PCB screw terminal, 12-way, UL, 300 V, 8 A or more. CHECK MPN and footprint",
         note=f"{chans[0]}-{chans[-1]}: CHn, 0V, +24V per channel")
r("R20", "10k", S, "+24V", "PWR_LED_A", note="power LED, 2.2 mA at 24 V")
part("D20", "Device:LED", "green PWR", LED0805, S, {2: "PWR_LED_A", 1: "0V"})
for n in range(1, 5):
    part(f"H{n}", "Mechanical:MountingHole", "M3", "MountingHole:MountingHole_3.2mm_M3", S, {},
         note="DIN-rail PCB carrier or clips. CHECK carrier part" if n == 1 else "")

# ---------------------------------------------------------------- channels
S = "channels"
for n, ch in enumerate(CH, start=1):
    # Status LED, on when the output is on or the input contact is closed. The series
    # 1N4148W keeps the LED's 5 V reverse rating safe from the coil's negative turn-off
    # spike when no flyback diode is fitted.
    r(f"R{n}", "10k", S, ch, f"LED{n}_R", note="2.2 mA at 24 V, 48 mW" if n == 1 else "")
    part(f"DS{n}", "Device:D", "1N4148W", "Diode_SMD:D_SOD-123", S, {2: f"LED{n}_R", 1: f"LED{n}_A"})
    part(f"LED{n}", "Device:LED", "green", LED0805, S, {2: f"LED{n}_A", 1: "0V"})
    # Flyback clamp at the valve end (OUT build): 0V -> Zn (or RZn) -> CLn -> Dn -> CHn. With
    # RZn fitted it is a plain diode (slow release); with Zn fitted instead it clamps at
    # Vz + 0.7 V below 0 V for a fast release.
    part(f"D{n}", "Device:D", "S1M", "Diode_SMD:D_SMA", S, {1: ch, 2: f"CL{n}"},
         mpn="S1M (1 kV, 1 A)", note="flyback, OUT build only" if n == 1 else "")
    r(f"RZ{n}", "0R", S, f"CL{n}", "0V", fp=R1206,
      note="fitted = plain diode clamp; remove and fit Z for fast release" if n == 1 else "")
    part(f"Z{n}", "Device:D_Zener", "24V 1.5W", "Diode_SMD:D_SMB", S, {1: "0V", 2: f"CL{n}"},
         mpn="1SMB5934BT3 (24 V, 1.5 W)", dnp=True,
         note="fast-release option. CHECK Vz + 0.7 V stays below the ISO2H823V2.5's own demagnetisation clamp" if n == 1 else "")
    r(f"RW{n}", "DNF", S, "+24V", ch, dnp=True,
      note="normally not fitted: field wire-break needs the bias R at the sensor end; RW here only detects a ribbon break" if n == 1 else "")
