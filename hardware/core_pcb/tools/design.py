"""Core PCB circuit, as data. gen_schematic.py turns this into the KiCad schematic and
check_netlist.py checks KiCad's exported netlist against it.

Each part: ref, symbol (KiCad library "Lib:Name", or "cems_core:Name" for the project's
own), value, footprint, sheet, pins {pin number: net}. A pin not listed gets a
no-connect flag. Nets are plain names; a net used on more than one sheet becomes a
global label. Pin numbers come from the KiCad 7 symbol libraries (checked against
docs/CORE_PCB.md section 4 for the MCU).

Values marked CHECK in the "note" field must be confirmed against the part's datasheet
before layout (see docs/CORE_PCB.md).
"""

SHEETS = [
    ("power", "Power", "24 V input, protection, 5 V and 3.3 V bucks, RTC battery, chassis"),
    ("mcu", "MCU", "STM32H573VIT6, clocks, reset, boot, SWD, service UART, LEDs"),
    ("eth_client", "Ethernet client port", "LAN8742A on RMII, client RJ45 (J2)"),
    ("eth_io", "Ethernet I/O port", "W5500 on SPI4, I/O RJ45 (J3), I/O boards only"),
    ("periph", "Memories and display", "NOR flash, FRAM, RVT70 display FFC (J4)"),
]

PARTS = []


def part(ref, symbol, value, footprint, sheet, pins, mpn="", note="", dnp=False):
    PARTS.append(dict(ref=ref, symbol=symbol, value=value, footprint=footprint,
                      sheet=sheet, pins={str(k): v for k, v in pins.items()},
                      mpn=mpn, note=note, dnp=dnp))


# Common footprints
R0603 = "Resistor_SMD:R_0603_1608Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
C1206 = "Capacitor_SMD:C_1206_3216Metric"
C1210 = "Capacitor_SMD:C_1210_3225Metric"
C1812 = "Capacitor_SMD:C_1812_4532Metric"
LED0603 = "LED_SMD:LED_0603_1608Metric"
FB0603 = "Inductor_SMD:L_0603_1608Metric"


def r(ref, value, sheet, a, b, fp=R0603, **kw):
    part(ref, "Device:R", value, fp, sheet, {1: a, 2: b}, **kw)


def c(ref, value, sheet, a, b, fp=C0603, **kw):
    part(ref, "Device:C", value, fp, sheet, {1: a, 2: b}, **kw)


def fb(ref, value, sheet, a, b, **kw):
    part(ref, "Device:FerriteBead_Small", value, FB0603, sheet, {1: a, 2: b}, **kw)


# ---------------------------------------------------------------- power
S = "power"
part("J1", "Connector:Screw_Terminal_01x03", "24VDC IN",
     "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-3-5.08_1x03_P5.08mm_Horizontal", S,
     {1: "+24V_IN", 2: "0V_IN", 3: "CHASSIS"},
     note="pin 1 +24 V, pin 2 0 V, pin 3 functional earth. Pluggable version to suit the enclosure")
part("F1", "Device:Fuse", "2A T", "Fuse:Fuse_1206_3216Metric", S, {1: "+24V_IN", 2: "+24V_F"},
     note="2 A time-lag, rated 63 V DC or more, I2t for the hold-up capacitors' inrush. CHECK part")
part("D1", "Device:D_Schottky", "SS16", "Diode_SMD:D_SMA", S, {2: "+24V_F", 1: "+24V_D"},
     mpn="SS16 (60 V, 1 A)", note="reverse-polarity protection")
part("D2", "Device:D_TVS", "SMDJ33CA", "Diode_SMD:D_SMC", S, {1: "+24V_D", 2: "0V_IN"},
     mpn="SMDJ33CA (3 kW)", note="bidirectional TVS ahead of the choke; IEC 61000-4-5 0.5 kV line-line (docs/CORE_PCB_EMC.md)")
