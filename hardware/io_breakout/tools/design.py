"""I/O breakout board circuit, as data. gen_schematic.py turns this into the KiCad schematic
and check_netlist.py checks KiCad's exported netlist against it.

Each part: ref, symbol (KiCad library "Lib:Name"), value, footprint, sheet, pins
{pin number: net}. A pin not listed gets a no-connect flag. Nets are plain names; a net
used on more than one sheet becomes a global label.

Rev 0.2 (31 Oct, Gary): ONE board for all the digital I/O on ONE 40-way ribbon, so no
ribbon can go into the wrong slot: 8 valve outputs (bank A, ISO2H823V2.5) and 8 inputs
(ISO1I813T), on two-tier pluggable terminals. Values marked CHECK in the "note" field must
be confirmed against the datasheet before layout.
"""

SHEETS = [
    ("connectors", "Connectors", "40-way latched IDC header, two-tier terminals, power LED, mounting"),
    ("channels", "Channels", "Per output: LED, flyback diode, optional fast-release Zener. Per input: LED"),
]

PARTS = []


def part(ref, symbol, value, footprint, sheet, pins, mpn="", note="", dnp=False):
    PARTS.append(dict(ref=ref, symbol=symbol, value=value, footprint=footprint,
                      sheet=sheet, pins={str(k): v for k, v in pins.items()},
                      mpn=mpn, note=note, dnp=dnp))


R0805 = "Resistor_SMD:R_0805_2012Metric"
R1206 = "Resistor_SMD:R_1206_3216Metric"
LED0805 = "LED_SMD:LED_0805_2012Metric"
TIER2x8 = "cems_io_breakout:Phoenix_MCDV_1,5_8-G-3.81_2x08_P3.81mm_TwoLevel"
OUT = [f"OUT{n}" for n in range(1, 9)]
IN = [f"IN{n}" for n in range(1, 9)]


def r(ref, value, sheet, a, b, fp=R0805, **kw):
    part(ref, "Device:R", value, fp, sheet, {1: a, 2: b}, **kw)


# ---------------------------------------------------------------- connectors
S = "connectors"
# Ribbon pinout, the same on the main board's J10: standard straight-through 40-way cable,
# pin 1 to pin 1. 1-16: each valve output beside its own 0 V return; 17-32: each input
# beside a 0 V; 33-40: 4 x +24 V (sensor supply and LEDs, fused 0.75 A on the main board)
# and 4 x 0 V. 20 x 0 V carry bank A's 4.8 A worst case, about 0.25 A each.
idc = {}
for i in range(8):
    idc[2 * i + 1], idc[2 * i + 2] = OUT[i], "0V"
    idc[2 * i + 17], idc[2 * i + 18] = IN[i], "0V"
for pin in range(33, 41):
    idc[pin] = "+24V" if pin % 2 else "0V"
part("J1", "Connector_Generic:Conn_02x20_Odd_Even", "IDC40", "Connector_IDC:IDC-Header_2x20_P2.54mm_Latch_Vertical",
     S, idc, mpn="Wurth 61204022821 (WR-BHD 40-way, long latches, 3 A per contact). CHECK MPN",
     note="pin 1 marked on the silkscreen; keyed by the shroud notch, standard straight-through ribbon")
# Two-tier pluggable headers: each channel is one column, upper tier over lower tier.
# Valves: OUTn over 0V. Inputs: INn over +24V (a 2-wire contact lands in one column).
# 3-wire sensors take their 0 V from J4.
part("J2", "Connector_Generic:Conn_02x08_Odd_Even", "VALVES 1-8", TIER2x8, S,
     {**{2 * i + 1: OUT[i] for i in range(8)}, **{2 * i + 2: "0V" for i in range(8)}},
     mpn="Phoenix MCDV 1,5/8-G-3,81 two-level header + 2 x MCVR plugs (or Degson/Dinkle equivalent). "
         "Coding keys: upper tier pin 1, lower tier pin 2 (plugs coded to match). CHECK MPN, footprint, coding parts",
     note="upper tier OUT1-8, lower tier 0V")
part("J3", "Connector_Generic:Conn_02x08_Odd_Even", "INPUTS 1-8", TIER2x8, S,
     {**{2 * i + 1: IN[i] for i in range(8)}, **{2 * i + 2: "+24V" for i in range(8)}},
     mpn="as J2; coding keys: upper tier pin 3, lower tier pin 4", note="upper tier IN1-8, lower tier +24V")
part("J4", "Connector_Generic:Conn_01x08", "SENSOR 0V",
     "Connector_Phoenix_MC:PhoenixContact_MCV_1,5_8-G-3.81_1x08_P3.81mm_Vertical", S,
     {n: "0V" for n in range(1, 9)}, mpn="Phoenix MCV 1,5/8-G-3,81 + plug; coding key at pin 5", note="0 V for 3-wire sensors, one per input")
r("R40", "10k", S, "+24V", "PWR_LED_A", note="power LED, 2.2 mA at 24 V")
part("D40", "Device:LED", "green PWR", LED0805, S, {2: "PWR_LED_A", 1: "0V"})
for n in range(1, 5):
    part(f"H{n}", "Mechanical:MountingHole", "M3", "MountingHole:MountingHole_3.2mm_M3", S, {},
         note="board 50 x 80 mm for a standard 50 x 80 mm DIN PCB holder (Adafruit 4435 for prototypes); CHECK hole positions" if n == 1 else "")

# ---------------------------------------------------------------- channels
S = "channels"
for n in range(1, 9):
    ch = OUT[n - 1]
    # Status LED; the series 1N4148W keeps the LED's 5 V reverse rating safe from the coil's
    # negative turn-off spike.
    r(f"R{n}", "10k", S, ch, f"LED{n}_R", note="2.2 mA at 24 V, 48 mW" if n == 1 else "")
    part(f"DS{n}", "Device:D", "1N4148W", "Diode_SMD:D_SOD-123", S, {2: f"LED{n}_R", 1: f"LED{n}_A"})
    part(f"LED{n}", "Device:LED", "green", LED0805, S, {2: f"LED{n}_A", 1: "0V"})
    # Flyback clamp at the valve end: 0V -> Zn (or RZn) -> CLn -> Dn -> OUTn. With RZn fitted it
    # is a plain diode (slow release); with Zn fitted instead, a fast-release clamp.
    part(f"D{n}", "Device:D", "S1M", "Diode_SMD:D_SMA", S, {1: ch, 2: f"CL{n}"},
         mpn="S1M (1 kV, 1 A)", note="flyback" if n == 1 else "")
    r(f"RZ{n}", "0R", S, f"CL{n}", "0V", fp=R1206,
      note="fitted = plain diode clamp; remove and fit Z for fast release" if n == 1 else "")
    part(f"Z{n}", "Device:D_Zener", "24V 1.5W", "Diode_SMD:D_SMB", S, {1: "0V", 2: f"CL{n}"},
         mpn="1SMB5934BT3 (24 V, 1.5 W)", dnp=True,
         note="fast-release option. CHECK Vz + 0.7 V stays below the ISO2H823V2.5's own demagnetisation clamp" if n == 1 else "")
for n in range(1, 9):
    # Input LED: lit while the contact is closed (input at 24 V)
    r(f"R{10 + n}", "10k", S, IN[n - 1], f"LED{10 + n}_A", note="input LED, 2.2 mA (the input itself draws about 2 mA)" if n == 1 else "")
    part(f"LED{10 + n}", "Device:LED", "yellow", LED0805, S, {2: f"LED{10 + n}_A", 1: "0V"})
