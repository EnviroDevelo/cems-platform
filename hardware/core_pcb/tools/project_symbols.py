"""Pin tables for the I/O parts that are missing from the KiCad 7 libraries.

gen_schematic.py draws each one as a plain box (ic_symbol) into cems_core.kicad_sym.
Every table was read from the maker's datasheet through a summarising web fetch, so each
part carries CHECK in design.py until a person has compared it pin by pin with the PDF.
Entries: (pin number, name, KiCad electrical type). Left side = logic (MCU) side,
right side = field or analog side. None leaves a gap.
"""

PI, PO, BI, PW, PS, NC, OC = "input", "output", "bidirectional", "power_in", "passive", "no_connect", "open_collector"

ICS = {}


def ic(name, desc, footprint, left, right, datasheet=""):
    ICS[name] = dict(desc=desc, footprint=footprint, left=left, right=right, datasheet=datasheet)


# Infineon ISOFACE ISO2H823V2.5: 8 x 0.6 A isolated high-side, SPI, PG-VQFN-70 12 x 12 mm
ic("ISO2H823V2.5", "8 ch isolated 24 V high-side switch 0.6 A, SPI, diagnostics (Infineon ISOFACE). CHECK pinout",
   "cems_core:Infineon_PG-VQFN-70_12x12mm_P0.5mm",
   [(37, "VCC", PW)] + [(n, "GND", PW) for n in (33, 34, 35, 39, 46, 47, 58, 59, 60)] + [None] +
   [(36, "SEL", PI), (41, "SYNC", PI), (40, "ODIS", PI), (42, "WR", PI), (43, "ALE/RST", PI),
    (44, "RD/MS0", PI), (45, "MS1", PI), (38, "CLKADJ", PS), None,
    (48, "~{CS}", PI), (54, "AD5/SCLK", PI), (49, "AD0/SDI", PI), (56, "AD7/SDO", PO),
    (53, "AD4/~{CRCERR}", PO), (57, "~{ERR}", OC), None,
    (50, "AD1", BI), (51, "AD2", BI), (52, "AD3", BI), (55, "AD6", BI), (23, "NC", NC), (70, "NC", NC)],
   [(n, "VBB", PW) for n in (2, 3, 6, 11, 12, 17, 20, 21, 26)] + [(n, "GNDBB", PW) for n in (1, 22, 27, 32, 61)] + [None] +
   [(24, "OUT0", PO), (25, "OUT0", PO), (18, "OUT1", PO), (19, "OUT1", PO), (15, "OUT2", PO), (16, "OUT2", PO),
    (13, "OUT3", PO), (14, "OUT3", PO), (9, "OUT4", PO), (10, "OUT4", PO), (7, "OUT5", PO), (8, "OUT5", PO),
    (4, "OUT6", PO), (5, "OUT6", PO), (68, "OUT7", PO), (69, "OUT7", PO), None,
    (28, "OLADJ", PS), (29, "IADJ", PS), (30, "VDDIO", PS), (31, "VCORE", PS), None,
    (62, "LEDX0", PS), (63, "LEDX1", PS), (64, "LEDX2", PS), (65, "LEDY0", PS), (66, "LEDY1", PS), (67, "LEDY2", PS)],
   "https://www.infineon.com/part/ISO2H823V2.5")

# Infineon ISOFACE ISO1I813T: 8 ch isolated 24 V digital input, SPI, wire-break, TSSOP-48
ic("ISO1I813T", "8 ch isolated 24 V digital input IEC 61131-2 type 1/2/3, SPI, wire-break (Infineon ISOFACE). CHECK pinout",
   "Package_SO:TSSOP-48_6.1x12.5mm_P0.5mm",
   [(5, "VCC", PW)] + [(n, "GND", PW) for n in (1, 7, 10, 11, 18, 24)] + [None] +
   [(2, "SEL", PI), (3, "SYNC", PI), (19, "MS0", PI), (20, "MS1", PI), (21, "DC_ENA", PI), (4, "ROSC", PS), None,
    (16, "~{CS}", PI), (13, "SCLK", PI), (8, "SDI", PI), (15, "SDO", PO), (14, "SSI", PI), (9, "SSO", PO),
    (12, "~{CRCERR}", PO), (6, "~{ERR}", OC), (22, "SW1", PO), (23, "SW2", PO), (17, "NC", NC)],
   [(26, "VBB", PW)] + [(n, "GNDBB", PW) for n in (25, 31, 37, 43, 48)] + [None, (36, "TS", PS), (38, "WB", PS), None] +
   [(28, "I0H", PI), (27, "I0L", PS), (30, "I1H", PI), (29, "I1L", PS), (33, "I2H", PI), (32, "I2L", PS),
    (35, "I3H", PI), (34, "I3L", PS), (40, "I4H", PI), (39, "I4L", PS), (42, "I5H", PI), (41, "I5L", PS),
    (45, "I6H", PI), (44, "I6L", PS), (47, "I7H", PI), (46, "I7L", PS)],
   "https://www.infineon.com/part/ISO1I813T")