c("C16", "4n7 2kV", S, "+24V_D", "CHASSIS", fp=C1812, note="Y-capacitor, common-mode to chassis (EFT, conducted RF)")
c("C17", "4n7 2kV", S, "0V_IN", "CHASSIS", fp=C1812, note="Y-capacitor")
part("L3", "Device:L_Coupled_1423", "WE-SL2 744227", "Inductor_SMD:L_CommonMode_Wuerth_WE-SL2", S,
     {1: "+24V_D", 4: "+24V", 2: "0V_IN", 3: "GND"}, mpn="Wurth 744227",
     note="common-mode choke on the 24 V input; windings 1-4 and 2-3 (datasheet: 1 A, 80 V, 0.16 R; footprint pads match)")
part("C1", "Device:C_Polarized", "47u 63V", "Capacitor_SMD:CP_Elec_8x10", S, {1: "+24V", 2: "GND"},
     note="bulk, aluminium electrolytic, 105 C")
part("C18", "Device:C_Polarized", "470u 63V", "Capacitor_THT:CP_Radial_D12.5mm_P5.00mm", S, {1: "+24V", 2: "GND"},
     mpn="470 uF 63 V, 105 C, low ESR, 12.5 mm radial",
     note="hold-up with C19: 16 V to 6 V gives 20 ms at full load, 40 ms with the backlight off (CORE_PCB.md 5a)")
part("C19", "Device:C_Polarized", "470u 63V", "Capacitor_THT:CP_Radial_D12.5mm_P5.00mm", S, {1: "+24V", 2: "GND"},
     mpn="470 uF 63 V, 105 C, low ESR, 12.5 mm radial")
c("C2", "2u2 100V", S, "+24V", "GND", fp=C1210)
c("C3", "100n 100V", S, "+24V", "GND")

# 24 V -> 5 V, TPS54360 (4.5-60 V in, 3.5 A), about 600 kHz
part("U1", "Regulator_Switching:TPS54360DDA", "TPS54360B",
     "Package_SO:TI_SO-PowerPAD-8_ThermalVias", S,
     {1: "BUCK5_BOOT", 2: "+24V", 3: "BUCK5_EN", 4: "BUCK5_RT", 5: "BUCK5_FB", 6: "BUCK5_COMP",
      7: "GND", 8: "BUCK5_SW", 9: "GND"},
     mpn="TPS54360BDDAR", note="TPS54360B, same pinout as TPS54360; RT and compensation from its datasheet eq. 44-51 (CORE_PCB.md 5)")
r("R1", "DNF", S, "+24V", "BUCK5_EN", dnp=True, note="UVLO divider, not fitted: EN's internal pull-up enables the regulator")
r("R2", "DNF", S, "BUCK5_EN", "GND", dnp=True)
r("R3", "162k 1%", S, "BUCK5_RT", "GND", note="about 600 kHz")
c("C4", "100n", S, "BUCK5_BOOT", "BUCK5_SW")
part("L1", "Device:L", "22uH", "Inductor_SMD:L_Bourns_SRN6045TA", S, {1: "BUCK5_SW", 2: "+5V"},
     mpn="Bourns SRN6045TA-220M", note="22 uH for 30 % ripple at 1 A, 600 kHz, 24 V in. CHECK Isat >= 1.6 A")
part("D3", "Device:D_Schottky", "B260A", "Diode_SMD:D_SMA", S, {1: "BUCK5_SW", 2: "GND"},
     mpn="B260A-13 (60 V, 2 A)", note="catch diode")
r("R4", "52k3 1%", S, "+5V", "BUCK5_FB")
r("R5", "10k 1%", S, "BUCK5_FB", "GND", note="0.8 V x (1 + 52.3/10) = 4.98 V")
r("R6", "4k99 1%", S, "BUCK5_COMP", "BUCK5_CC", note="type II compensation, crossover about 18 kHz")
c("C5", "33n", S, "BUCK5_CC", "GND")
c("C6", "100p", S, "BUCK5_COMP", "GND")
c("C7", "22u 10V", S, "+5V", "GND", fp=C1210)
c("C8", "22u 10V", S, "+5V", "GND", fp=C1210)

# 5 V -> 3.3 V, TLV62569 (2 A)
part("U2", "Regulator_Switching:TLV62569DBV", "TLV62569DBV", "Package_TO_SOT_SMD:SOT-23-5", S,
     {1: "+5V", 2: "GND", 3: "BUCK3_SW", 4: "+5V", 5: "BUCK3_FB"}, mpn="TLV62569DBVR",
     note="EN tied to VIN")
