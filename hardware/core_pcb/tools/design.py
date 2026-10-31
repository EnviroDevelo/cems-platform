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
    ("io_dig", "Field digital I/O", "Output gate, bank A 8 x 0.6 A (ISO2H823V2.5), bank B 8 x 1 A (ISO808A-1), 8 inputs (ISO1I813T)"),
    ("io_pwr", "Field power, pump, relays, USB", "Field supplies A and B, pump switch / PWM / DIR / tacho, 2 relay coil outputs, USB-C"),
    ("io_ana", "Analog inputs and temperatures", "Isolated: 6 x 4-20 mA, level and mass-flow ports (AD4112); 6 temperatures (ADS124S08)"),
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
    34: "ETH_RXD1", 30: "ETH_TX_EN_MCU", 51: "ETH_TXD0_MCU", 54: "ETH_TXD1_MCU", 58: "ETH_PHY_RST",
    # display, SPI3
    78: "EVE_SCK_MCU", 79: "EVE_MISO", 80: "EVE_MOSI_MCU", 77: "EVE_CS", 83: "EVE_PD", 81: "EVE_INT",
    # W5500, SPI4
    43: "SPI4_SCK_MCU", 44: "SPI4_MISO", 45: "SPI4_MOSI_MCU", 42: "W5500_CS",
    40: "W5500_INT", 41: "W5500_RST",
    # NOR, SPI2
    52: "NOR_SCK_MCU", 53: "NOR_MISO", 18: "NOR_MOSI", 57: "NOR_CS",
    # FRAM, I2C1
    92: "FRAM_SCL", 93: "FRAM_SDA",
    # PSRAM, OCTOSPI1 (AF9: PB2 CLK, PB10 NCS, PC2 IO2; AF6: PB1 IO0, PB0 IO1, PA6 IO3)
    37: "PSRAM_CLK_MCU", 47: "PSRAM_NCS", 36: "PSRAM_IO0", 35: "PSRAM_IO1", 17: "PSRAM_IO2", 31: "PSRAM_IO3",
    # service UART, SWD
    68: "UART_TX", 69: "UART_RX", 72: "SWDIO", 76: "SWCLK", 89: "SWO",
    # misc
    15: "V24_SENSE", 59: "LED_RUN", 60: "LED_FAULT", 38: "HWREV0", 39: "HWREV1",
    4: "ROLE_A", 5: "ROLE_B",
    # I/O node section (sheets io_dig, io_pwr, io_ana): chip selects on the shared SPI4
    1: "CS_A", 2: "CS_B", 3: "CS_IN", 46: "CS_T", 97: "CS_AI", 84: "CS_EXP", 88: "EXP_INT",
    82: "A_ERR", 7: "IN_ERR", 61: "B_FAULT", 62: "B_PGOOD",
    95: "GATE_EN", 96: "WDI", 55: "GATE_FB",
    67: "PUMP_PWM", 23: "PUMP_TACH", 63: "PUMP_DIR", 64: "PUMP_EN", 90: "REL1_N", 91: "REL2_N",
    65: "USB_VBUS_SNS", 70: "USB_DM", 71: "USB_DP",
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
# Board role (CORE or I/O node), read at boot beside hw_rev: CORE_PCB.md 12
part("J7", "Connector_Generic:Conn_01x03", "ROLE", "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical",
     S, {1: "ROLE_A", 2: "GND", 3: "ROLE_B"},
     note="shunt with pull tab; silkscreen 1-2 CORE / 2-3 I/O NODE / none = OFF; reachable only with the housing open")
r("R27", "10k", S, "+3V3", "ROLE_A")
r("R28", "10k", S, "+3V3", "ROLE_B")
c("C36", "100n", S, "ROLE_A", "GND", note="ESD, at J7")
c("C37", "100n", S, "ROLE_B", "GND")