# ST ISO808A-1: 8 x 1 A isolated high-side, SPI, PowerSO-36 (tab = VCC)
ic("ISO808A-1", "8 ch galvanically isolated 24 V high-side driver 1 A, SPI (ST). CHECK pinout",
   "Package_SO:HSOP-36-1EP_11.0x15.9mm_P0.65mm_SlugDown",
   [(2, "VDD", PW), (15, "GNDDD", PW), None, (3, "OUT_EN", PI), (4, "~{SS}", PI), (5, "CLK", PI), (6, "SDI", PI),
    (13, "SDO", PO), (14, "~{FAULT}", OC), (7, "PGOOD", OC), None] +
   [(n, "NC", NC) for n in (1, 8, 9, 10, 11, 12, 16, 17, 18, 20)],
   [(37, "VCC(TAB)", PW), (19, "GNDCC", PW), None,
    (35, "OUT1", PO), (36, "OUT1", PO), (33, "OUT2", PO), (34, "OUT2", PO), (31, "OUT3", PO), (32, "OUT3", PO),
    (29, "OUT4", PO), (30, "OUT4", PO), (27, "OUT5", PO), (28, "OUT5", PO), (25, "OUT6", PO), (26, "OUT6", PO),
    (23, "OUT7", PO), (24, "OUT7", PO), (21, "OUT8", PO), (22, "OUT8", PO)],
   "https://www.st.com/en/power-management/iso808a.html")

# TI ADS124S08: 12 ch 24-bit delta-sigma ADC, TQFP-32
ic("ADS124S08", "24-bit 4 kSPS 12 ch delta-sigma ADC, PGA, IDACs, reference (TI). CHECK pinout",
   "Package_QFP:TQFP-32_7x7mm_P0.8mm",
   [(15, "IOVDD", PW), (16, "DVDD", PW), (14, "DGND", PW), None,
    (9, "~{CS}", PI), (11, "SCLK", PI), (10, "DIN", PI), (12, "DOUT/~{DRDY}", PO), (13, "~{DRDY}", PO),
    (8, "START/SYNC", PI), (18, "~{RESET}", PI), (17, "CLK", PI), (25, "NC", NC)],
   [(26, "AVDD", PW), (27, "AVSS", PW), (28, "AVSS-SW", PS), (23, "REFOUT", PS), (24, "REFCOM", PS),
    (30, "REFP0", PS), (29, "REFN0", PS), None,
    (7, "AIN0", PI), (6, "AIN1", PI), (5, "AIN2", PI), (4, "AIN3", PI), (3, "AIN4", PI), (2, "AIN5", PI),
    (32, "AIN6/REFP1", PI), (31, "AIN7/REFN1", PI), (22, "AIN8", PI), (21, "AIN9", PI), (20, "AIN10", PI),
    (19, "AIN11", PI), (1, "AINCOM", PI)],
   "https://www.ti.com/product/ADS124S08")

# ADI AD4112: 24-bit ADC, 8 x +/-10 V inputs and 4 x 0-20 mA inputs, LFCSP-40
ic("AD4112", "24-bit ADC, 8 x +/-10 V and 4 x 0-20 mA inputs, internal reference (Analog Devices). CHECK pinout",
   "Package_CSP:LFCSP-40-1EP_6x6mm_P0.5mm_EP3.9x3.9mm",
   [(20, "IOVDD", PW), (21, "DGND", PW), (22, "REGCAPD", PS), None,
    (17, "~{CS}", PI), (16, "SCLK", PI), (15, "DIN", PI), (14, "DOUT/~{RDY}", PO), (18, "~{ERROR}", BI),
    (19, "~{SYNC}", PI), (12, "XTAL1", PS), (13, "XTAL2/CLKIO", BI), (25, "GPO0", PO), (38, "GPO1", PO), None,
    (10, "DNC", NC), (23, "DNC", NC), (24, "DNC", NC)],
   [(9, "AVDD", PW), (8, "AVSS", PW), (41, "EP", PW), (7, "REGCAPA", PS), (6, "REFOUT", PS), (40, "REF+", PS),
    (39, "REF-", PS), (11, "VBIAS-", PS), None,
    (2, "VIN0", PI), (3, "VIN1", PI), (4, "VIN2", PI), (5, "VIN3", PI), (26, "VIN4", PI), (27, "VIN5", PI),
    (28, "VIN6", PI), (29, "VIN7", PI), (1, "VINCOM", PI), None,
    (34, "IIN0+", PI), (33, "IIN0-", PI), (35, "IIN1+", PI), (32, "IIN1-", PI), (36, "IIN2+", PI), (31, "IIN2-", PI),
    (37, "IIN3+", PI), (30, "IIN3-", PI)],
   "https://www.analog.com/en/products/ad4112.html")

# TI ISO7741: quad digital isolator, 3 forward / 1 reverse, SOIC-16 wide
ic("ISO7741", "Quad digital isolator, 3 forward / 1 reverse, 5 kVrms (TI). CHECK pinout",
   "Package_SO:SOIC-16W_7.5x10.3mm_P1.27mm",
   [(1, "VCC1", PW), (2, "GND1", PW), (8, "GND1", PW), (7, "EN1", PI), None,
    (3, "INA", PI), (4, "INB", PI), (5, "INC", PI), (6, "OUTD", PO)],
   [(16, "VCC2", PW), (9, "GND2", PW), (15, "GND2", PW), (10, "EN2", PI), None,
    (14, "OUTA", PO), (13, "OUTB", PO), (12, "OUTC", PO), (11, "IND", PI)],
   "https://www.ti.com/product/ISO7741")