c("C9", "10u 10V", S, "+5V", "GND", fp=C0805)
part("L2", "Device:L", "2u2 3A", "Inductor_SMD:L_Wuerth_MAPI-4030", S, {1: "BUCK3_SW", 2: "+3V3"},
     mpn="Wurth 74438357022 (MAPI 4030, 2.2 uH)", note="CHECK")
r("R7", "100k 1%", S, "+3V3", "BUCK3_FB")
r("R8", "22k1 1%", S, "BUCK3_FB", "GND", note="0.6 V x (1 + 100/22.1) = 3.31 V")
c("C10", "22p", S, "+3V3", "BUCK3_FB", note="feed-forward, CHECK")
c("C11", "22u 10V", S, "+3V3", "GND", fp=C1206)
c("C12", "22u 10V", S, "+3V3", "GND", fp=C1206)

# 24 V supply sense for the ADC (PC0)
r("R9", "100k 1%", S, "+24V", "V24_SENSE")
r("R10", "8k2 1%", S, "V24_SENSE", "GND", note="32 V -> 2.42 V")
c("C13", "100n", S, "V24_SENSE", "GND")

# RTC battery
part("BT1", "Device:Battery_Cell", "CR2032", "Battery:BatteryHolder_Keystone_3034_1x20mm", S,
     {1: "VBAT", 2: "GND"}, mpn="Keystone 3034 holder + CR2032")
c("C14", "100n", S, "VBAT", "GND")

# Chassis to logic ground
r("R11", "1M", S, "CHASSIS", "GND", fp="Resistor_SMD:R_1206_3216Metric")
c("C15", "4n7 2kV", S, "CHASSIS", "GND", fp=C1812, note="safety-rated, 2 kV")

# ---------------------------------------------------------------- MCU
S = "mcu"
mcu = {
    # power
    6: "VBAT", 10: "GND", 11: "+3V3", 19: "GND", 20: "GND", 21: "+3V3A", 22: "+3V3A",
    27: "GND", 28: "+3V3", 48: "VCAP", 49: "GND", 50: "+3V3", 73: "+3V3", 74: "GND",
    75: "+3V3", 98: "VCAP", 99: "GND", 100: "+3V3",
    # clocks, reset, boot
    8: "LSE_IN", 9: "LSE_OUT", 12: "HSE_IN", 13: "HSE_OUT", 14: "NRST", 94: "BOOT0",
    # RMII
    24: "ETH_REF_CLK", 25: "ETH_MDIO", 16: "ETH_MDC", 32: "ETH_CRS_DV", 33: "ETH_RXD0",
    34: "ETH_RXD1", 30: "ETH_TX_EN", 51: "ETH_TXD0", 54: "ETH_TXD1", 58: "ETH_PHY_RST",
    # display, SPI3
    78: "EVE_SCK_MCU", 79: "EVE_MISO", 80: "EVE_MOSI_MCU", 77: "EVE_CS", 83: "EVE_PD", 81: "EVE_INT",
    # W5500, SPI4
    43: "W5500_SCK_MCU", 44: "W5500_MISO", 45: "W5500_MOSI_MCU", 42: "W5500_CS",
    40: "W5500_INT", 41: "W5500_RST",
    # NOR, SPI2
    52: "NOR_SCK_MCU", 53: "NOR_MISO", 18: "NOR_MOSI", 57: "NOR_CS",
    # FRAM, I2C1
    92: "FRAM_SCL", 93: "FRAM_SDA",
    # service UART, SWD
    68: "UART_TX", 69: "UART_RX", 72: "SWDIO", 76: "SWCLK", 89: "SWO",
    # misc
    15: "V24_SENSE", 59: "LED_RUN", 60: "LED_FAULT", 38: "HWREV0", 39: "HWREV1",
}
part("U3", "MCU_ST_STM32H5:STM32H573VITx", "STM32H573VIT6", "Package_QFP:LQFP-100_14x14mm_P0.5mm",
     S, mcu, mpn="STM32H573VIT6", note="STM32H563VIT6 is pin-compatible")