# ---------------------------------------------------------------- client Ethernet (LAN8742A)
S = "eth_client"
part("U4", "Interface_Ethernet:LAN8742A", "LAN8742A-CZ",
     "Package_DFN_QFN:VQFN-24-1EP_4x4mm_P0.5mm_EP2.5x2.5mm_ThermalVias", S,
     {1: "+3V3_PHYA", 2: "PHY_LED2", 3: "PHY_LED1", 4: "PHY_XTAL2", 5: "PHY_XTAL1", 6: "PHY_VDDCR",
      7: "PHY_RXD1", 8: "PHY_RXD0", 9: "+3V3", 11: "PHY_CRS_DV", 12: "ETH_MDIO",
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
# 33R source series terminations on the other RMII lines (as on ST's DK): TX at the MCU,
# RX at the PHY. They tame the 50 MHz edges (reflections, EMC); the RX ones sit between the
# PHY's MODE/strap pins and the MCU and do not change the straps (internal pull-ups,
# MODE[2:0] = 111 = all capable with auto-negotiation).
for ref, a_, b_, where in (("R41", "ETH_TX_EN_MCU", "ETH_TX_EN", "MCU"), ("R42", "ETH_TXD0_MCU", "ETH_TXD0", "MCU"),
                           ("R43", "ETH_TXD1_MCU", "ETH_TXD1", "MCU"), ("R44", "PHY_CRS_DV", "ETH_CRS_DV", "PHY"),
                           ("R45", "PHY_RXD0", "ETH_RXD0", "PHY"), ("R46", "PHY_RXD1", "ETH_RXD1", "PHY")):
    r(ref, "33R", S, a_, b_, note=f"series, at the {where}")
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
     30: "W5500_XI", 31: "W5500_XO", 32: "W5500_CS", 33: "SPI4_SCK", 34: "SPI4_MISO",
     35: "SPI4_MOSI", 36: "W5500_INT", 37: "W5500_RST", 48: "GND"}
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
r("R51", "33R", S, "SPI4_SCK_MCU", "SPI4_SCK", note="series, at the MCU")
r("R52", "33R", S, "SPI4_MOSI_MCU", "SPI4_MOSI", note="series, at the MCU")
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
part("U10", "cems_core:APS6404L", "APS6404L", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", S,
     {1: "PSRAM_NCS", 2: "PSRAM_IO1", 3: "PSRAM_IO2", 4: "GND", 5: "PSRAM_IO0", 6: "PSRAM_CLK",
      7: "PSRAM_IO3", 8: "+3V3"},
     mpn="AP Memory APS6404L-3SQR-SN (-ZR for -40 to 105 C)",
     note="8 MB QSPI PSRAM on OCTOSPI1, up to about 100 MHz: keep it next to the MCU, "
          "traces under 30 mm and roughly matched")
c("C76", "100n", S, "+3V3", "GND", note="at U10 VDD")
c("C77", "1u", S, "+3V3", "GND")
r("R78", "33R", S, "PSRAM_CLK_MCU", "PSRAM_CLK", note="series, at the MCU")
r("R79", "10k", S, "+3V3", "PSRAM_NCS", note="keeps the PSRAM deselected through reset")
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


# ================================================================ I/O node section (rev 0.2)
# The same board is a CORE or an I/O node (role jumper J7, CORE_PCB.md 12). Every field
# chip sits on the shared SPI4 (with the W5500) at 4 MHz, one chip select each. Field
# grounds: FIELD_0V (supplies A and B, banks, inputs, pump) is isolated from GND by the
# ISOFACE / ISO808A / ISO7741 parts; GND_T (temperatures) and GND_AI (analog) are each
# their own isolated island. Refs from here: R101+, C101+, U11+, J10+, F10+, D20+, Q1+.
_n = {"R": 100, "C": 100}


def nr(value, sheet, a, b, fp=R0603, **kw):
    _n["R"] += 1
    r(f"R{_n['R']}", value, sheet, a, b, fp=fp, **kw)


def nc(value, sheet, a, b, fp=C0603, **kw):
    _n["C"] += 1
    c(f"C{_n['C']}", value, sheet, a, b, fp=fp, **kw)


R0805 = "Resistor_SMD:R_0805_2012Metric"
R1206 = "Resistor_SMD:R_1206_3216Metric"
SOT23_5 = "Package_TO_SOT_SMD:SOT-23-5"
MCV = "Connector_Phoenix_MC:PhoenixContact_MCV_1,5_{n}-G-3.81_1x{n:02d}_P3.81mm_Vertical"
MSTBVA = "Connector_Phoenix_MSTB:PhoenixContact_MSTBVA_2,5_{n}-G-5,08_1x{n:02d}_P5.08mm_Vertical"
MCV35 = "Connector_Phoenix_MC:PhoenixContact_MCV_1,5_{n}-G-3.5_1x{n:02d}_P3.50mm_Vertical"
IDC40 = "Connector_IDC:IDC-Header_2x20_P2.54mm_Latch_Vertical"
NANO2 = "Fuse:Fuseholder_Littelfuse_Nano2_157x"
PTC1206 = "Fuse:Fuse_1206_3216Metric"
PTC1812 = "Fuse:Fuse_1812_4532Metric"


# Wrong-plug proofing (docs/CORE_PCB.md section 15): three pitch families that cannot mate with
# each other (5.08 mm power and 24 V outputs, 3.81 mm bank B, 3.5 mm isolated sensors), every
# size used once per family except the temperature and analog groups (a swap inside a group is
# harmless), and a Phoenix coding key in each header so no plug fits a larger header shifted along.
def mcv(ref, sheet, nets, value, key=None, pitch="3.81", **kw):
    n = len(nets)
    fp = MCV.format(n=n) if pitch == "3.81" else MCV35.format(n=n)
    keys = ", ".join(map(str, key)) if isinstance(key, tuple) else key
    code = f"; coding key at pin {keys} (header CR-MC, plug coded to match). CHECK coding parts" if key else ""
    part(ref, f"Connector_Generic:Conn_01x{n:02d}", value, fp, sheet,
         {i + 1: net for i, net in enumerate(nets)},
         mpn=kw.pop("mpn", f"Phoenix MCV 1,5/{n}-G-{pitch.replace('.', ',')} + coded, labelled plug{code}"), **kw)


def mstb(ref, sheet, nets, value, key=None, **kw):
    n = len(nets)
    keys = ", ".join(str(k) for k in key) if isinstance(key, tuple) else str(key)
    part(ref, f"Connector_Generic:Conn_01x{n:02d}", value, MSTBVA.format(n=n), sheet,
         {i + 1: net for i, net in enumerate(nets)},
         mpn=f"Phoenix MSTBVA 2,5/{n}-G-5,08 + coded, labelled plug; coding key at pin {keys} "
             "(header CR-MSTB, plug coded to match). CHECK coding parts", **kw)


def gate_and(ref, sheet, a, b, y, note=""):
    part(ref, "74xGxx:74LVC1G08", "74LVC1G08", SOT23_5, sheet, {1: a, 2: b, 3: "GND", 4: y, 5: "+3V3"},
         mpn="SN74LVC1G08DBVR", note=note)
    nc("100n", sheet, "+3V3", "GND")


def inverter(ref, sheet, a, y, vcc="+3V3", gnd="GND", sym="74xGxx:74LVC1G04", note="", dnp=False):
    part(ref, sym, sym.split(":")[1], SOT23_5, sheet, {2: a, 3: gnd, 4: y, 5: vcc},
         mpn=f"SN{sym.split(':')[1]}DBVR", note=note, dnp=dnp)
    nc("100n", sheet, vcc, gnd, dnp=dnp)


# ---------------------------------------------------------------- field digital I/O
S = "io_dig"
# Hardware output gate: all field outputs off within one watchdog period if the firmware
# stops kicking WDI, and off from reset until the firmware sets GATE_EN.
part("U11", "Power_Supervisor:MAX6369", "MAX6369", "Package_TO_SOT_SMD:SOT-23-8", S,
     {1: "WDI", 2: "GND", 4: "GND", 5: "+3V3", 6: "GND", 7: "WDO_N", 8: "+3V3"},
     mpn="MAX6369KA+T",
     note="SET2..0 = 010: 30 ms startup delay and timeout (minimum), so outputs drop within 50 ms. WDO pulses; U13 latches it")
nc("100n", S, "+3V3", "GND")
nr("10k", S, "+3V3", "WDO_N")
nr("100k", S, "GATE_EN", "GND", note="gate shut while the MCU is in reset")
# MAX6369 WDO only pulses (about 100 ms) and then re-arms, so a D flip-flop latches the trip:
# WDO_N clears it, and the firmware re-arms the gate by taking GATE_EN low and then high.
part("U13", "74xGxx:74LVC1G175", "74LVC1G175", "Package_TO_SOT_SMD:SOT-23-6", S,
     {1: "GATE_EN", 2: "GND", 3: "+3V3", 4: "GATE_ARM", 5: "+3V3", 6: "WDO_N"}, mpn="SN74LVC1G175DBVR",
     note="D flip-flop: rising GATE_EN sets GATE_ARM, a watchdog trip clears it until the next rising edge")
nc("100n", S, "+3V3", "GND")
gate_and("U12", S, "GATE_EN", "GATE_ARM", "GATE", note="GATE = GATE_EN and armed (no watchdog trip since)")
nr("1k", S, "GATE", "GATE_FB", note="firmware reads the gate back (PD8)")

# Bank A: 8 x 0.6 A, ISO2H823V2.5, to the breakout over the 40-way ribbon J10
a = {37: "+3V3", 36: "+3V3", 41: "+3V3", 40: "GATE", 43: "GND", 44: "+3V3", 45: "+3V3",
     38: "A_CLKADJ", 48: "CS_A", 54: "SPI4_SCK", 49: "SPI4_MOSI", 56: "SPI4_MISO", 57: "A_ERR",
     28: "A_OLADJ", 29: "A_IADJ", 30: "A_VDDIO", 31: "A_VCORE"}
a.update({n: "GND" for n in (33, 34, 35, 39, 46, 47, 58, 59, 60)})
a.update({n: "+24V_FA" for n in (2, 3, 6, 11, 12, 17, 20, 21, 26)})
a.update({n: "FIELD_0V" for n in (1, 22, 27, 32, 61)})
for ch, pins in enumerate(((24, 25), (18, 19), (15, 16), (13, 14), (9, 10), (7, 8), (4, 5), (68, 69))):
    for p in pins:
        a[p] = f"A_OUT{ch + 1}"
a.update({62: "A_LEDX", 63: "A_LEDX", 64: "A_LEDX", 65: "A_LEDY", 66: "A_LEDY", 67: "A_LEDY"})
part("U14", "cems_core:ISO2H823V2.5", "ISO2H823V2.5", "cems_core:Infineon_PG-VQFN-70_12x12mm_P0.5mm", S, a,
     mpn="Infineon ISO2H823V25XUMA1",
     note="pinout checked against datasheet Rev 2.1 (31 Oct). Serial mode 3: SEL high, MS0/MS1 high, ALE/RST low; "
          "ODIS active low from GATE. No LED matrix: X pins tied, Y pins tied. The package has two exposed pads "
          "(GND and GNDBB): footprint is a placeholder to redraw from the package drawing")
nc("2u2", S, "+3V3", "GND", fp=C0805)
nc("100n", S, "+3V3", "GND")
nr("10k 0.1%", S, "A_CLKADJ", "GND", note="CLKADJ, high-precision 10k (datasheet)")
nr("6k81 1%", S, "A_IADJ", "FIELD_0V", note="IADJ, 6.81k only (datasheet)")
nr("12k1 1%", S, "A_OLADJ", "FIELD_0V", note="open-load threshold, about 1 mA (2.3k-25k allowed: 5-0.5 mA)")
nc("1u 50V", S, "A_VDDIO", "FIELD_0V", fp=C0805)
nc("470n", S, "A_VCORE", "FIELD_0V")
nc("2u2 50V", S, "+24V_FA", "FIELD_0V", fp=C1210)
nc("100n 50V", S, "+24V_FA", "FIELD_0V")
nr("10k", S, "+3V3", "A_ERR")
for ch in range(1, 9):
    nc("12n 100V", S, f"A_OUT{ch}", "FIELD_0V", note="per output, datasheet application circuit" if ch == 1 else "")

# Bank B: 8 x 1 A, ISO808A-1, on a 10-way 3.81 mm pluggable J11
b = {2: "+3V3", 15: "GND", 3: "GATE", 4: "CS_B", 5: "SPI4_SCK", 6: "SPI4_MOSI", 13: "SPI4_MISO",
     14: "B_FAULT", 7: "B_PGOOD", 37: "+24V_FB", 19: "FIELD_0V"}
for ch, pins in enumerate(((35, 36), (33, 34), (31, 32), (29, 30), (27, 28), (25, 26), (23, 24), (21, 22))):
    for p in pins:
        b[p] = f"B_OUT{ch + 1}"
part("U15", "cems_core:ISO808A-1", "ISO808A-1", "Package_SO:HSOP-36-1EP_11.0x15.9mm_P0.65mm_SlugDown", S, b,
     mpn="ST ISO808A-1TR", note="pinout checked against DS14170 (31 Oct). Tab = VCC (+24V_FB): copper pour on the field side only")
nc("100n", S, "+3V3", "GND")
nc("1u", S, "+3V3", "GND")
nc("100n 50V", S, "+24V_FB", "FIELD_0V")
part("C200", "Device:C_Polarized", "100u 50V", "Capacitor_SMD:CP_Elec_10x10", S, {1: "+24V_FB", 2: "FIELD_0V"},
     note="bank B bulk")
nr("10k", S, "+3V3", "B_FAULT")
nr("10k", S, "+3V3", "B_PGOOD")
mcv("J11", S, [f"B_OUT{n}" for n in range(1, 9)] + ["FIELD_0V", "FIELD_0V"], "BANK B",
    note="8 A return over 2 contacts: CHECK the plug rating (8 A per contact)")
for ch in range(1, 9):
    # Same flyback options as the breakout board: D + RZ = plain clamp, Z instead of RZ = fast release
    part(f"D{30 + ch}", "Device:D", "S1M", "Diode_SMD:D_SMA", S, {1: f"B_OUT{ch}", 2: f"B_CL{ch}"},
         note="flyback at the terminal, as on the breakout" if ch == 1 else "")
    nr("0R", S, f"B_CL{ch}", "FIELD_0V", fp=R1206)
    part(f"D{40 + ch}", "Device:D_Zener", "24V 1.5W", "Diode_SMD:D_SMB", S, {1: "FIELD_0V", 2: f"B_CL{ch}"},
         mpn="1SMB5934BT3", dnp=True, note="fast-release option (remove the 0R)" if ch == 1 else "")

# 8 digital inputs: ISO1I813T, type 1/3, to the breakout over the same ribbon J10
d = {5: "+3V3", 2: "+3V3", 3: "+3V3", 19: "+3V3", 20: "+3V3", 21: "GND", 4: "IN_ROSC",
     16: "CS_IN", 13: "SPI4_SCK", 8: "SPI4_MOSI", 15: "SPI4_MISO", 14: "GND", 6: "IN_ERR",
     26: "+24V_FA", 36: "IN_TS", 38: "IN_WB"}
d.update({n: "GND" for n in (1, 7, 10, 11, 18, 24)})
d.update({n: "FIELD_0V" for n in (25, 31, 37, 43, 48)})
for ch, (h, l) in enumerate(((28, 27), (30, 29), (33, 32), (35, 34), (40, 39), (42, 41), (45, 44), (47, 46))):
    d[h], d[l] = f"IN{ch + 1}_H", f"IN{ch + 1}_L"
part("U16", "cems_core:ISO1I813T", "ISO1I813T", "Package_SO:TSSOP-48_6.1x12.5mm_P0.5mm", S, d,
     mpn="Infineon ISO1I813TXUMA1",
     note="serial mode; internal DC/DC off (DC_ENA low), field side from VBB. Values from Infineon's "
          "ISO1I813T evaluation board. Pinout checked against the datasheet (31 Oct): serial mode 3 (24-bit, CRC)")
nc("100n", S, "+3V3", "GND")
nc("10u", S, "+3V3", "GND", fp=C0805)
nc("470n 50V", S, "+24V_FA", "FIELD_0V", fp=C0805)
nc("470n 50V", S, "+24V_FA", "FIELD_0V", fp=C0805)
nr("22k1 1%", S, "IN_ROSC", "GND", note="22.1k: about 500 kHz scan (datasheet typical)")
nr("33R", S, "IN_TS", "FIELD_0V", note="type 1 (eval board RTS = 33 ohm; datasheet: type 2 = 33k, type 3 = 330k). CHECK type 1 value")
nr("33k", S, "IN_WB", "FIELD_0V", note="wire-break current, 25-55k range (datasheet curves)")
nr("10k", S, "+3V3", "IN_ERR")
for ch in range(1, 9):
    nr("12k 1%", S, f"DI{ch}", f"IN{ch}_H", fp=R1206, note="RIN, eval board" if ch == 1 else "")
    nr("2k 1%", S, f"IN{ch}_L", "FIELD_0V", note="RLED (no LED here; the breakout has them)" if ch == 1 else "")

# One 40-way ribbon carries all the digital I/O to one breakout (Gary, 31 Oct: one header,
# so no ribbon can go into the wrong slot). Pins 1-16: valve outputs, each beside its 0 V
# return; 17-32: inputs, each beside a 0 V; 33-40: 4 x +24 V sensor and LED supply, 4 x 0 V.
idc = {}
for i in range(8):
    idc[2 * i + 1], idc[2 * i + 2] = f"A_OUT{i + 1}", "FIELD_0V"
    idc[2 * i + 17], idc[2 * i + 18] = f"DI{i + 1}", "FIELD_0V"
for p in range(33, 41):
    idc[p] = "+24V_RIB" if p % 2 else "FIELD_0V"
part("J10", "Connector_Generic:Conn_02x20_Odd_Even", "DIGITAL I/O IDC40", IDC40, S, idc,
     mpn="Wurth 61204022821 (WR-BHD 40-way, long latches, 3 A). CHECK MPN",
     note="same pinout as the breakout J1, standard straight-through 40-way ribbon")
part("F10", "Device:Polyfuse", "0.75A", PTC1812, S, {1: "+24V_FA", 2: "+24V_RIB"},
     mpn="Bourns MF-MSMF075-2", note="ribbon +24 V: sensor supply and breakout LEDs")

# SPI expansion header (Gary 31 Oct): add-on modules such as a 4 x 4-20 mA analog output board
# plug in here. The module carries its own isolation and isolated supply; the core gives it
# 24 V and 3.3 V (each behind a PTC), SPI4 through series resistors, its own chip select and
# an interrupt. Its MISO enters SPI4 only through a buffer enabled by CS_EXP, so a faulty or
# missing module can never hold the shared bus. Latched, polarised 10-way header: cannot be
# reversed, and no other cable in the system has 10 ways.
part("J26", "Connector_Generic:Conn_02x05_Odd_Even", "SPI EXPANSION", "Connector_IDC:IDC-Header_2x05_P2.54mm_Latch_Vertical", S,
     {1: "+24V_EXP", 2: "GND", 3: "+3V3_EXP", 4: "GND", 5: "EXP_SCK", 6: "GND", 7: "EXP_MOSI", 8: "EXP_MISO",
      9: "EXP_CS", 10: "EXP_INT"},
     mpn="Wurth 61201021621 (WR-BHD 10-way latched) CHECK MPN",
     note="module: 1 +24 V (0.5 A), 3 +3.3 V (0.1 A, logic side of its isolator only), 5 SCK, 7 MOSI, 8 MISO, 9 CS, 10 INT (open drain, low = attention)")
part("F23", "Device:Polyfuse", "0.5A", PTC1206, S, {1: "+24V", 2: "+24V_EXP"}, mpn="Bourns MF-NSMF050-2")
part("F24", "Device:Polyfuse", "0.1A", PTC1206, S, {1: "+3V3", 2: "+3V3_EXP"}, mpn="Bourns MF-NSMF010-2")
nr("100R", S, "SPI4_SCK", "EXP_SCK", note="series: limits a shorted module line, damps the cable")
nr("100R", S, "SPI4_MOSI", "EXP_MOSI")
nr("100R", S, "CS_EXP", "EXP_CS")
part("U39", "74xGxx:74LVC1G125", "74LVC1G125", SOT23_5, S, {1: "CS_EXP", 2: "EXP_MISO", 3: "GND", 4: "SPI4_MISO", 5: "+3V3"},
     mpn="SN74LVC1G125DBVR", note="module MISO onto SPI4 only while CS_EXP is low")
nc("100n", S, "+3V3", "GND")
nr("100k", S, "EXP_MISO", "GND", note="defined level with no module")
nr("10k", S, "+3V3", "EXP_INT")
nc("100n", S, "+24V_EXP", "GND", fp=C0805)
# CS pull-ups: every chip stays deselected while the MCU is in reset
for cs in ("CS_A", "CS_B", "CS_IN", "CS_T", "CS_AI", "CS_EXP"):
    nr("10k", S, "+3V3", cs)

# ---------------------------------------------------------------- field power, pump, relays, USB
S = "io_pwr"
# Field supplies come in on their own terminals, never through the core's 2 A input.
# Reverse polarity: each fuse is followed by a unidirectional TVS that conducts and blows it.
mstb("J13", S, ["FA_IN", "FIELD_0V", "FB_IN", "FIELD_0V"], "FIELD 24V A+B", key=3,
     note="one 4-way plug for both field supplies (12 A per contact): A = bank A, inputs, pump (about 10 A); "
          "B = bank B and relays (about 8 A). Link A and B in the plug for a single supply")
for ref, src, dst, val, note in (("F12", "FA_IN", "+24V_FA", "6.3A T", "bank A, inputs, ribbons"),
                                 ("F13", "FA_IN", "+24V_FP", "5A T", "pump"),
                                 ("F14", "FB_IN", "+24V_FB", "10A T", "bank B, relays")):
    part(ref, "Device:Fuse", val, NANO2, S, {1: src, 2: dst},
         mpn=f"Littelfuse 0453 {val} NANO2 in a 157 series holder", note=note)
for i, net in enumerate(("+24V_FA", "+24V_FP", "+24V_FB")):
    part(f"D{21 + i}", "Device:D_Zener", "SMCJ33A", "Diode_SMD:D_SMC", S, {1: net, 2: "FIELD_0V"},
         mpn="SMCJ33A", note="unidirectional TVS: surge clamp, and blows the fuse on reverse polarity" if i == 0 else "")
    part(f"C{201 + i}", "Device:C_Polarized", "100u 50V", "Capacitor_SMD:CP_Elec_10x10", S, {1: net, 2: "FIELD_0V"})
for i, net in enumerate(("+24V_FA", "+24V_FB")):
    nr("10k", S, net, f"F{'AB'[i]}_LED_A", fp=R0805)
    part(f"D{24 + i}", "Device:LED", f"green FIELD {'AB'[i]}", LED0603, S, {2: f"F{'AB'[i]}_LED_A", 1: "FIELD_0V"})

# Pump: switched supply (IPS7091 smart high-side switch, current limit, behind the gate),
# PWM and DIR as 24 V push-pull outputs, tacho comparator; signals through an ISO7741.
part("U17", "Regulator_Linear:L78L05_SOT89", "L78L05", "Package_TO_SOT_SMD:SOT-89-3", S,
     {3: "+24V_FA", 2: "FIELD_0V", 1: "+5V_P"}, mpn="L78L05ACUTR", note="pump logic 5 V, field side")
nc("330n 50V", S, "+24V_FA", "FIELD_0V")
nc("1u", S, "+5V_P", "FIELD_0V")
gate_and("U19", S, "PUMP_PWM", "GATE", "PUMP_PWM_G")
gate_and("U20", S, "PUMP_EN", "GATE", "PUMP_EN_G")
nr("100k", S, "PUMP_EN", "GND")
nr("100k", S, "PUMP_PWM", "GND")
part("U18", "cems_core:ISO7741", "ISO7741", "Package_SO:SOIC-16W_7.5x10.3mm_P1.27mm", S,
     {1: "+3V3", 2: "GND", 8: "GND", 7: "+3V3", 3: "PUMP_PWM_G", 4: "PUMP_DIR", 5: "PUMP_EN_G", 6: "PUMP_TACH",
      16: "+5V_P", 9: "FIELD_0V", 15: "FIELD_0V", 10: "+5V_P", 14: "P_PWM_F", 13: "P_DIR_F", 12: "P_EN_F",
      11: "P_TACH_F"},
     mpn="TI ISO7741FDWR (F: outputs low when side 1 is unpowered)", note="pinout checked (31 Oct)")
nc("100n", S, "+3V3", "GND")
nc("100n", S, "+5V_P", "FIELD_0V")
part("U21", "Power_Management:IPS7091SPBF", "IPS7091S", "Package_TO_SOT_SMD:TO-263-5_TabPin3", S,
     {1: "FIELD_0V", 2: "P_EN_F", 3: "+24V_FP", 4: "P_DG", 5: "PUMP_PWR"},
     mpn="Infineon IPS7091SPBF", note="about 4 A pump supply with current limit; can PWM a brushed motor to 1 kHz. "
                                      "CHECK rating once the 107Z current is known")
nr("10k", S, "+5V_P", "P_DG", note="diagnostic, test point only")
part("D26", "Device:D_Schottky", "SS56", "Diode_SMD:D_SMC", S, {1: "PUMP_PWR", 2: "FIELD_0V"},
     mpn="SS56 (60 V, 5 A)", note="motor freewheel")
nc("100n 50V", S, "+24V_FP", "FIELD_0V")
# 24 V push-pull: Schmitt inverter, 2N7002, 10k pull-up, BC817/BC807 emitter followers
for sig, name, u, q in (("PWM", "P_PWM", "U22", 1), ("DIR", "P_DIR", "U23", 4)):
    inverter(u, S, f"{name}_F", f"{name}_N", vcc="+5V_P", gnd="FIELD_0V", sym="74xGxx:74LVC1G14")
    part(f"Q{q}", "Transistor_FET:2N7002", "2N7002", "Package_TO_SOT_SMD:SOT-23", S,
         {1: f"{name}_N", 2: "FIELD_0V", 3: f"{name}_B"})
    nr("10k", S, "+24V_FA", f"{name}_B", fp=R0805)
    part(f"Q{q + 1}", "Transistor_BJT:BC817", "BC817", "Package_TO_SOT_SMD:SOT-23", S,
         {1: f"{name}_B", 3: "+24V_FA", 2: f"{name}_E"})
    part(f"Q{q + 2}", "Transistor_BJT:BC807", "BC807", "Package_TO_SOT_SMD:SOT-23", S,
         {1: f"{name}_B", 3: "FIELD_0V", 2: f"{name}_E"})
    nr("1k 1W", S, f"{name}_E", f"{name}_OUT", fp="Resistor_SMD:R_2512_6332Metric", mpn="anti-surge 2512, 1 W",
       note=f"pump {sig}: 24 V push-pull, 20 Hz-25 kHz. 1k makes the output survive a short to 0 V or 24 V "
            "for ever (24 mA, 0.6 W). CHECK the 107Z input current (high level = 24 V - 1k x I)")
# Tacho: 30 V tolerant, 1.5 V threshold, optional pull-up for open-collector tachos
nr("10k", S, "P_TACH_IN", "P_TACH_C", fp=R0805)
part("D27", "Diode:BAT54S", "BAT54S", "Package_TO_SOT_SMD:SOT-23", S,
     {1: "FIELD_0V", 2: "+5V_P", 3: "P_TACH_C"}, note="clamp to the 5 V rail")
nc("1n", S, "P_TACH_C", "FIELD_0V")
nr("10k", S, "+5V_P", "P_TREF")
nr("4k3", S, "P_TREF", "FIELD_0V", note="1.5 V threshold")
part("U24", "Comparator:LMV331", "LMV331", SOT23_5, S,
     {1: "P_TACH_C", 3: "P_TREF", 2: "FIELD_0V", 5: "+5V_P", 4: "P_TACH_F"}, mpn="LMV331IDBVR",
     note="CHECK pin order for the SOT-23-5 variant")
nc("100n", S, "+5V_P", "FIELD_0V")
nr("10k", S, "+5V_P", "P_TACH_F")
nr("1M", S, "P_TACH_F", "P_TACH_C", note="hysteresis")
nr("4k7", S, "+5V_P", "P_TACH_PU")
part("JP2", "Connector_Generic:Conn_01x02", "TACH PULL-UP", "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",
     S, {1: "P_TACH_PU", 2: "P_TACH_IN"}, note="fit for an open-collector tacho")
mstb("J15", S, ["PUMP_PWR", "FIELD_0V", "P_PWM_OUT", "P_DIR_OUT", "P_TACH_IN", "FIELD_0V"], "PUMP", key=(1, 3, 5),
     note="one plug for the whole pump: power, 0 V, PWM, DIR, tacho, signal 0 V (Thomas 107Z: signal ground "
          "must be field 0 V)")

# Relay coil outputs for DIN-socket relays (Finder 40.52 24 V, about 21 mA): PhotoMOS
# switches, LED anodes fed from GATE so the gate drops them too; MCU pin low = on.
for k in (1, 2):
    part("F15" if k == 1 else "F22", "Device:Polyfuse", "50mA", PTC1206, S, {1: "+24V_FB", 2: f"+24V_REL{k}"},
         mpn="Bourns MF-NSMF005-2", note="per-channel limit: a shorted coil wire trips only its own PTC" if k == 1 else "")
    nr("330R", S, "GATE", f"REL{k}_A")
    part(f"U{24 + k}", "Relay_SolidState:CPC1017N", "CPC1017N", "Package_SO:SOP-4_3.8x4.1mm_P2.54mm", S,
         {1: f"REL{k}_A", 2: f"REL{k}_N", 3: f"+24V_REL{k}", 4: f"REL{k}_OUT"}, mpn="IXYS CPC1014NTR",
         note="60 V, 400 mA PhotoMOS: rides out a short until its PTC trips. CHECK with a 10 s short test" if k == 1 else "")
    part(f"D{27 + k}", "Device:D", "S1M", "Diode_SMD:D_SMA", S, {1: f"REL{k}_OUT", 2: "FIELD_0V"})
mstb("J17", S, ["REL1_OUT", "REL2_OUT", "FIELD_0V"], "RELAY COILS", key=1)

# USB-C service port (ST USB bootloader, address, serial number). Self-powered: VBUS only sensed.
part("J18", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C",
     "Connector_USB:USB_C_Receptacle_HCTL_HC-TYPE-C-16P-01A", S,
     {"A4": "VBUS_USB", "B4": "VBUS_USB", "A9": "VBUS_USB", "B9": "VBUS_USB",
      "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "A5": "USB_CC1", "B5": "USB_CC2",
      "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DM", "B7": "USB_DM", "S1": "CHASSIS"},
     mpn="HCTL HC-TYPE-C-16P-01A (or GCT USB4105)")
nr("5k1", S, "USB_CC1", "GND")
nr("5k1", S, "USB_CC2", "GND")
part("U27", "Power_Protection:USBLC6-2SC6", "USBLC6-2SC6", "Package_TO_SOT_SMD:SOT-23-6", S,
     {1: "USB_DP", 6: "USB_DP", 3: "USB_DM", 4: "USB_DM", 2: "GND", 5: "VBUS_USB"}, mpn="USBLC6-2SC6")
nr("33k", S, "VBUS_USB", "USB_VBUS_SNS")
nr("68k", S, "USB_VBUS_SNS", "GND", note="5 V -> 3.4 V")

# ---------------------------------------------------------------- analog inputs and temperatures
S = "io_ana"


def isow(ref, inv, cs, side, vsel_net, note, pre=None, dnp=False):
    """ISOW7741 (isolator + isolated supply) for one SPI chip. Its side-1 MISO output is
    enabled only while that chip's CS is low (EN_IO1 = not CS), so it can share SPI4_MISO."""
    v, g = side
    pre = pre or ("T" if g == "GND_T" else "AI")
    inverter(inv, S, cs, f"{pre}_EN1", dnp=dnp)
    part(ref, "Isolator:ISOW7741", "ISOW7741", "Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm", S,
         {1: "+3V3", 6: "GND", 9: "+5V", 10: "GND", 2: "SPI4_SCK", 3: "SPI4_MOSI", 4: cs, 5: "SPI4_MISO",
          7: f"{pre}_EN1", 8: f"{pre}_ENFLT", 12: v, 20: v, 11: g, 15: g, 13: vsel_net, 14: v,
          19: f"{pre}_SCLK", 18: f"{pre}_DIN", 17: f"{pre}_CS", 16: f"{pre}_DOUT"},
         mpn="TI ISOW7741DFMR", note=note, dnp=dnp)
    nr("10k", S, "+3V3", f"{pre}_ENFLT", note="EN/FLT high (VIO level) enables the DC/DC; reads low on a fault", dnp=dnp)
    nc("10u", S, "+5V", "GND", fp=C0805, dnp=dnp)
    nc("100n", S, "+5V", "GND", dnp=dnp)
    nc("100n", S, "+3V3", "GND", dnp=dnp)
    nc("10u", S, v, g, fp=C0805, dnp=dnp)
    nc("100n", S, v, g, dnp=dnp)


# Temperatures: ADS124S08, 3 connectors x 2 channels (TC, RTD or NTC), as io-node today
isow("U28", "U29", "CS_T", ("+3V3_T", "GND_T"), "GND_T", "VSEL to GND2 = 3.3 V out (140 mA)")
fb("FB10", "600R@100MHz", S, "+3V3_T", "+3V3_TA")
t = {15: "+3V3_T", 16: "+3V3_T", 14: "GND_T", 9: "T_CS", 11: "T_SCLK", 10: "T_DIN", 12: "T_DOUT",
     8: "GND_T", 18: "T_RST", 17: "GND_T", 26: "+3V3_TA", 27: "GND_T", 28: "GND_T", 23: "T_REFOUT",
     24: "GND_T", 30: "T_REFP", 29: "GND_T", 1: "T_AINCOM"}
ain = {0: 7, 1: 6, 2: 5, 3: 4, 4: 3, 5: 2, 6: 32, 7: 31, 8: 22, 9: 21, 10: 20, 11: 19}
for k in range(6):
    t[ain[2 * k]], t[ain[2 * k + 1]] = f"T{k + 1}_P", f"T{k + 1}_N"
part("U30", "cems_core:ADS124S08", "ADS124S08", "Package_QFP:TQFP-32_7x7mm_P0.8mm", S, t,
     mpn="TI ADS124S08IPBSR",
     note="pinout checked (SBAS660, 31 Oct). Internal 4.096 MHz clock (CLK to DGND), START low (conversions by command), DOUT/DRDY on one pin")
nc("100n", S, "+3V3_T", "GND_T")
nc("1u", S, "+3V3_TA", "GND_T")
nc("100n", S, "+3V3_TA", "GND_T")
nc("1u", S, "T_REFOUT", "GND_T", note="REFOUT to REFCOM")
nr("10k", S, "+3V3_T", "T_RST")
nr("5k11 0.1%", S, "T_REFP", "GND_T", note="RTD reference resistor, 10 ppm/K")
nc("100n", S, "T_REFP", "GND_T")
nc("100n", S, "T_AINCOM", "GND_T", note="CHECK AINCOM and the TC bias against the io-node schematic")
for k in range(1, 7):
    nc("10n C0G", S, f"T{k}_P", f"T{k}_N", note="differential filter" if k == 1 else "")
    nr("10M", S, f"T{k}_N", "T_AINCOM", note="open-TC bias" if k == 1 else "")
for j in range(3):
    nr("100R", S, f"T_RTN{j + 1}", "T_REFP", note="RTD return to the reference resistor" if j == 0 else "")
    mcv(f"J{19 + j}", S, [f"T{2 * j + 1}_P", f"T{2 * j + 1}_N", f"T{2 * j + 2}_P", f"T{2 * j + 2}_N", f"T_RTN{j + 1}"],
        f"TEMP{j + 1}", key=1, pitch="3.5", note=f"T{2 * j + 1} +/-, T{2 * j + 2} +/-, RTD return")

# Analog: AD4112 on its own isolated island (ISOW7741 for SPI, ITX2424SA for 24 V loop power)
isow("U31", "U32", "CS_AI", ("+5V_AI", "GND_AI"), "+5V_AI", "VSEL to VISOOUT = 5 V out (110 mA)")
part("U33", "Converter_DCDC:ITX2424SA", "ITX2424SA", "Converter_DCDC:Converter_DCDC_XP_POWER-ITXxxxxSA_THT", S,
     {2: "+24V", 1: "GND", 6: "+24V_AI", 7: "GND_AI"}, mpn="XP Power ITX2424SA (6 W, 1.6 kV)",
     note="loop power 6 x 22 mA + level + mass-flow supply, about 4 W")
nc("10u 50V", S, "+24V", "GND", fp=C1210)
nc("10u 50V", S, "+24V_AI", "GND_AI", fp=C1210)
fb("FB11", "600R@100MHz", S, "+5V_AI", "+5V_AIA")
ai = {20: "+5V_AI", 21: "GND_AI", 22: "AI_REGCAPD", 17: "AI_CS", 16: "AI_SCLK", 15: "AI_DIN", 14: "AI_DOUT",
      18: "AI_ERR", 19: "+5V_AI", 9: "+5V_AIA", 8: "GND_AI", 41: "GND_AI", 7: "AI_REGCAPA", 6: "AI_REF",
      40: "AI_REF", 39: "GND_AI", 11: "GND_AI",
      2: "AI5_IN", 3: "AI6_IN", 4: "LVL_SIG", 5: "FLOW_SIG", 26: "V10_MON", 27: "+5V_LVL", 1: "GND_AI",
      34: "AI1_IN", 35: "AI2_IN", 36: "AI3_IN", 37: "AI4_IN", 33: "GND_AI", 32: "GND_AI", 31: "GND_AI", 30: "GND_AI"}
part("U34", "cems_core:AD4112", "AD4112", "Package_CSP:LFCSP-40-1EP_6x6mm_P0.5mm_EP3.9x3.9mm", S, ai,
     mpn="ADI AD4112BCPZ",
     note="internal oscillator and 2.5 V reference. VIN0/1: 4-20 mA ch 5/6 on shunts; VIN2 level; VIN3 mass flow; "
          "VIN4 10 V monitor; VIN5 5 V monitor. Pinout checked (31 Oct); CHECK the exposed pad size against the CP-40 drawing")
nc("100n", S, "+5V_AI", "GND_AI")
nc("1u", S, "+5V_AIA", "GND_AI")
nc("100n", S, "+5V_AIA", "GND_AI")
nc("1u", S, "AI_REGCAPA", "GND_AI")
nc("100n", S, "AI_REGCAPA", "GND_AI")
nc("1u", S, "AI_REGCAPD", "GND_AI")
nc("100n", S, "AI_REF", "GND_AI")
nr("10k", S, "+5V_AI", "AI_ERR")
for ch in (5, 6):
    nr("250R 0.1%", S, f"AI{ch}_IN", "GND_AI", fp=R1206, mpn="Vishay PTN1206 0.4 W, 10 ppm/K thin film",
       note="4-20 mA shunt -> 1-5 V; takes the 30 mA limit for ever (0.23 W)")
# Loop supplies: a 30 mA two-terminal current limiter per loop, so a loop wire shorted to its
# input or to 0 V can never push more than 30 mA into a shunt or an AD4112 IIN pin (abs max 50 mA).
for ch in range(1, 7):
    part(f"F{15 + ch}", "Device:D", "CCR 30mA", "Diode_SMD:D_PowerDI-123", S, {2: "+24V_AI", 1: f"LOOP{ch}"},
         mpn="Diodes AL5809-30P1-7 (60 V, 30 mA)",
         note="loop current limit; shorted loop = 0.7 W: copper on the cathode. CHECK the 30 mA option, "
              "its 2.5 V headroom and the AD4112 IIN abs max" if ch == 1 else "")
for j in range(3):
    a1, a2 = 2 * j + 1, 2 * j + 2
    mcv(f"J{22 + j}", S, [f"LOOP{a1}", f"AI{a1}_IN", "GND_AI", f"LOOP{a2}", f"AI{a2}_IN", "GND_AI"], f"AI {a1}-{a2}",
        key=3, pitch="3.5",
        note="per channel: 24 V loop, IN, 0 V (2-wire or 4-wire transmitters)")
# Sensor supplies: 5 V level port; precision 10 V for the mass-flow sensor (REF5010 drives
# the LT3045's SET pin, so the output is the reference +/- a few mV, about 0.05 %)
part("U35", "Regulator_Linear:L78L05_SOT89", "L78L05", "Package_TO_SOT_SMD:SOT-89-3", S,
     {3: "+24V_AI", 2: "GND_AI", 1: "+5V_LVL"}, mpn="L78L05ACUTR", note="level sensor 5 V, internal current limit")
nc("330n 50V", S, "+24V_AI", "GND_AI")
nc("1u", S, "+5V_LVL", "GND_AI")
nr("100k", S, "LVL_SIG", "GND_AI", note="defined level when unplugged")
part("U36", "Regulator_Linear:LM317L_SO8", "LM317L", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", S,
     {1: "+24V_AI", 2: "+13V_AI", 3: "+13V_AI", 6: "+13V_AI", 7: "+13V_AI", 4: "AI_ADJ"},
     mpn="LM317LDR", note="13 V pre-regulator, about 0.5 W: copper on the VO pins")
nr("240R 1%", S, "+13V_AI", "AI_ADJ")
nr("2k26 1%", S, "AI_ADJ", "GND_AI", note="1.25 x (1 + 2260/240) = 13.0 V")
nc("1u 25V", S, "+13V_AI", "GND_AI", fp=C0805)
part("U37", "Reference_Voltage:REF5010AD", "REF5010", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", S,
     {2: "+13V_AI", 4: "GND_AI", 6: "REF10", 5: "REF10_NR"}, mpn="REF5010AIDR (0.05 %, 3 ppm/K)")
nc("1u", S, "REF10_NR", "GND_AI")
nc("1u 25V", S, "REF10", "GND_AI", fp=C0805)
part("U38", "Regulator_Linear:LT3045xMSE", "LT3045", "Package_SO:MSOP-12-1EP_3x4mm_P0.65mm_EP1.65x2.85mm", S,
     {1: "+13V_AI", 2: "+13V_AI", 3: "+13V_AI", 4: "+13V_AI", 6: "FLOW_ILIM", 7: "+13V_AI", 8: "REF10",
      9: "GND_AI", 13: "GND_AI", 10: "+10V_FLOW", 11: "+10V_FLOW", 12: "+10V_FLOW"},
     mpn="LT3045EMSE#PBF", note="SET driven by the REF5010 (no RSET). CHECK PGFB tie when PG is unused")
nr("3k", S, "FLOW_ILIM", "GND_AI", note="about 50 mA current limit")
nc("4u7 25V", S, "+13V_AI", "GND_AI", fp=C0805)
nc("10u 25V", S, "+10V_FLOW", "GND_AI", fp=C0805)
nr("100k 0.1%", S, "+10V_FLOW", "V10_MON", note="10 V monitor for ratiometric correction")
nr("100k 0.1%", S, "V10_MON", "GND_AI")
nr("1M", S, "FLOW_SIG", "GND_AI")
# Sensor supply clamps: a 24 V loop wire landing on a sensor supply pin is held by these TVSs
# while the loop's 30 mA limiter carries the current, so neither regulator nor sensor sees 24 V.
part("D49", "Device:D_Zener", "SMAJ5.0A", "Diode_SMD:D_SMA", S, {1: "+5V_LVL", 2: "GND_AI"},
     mpn="SMAJ5.0A", note="clamps +5V_LVL")
part("D50", "Device:D_Zener", "SMAJ11A", "Diode_SMD:D_SMA", S, {1: "+10V_FLOW", 2: "GND_AI"},
     mpn="SMAJ11A", note="clamps +10V_FLOW (12.2 V min breakdown)")
mcv("J25", S, ["+5V_LVL", "LVL_SIG", "GND_AI", "+10V_FLOW", "FLOW_SIG", "GND_AI", "GND_AI"], "SENSORS",
    key=5, pitch="3.5",
    note="level (DFRobot SEN0205): 5 V, SIG, 0 V; mass flow (AWM5104VN): +10 V, OUT, 0 V")