for i, ref in enumerate(["C20", "C21", "C22", "C23", "C24", "C25"]):
    c(ref, "100n", S, "+3V3", "GND", note="one per VDD pin, VDDUSB included")
c("C26", "4u7", S, "+3V3", "GND", fp=C0805)
fb("FB1", "600R@100MHz", S, "+3V3", "+3V3A", note="VDDA / VREF+ filter")
c("C27", "1u", S, "+3V3A", "GND")
c("C28", "100n", S, "+3V3A", "GND")
c("C29", "2u2", S, "VCAP", "GND", note="VCAP, CHECK value and count against the H5 datasheet")
c("C30", "2u2", S, "VCAP", "GND")
part("Y1", "Device:Crystal_GND24", "25MHz", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm", S,
     {1: "HSE_IN", 3: "HSE_OUT", 2: "GND", 4: "GND"}, note="HSE, CL 10 pF class. CHECK against the DK")
c("C31", "12p", S, "HSE_IN", "GND", note="CHECK for the crystal's CL")
c("C32", "12p", S, "HSE_OUT", "GND")
part("Y2", "Device:Crystal", "32.768kHz", "Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm", S,
     {1: "LSE_IN", 2: "LSE_OUT"}, note="CL 6 pF, ESR per AN2867")
c("C33", "6p8", S, "LSE_IN", "GND", note="AN2867, CHECK for the crystal")
c("C34", "6p8", S, "LSE_OUT", "GND")
c("C35", "100n", S, "NRST", "GND")
part("SW1", "Switch:SW_Push", "RESET", "Button_Switch_SMD:SW_SPST_B3U-1000P", S, {1: "NRST", 2: "GND"})
r("R20", "10k", S, "BOOT0", "GND")
part("JP1", "Connector_Generic:Conn_01x02", "BOOT0", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
     S, {1: "+3V3", 2: "BOOT0"}, note="fit a jumper for the ST bootloader")
part("J5", "Connector:Conn_ARM_JTAG_SWD_10", "SWD", "Connector_PinHeader_1.27mm:PinHeader_2x05_P1.27mm_Vertical_SMD",
     S, {1: "+3V3", 2: "SWDIO", 3: "GND", 4: "SWCLK", 5: "GND", 6: "SWO", 9: "GND", 10: "NRST"})
part("J6", "Connector_Generic:Conn_01x06", "SERVICE UART", "Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical",
     S, {1: "GND", 4: "UART_RX", 5: "UART_TX"},
     note="FTDI TTL-232R-3V3 order: 1 GND, 2 CTS, 3 VCC (not connected), 4 cable TXD, 5 cable RXD, 6 RTS")
r("R21", "1k", S, "LED_RUN", "LED_RUN_A")
part("D10", "Device:LED", "green RUN", LED0603, S, {2: "LED_RUN_A", 1: "GND"})
r("R22", "1k", S, "LED_FAULT", "LED_FAULT_A")
part("D11", "Device:LED", "red FAULT", LED0603, S, {2: "LED_FAULT_A", 1: "GND"})
r("R23", "10k", S, "HWREV0", "GND", note="hardware revision 0: both pulled down")
r("R24", "10k", S, "HWREV1", "GND")
r("R25", "10k", S, "+3V3", "HWREV0", dnp=True)
r("R26", "10k", S, "+3V3", "HWREV1", dnp=True)

# ---------------------------------------------------------------- client Ethernet (LAN8742A)
S = "eth_client"
part("U4", "Interface_Ethernet:LAN8742A", "LAN8742A-CZ",
     "Package_DFN_QFN:VQFN-24-1EP_4x4mm_P0.5mm_EP2.5x2.5mm_ThermalVias", S,
     {1: "+3V3_PHYA", 2: "PHY_LED2", 3: "PHY_LED1", 4: "PHY_XTAL2", 5: "PHY_XTAL1", 6: "PHY_VDDCR",
      7: "ETH_RXD1", 8: "ETH_RXD0", 9: "+3V3", 11: "ETH_CRS_DV", 12: "ETH_MDIO",
      13: "ETH_MDC", 14: "PHY_REFCLKO", 15: "ETH_PHY_RST", 16: "ETH_TX_EN", 17: "ETH_TXD0",
      18: "ETH_TXD1", 19: "+3V3_PHYA", 20: "ETH1_TXN", 21: "ETH1_TXP", 22: "ETH1_RXN",
      23: "ETH1_RXP", 24: "PHY_RBIAS", 25: "GND"},
     mpn="LAN8742A-CZ-TR (-40 to 85 C)",
     note="straps: MODE[2:0] internal pull-ups = all capable; PHYAD0 internal pull-down = address 0; "
          "nINTSEL pulled low (R33) = pin 14 is REFCLKO, 50 MHz to the MCU; REGOFF internal pull-down = regulator on")
fb("FB2", "600R@100MHz", S, "+3V3", "+3V3_PHYA")
c("C40", "1u", S, "+3V3_PHYA", "GND")
c("C41", "100n", S, "+3V3_PHYA", "GND", note="at pin 1")
c("C42", "100n", S, "+3V3_PHYA", "GND", note="at pin 19")
c("C43", "100n", S, "+3V3", "GND", note="VDDIO")
c("C44", "1u", S, "PHY_VDDCR", "GND", note="VDDCR, with C45")
c("C45", "470p", S, "PHY_VDDCR", "GND")
r("R30", "12k1 1%", S, "PHY_RBIAS", "GND")
part("Y3", "Device:Crystal_GND24", "25MHz", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm", S,
     {1: "PHY_XTAL1", 3: "PHY_XTAL2", 2: "GND", 4: "GND"}, note="CL 20 pF, +/-50 ppm total (LAN8742A datasheet)")
c("C46", "33p", S, "PHY_XTAL1", "GND", note="for CL 20 pF with about 4 pF stray")
c("C47", "33p", S, "PHY_XTAL2", "GND")
r("R31", "22R", S, "PHY_REFCLKO", "ETH_REF_CLK", note="series, at the PHY")
r("R32", "1k5", S, "+3V3", "ETH_MDIO")
r("R33", "10k", S, "PHY_LED2", "GND", note="strap nINTSEL = 0 (REFCLKO mode)")
r("R34", "10k", S, "+3V3", "ETH_PHY_RST", note="PHY runs if the firmware leaves PD11 as input")
# line side: 49.9 R pull-ups to the centre-tap supply (LAN8742A datasheet, confirmed)
fb("FB3", "600R@100MHz", S, "+3V3", "ETH1_CT")
c("C48", "22n", S, "ETH1_CT", "GND")
c("C49", "100n", S, "ETH1_CT", "GND")
for ref, net in (("R35", "ETH1_TXP"), ("R36", "ETH1_TXN"), ("R37", "ETH1_RXP"), ("R38", "ETH1_RXN")):
    r(ref, "49R9 1%", S, net, "ETH1_CT")
part("U5", "Power_Protection:TPD4E05U06DQA", "TPD4E05U06", "Package_SON:USON-10_2.5x1.0mm_P0.5mm", S,
     {1: "ETH1_TXP", 2: "ETH1_TXN", 3: "GND", 4: "ETH1_RXP", 5: "ETH1_RXN", 8: "GND"},
     note="flow-through: route each line across pins 1/10, 2/9, 4/7, 5/6")
part("J2", "Connector:RJ45_Wuerth_7499010121A", "CLIENT", "Connector_RJ:RJ45_Wuerth_7499010121A_Horizontal", S,
     {1: "ETH1_TXP", 2: "ETH1_CT", 3: "ETH1_TXN", 4: "ETH1_RXP", 5: "ETH1_CT", 6: "ETH1_RXN",
      8: "CHASSIS", "SH": "CHASSIS", 12: "ETH1_LED1_A", 11: "GND", 10: "ETH1_LED2_A", 9: "GND"},
     mpn="Wurth 7499010121A (MagJack, 1500 Vrms, LEDs)", note="client network")
r("R39", "330R", S, "PHY_LED1", "ETH1_LED1_A", note="LED1 = link/activity, active high (REGOFF strap low)")
r("R40", "330R", S, "PHY_LED2", "ETH1_LED2_A", note="LED2 = speed, active high (nINTSEL strap low)")

# ---------------------------------------------------------------- I/O Ethernet (W5500)
S = "eth_io"
w = {1: "ETH2_TXN", 2: "ETH2_TXP", 3: "GND", 4: "+3V3_WA", 5: "ETH2_RXN", 6: "ETH2_RXP",
     8: "+3V3_WA", 9: "GND", 10: "W5500_EXRES", 11: "+3V3_WA", 14: "GND", 15: "+3V3_WA",
     16: "GND", 17: "+3V3_WA", 19: "GND", 20: "W5500_TOCAP", 21: "+3V3_WA", 22: "W5500_1V2O",
     23: "GND", 25: "W5500_LINKLED", 27: "W5500_ACTLED", 28: "+3V3", 29: "GND",
     30: "W5500_XI", 31: "W5500_XO", 32: "W5500_CS", 33: "W5500_SCK", 34: "W5500_MISO",
     35: "W5500_MOSI", 36: "W5500_INT", 37: "W5500_RST", 48: "GND"}
part("U6", "Interface_Ethernet:W5500", "W5500", "Package_QFP:LQFP-48_7x7mm_P0.5mm", S, w,
     mpn="W5500 (-40 to 85 C)",
     note="PMODE[2:0] internal pull-ups = all capable, auto-negotiation; RSVD pin 23 to ground, RSVD 38-42 NC; VBG open. "
          "No auto-MDIX: the I/O board's LAN8742A does HP auto-MDIX, so a straight cable works")
fb("FB4", "600R@100MHz", S, "+3V3", "+3V3_WA")
c("C50", "10u", S, "+3V3_WA", "GND", fp=C0805)
for ref in ("C51", "C52", "C53", "C54", "C55", "C56"):
    c(ref, "100n", S, "+3V3_WA", "GND", note="one per AVDD pin")
c("C57", "100n", S, "+3V3", "GND", note="VDD pin 28")
c("C58", "10n", S, "W5500_1V2O", "GND")
c("C59", "4u7", S, "W5500_TOCAP", "GND", fp=C0805)
r("R50", "12k4 1%", S, "W5500_EXRES", "GND")
part("Y4", "Device:Crystal_GND24", "25MHz", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm", S,
     {1: "W5500_XI", 3: "W5500_XTAL2", 2: "GND", 4: "GND"},
     note="own crystal, CL 18 pF, +/-30 ppm (W5500 datasheet); separate from the MCU's HSE")
c("C60", "18p", S, "W5500_XI", "GND", note="W5500 reference circuit")
c("C61", "18p", S, "W5500_XTAL2", "GND")
r("R62", "1M", S, "W5500_XI", "W5500_XO", note="W5500 reference circuit")
r("R63", "0R", S, "W5500_XO", "W5500_XTAL2", note="drive-level option, W5500 reference circuit")
r("R51", "33R", S, "W5500_SCK_MCU", "W5500_SCK", note="series, at the MCU")
r("R52", "33R", S, "W5500_MOSI_MCU", "W5500_MOSI", note="series, at the MCU")
r("R53", "10k", S, "+3V3", "W5500_CS")
r("R54", "10k", S, "+3V3", "W5500_INT")
r("R55", "10k", S, "+3V3", "W5500_RST")
# line side per the WIZnet W5500 reference design. CHECK against it before layout
r("R56", "49R9 1%", S, "ETH2_TXP", "+3V3_WA")
r("R57", "49R9 1%", S, "ETH2_TXN", "+3V3_WA")
r("R58", "49R9 1%", S, "ETH2_RXP", "ETH2_RXTERM")
r("R59", "49R9 1%", S, "ETH2_RXN", "ETH2_RXTERM")
c("C62", "6n8", S, "ETH2_RXTERM", "GND")
c("C63", "10n", S, "ETH2_RCT", "GND")
c("C64", "100n", S, "+3V3_WA", "GND", note="TX centre tap")
part("U7", "Power_Protection:TPD4E05U06DQA", "TPD4E05U06", "Package_SON:USON-10_2.5x1.0mm_P0.5mm", S,
     {1: "ETH2_TXP", 2: "ETH2_TXN", 3: "GND", 4: "ETH2_RXP", 5: "ETH2_RXN", 8: "GND"})
part("J3", "Connector:RJ45_Wuerth_7499010121A", "I/O ONLY", "Connector_RJ:RJ45_Wuerth_7499010121A_Horizontal", S,
     {1: "ETH2_TXP", 2: "+3V3_WA", 3: "ETH2_TXN", 4: "ETH2_RXP", 5: "ETH2_RCT", 6: "ETH2_RXN",
      8: "CHASSIS", "SH": "CHASSIS", 12: "ETH2_LED1_A", 11: "W5500_LINKLED", 10: "ETH2_LED2_A",
      9: "W5500_ACTLED"},
     mpn="Wurth 7499010121A", note="I/O boards only; silkscreen I/O ONLY")
r("R60", "330R", S, "+3V3", "ETH2_LED1_A", note="link LED; W5500 LED outputs sink (active low)")
r("R61", "330R", S, "+3V3", "ETH2_LED2_A", note="activity LED")

# ---------------------------------------------------------------- memories and display
S = "periph"
part("U8", "Memory_Flash:W25Q128JVS", "W25Q128JV", "Package_SO:SOIC-8_5.23x5.23mm_P1.27mm", S,
     {1: "NOR_CS", 2: "NOR_MISO", 3: "NOR_WP", 4: "GND", 5: "NOR_MOSI", 6: "NOR_SCK", 7: "NOR_HOLD",
      8: "+3V3"}, mpn="W25Q128JVSIQ", note="event log and data logger (H3)")
c("C70", "100n", S, "+3V3", "GND")
r("R70", "10k", S, "+3V3", "NOR_CS")
r("R71", "10k", S, "+3V3", "NOR_WP")
r("R72", "10k", S, "+3V3", "NOR_HOLD")
r("R73", "33R", S, "NOR_SCK_MCU", "NOR_SCK", note="series, at the MCU")
part("U9", "cems_core:MB85RC1MT", "MB85RC1MT", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", S,
     {2: "GND", 3: "GND", 4: "GND", 5: "FRAM_SDA", 6: "FRAM_SCL", 7: "GND", 8: "+3V3"},
     mpn="MB85RC1MTPNF", note="128 KB FRAM (H2), I2C address 0x50. A1 = A2 = 0, WP low = writes enabled (pinout per datasheet)")
c("C71", "100n", S, "+3V3", "GND")
r("R74", "2k2", S, "+3V3", "FRAM_SCL")
r("R75", "2k2", S, "+3V3", "FRAM_SDA")
part("J4", "Connector_Generic:Conn_01x20", "RVT70 RiBUS",
     "Connector_FFC-FPC:Hirose_FH12-20S-0.5SH_1x20-1MP_P0.50mm_Horizontal", S,
     {1: "+3V3", 2: "GND", 3: "EVE_SCK", 4: "EVE_MISO", 5: "EVE_MOSI", 6: "EVE_CS", 7: "EVE_INT",
      8: "EVE_PD", 17: "+5V", 18: "+5V", 19: "GND", 20: "GND"},
     mpn="Hirose FH12-20S-0.5SH(55)",
     note="RiBUS pinout from the RVT70HSBNWC00 V1.1A datasheet; 9-12 (GPIO, audio, QSPI) left open; "
          "13-16 NC. CHECK contact side against the cable at mechanical layout")
r("R76", "33R", S, "EVE_SCK_MCU", "EVE_SCK", note="series, at the MCU")
r("R77", "33R", S, "EVE_MOSI_MCU", "EVE_MOSI", note="series, at the MCU")
c("C72", "10u", S, "+3V3", "GND", fp=C0805, note="at J4")
c("C73", "100n", S, "+3V3", "GND", note="at J4")
c("C74", "10u", S, "+5V", "GND", fp=C0805, note="at J4, backlight")
c("C75", "100n", S, "+5V", "GND", note="at J4")
