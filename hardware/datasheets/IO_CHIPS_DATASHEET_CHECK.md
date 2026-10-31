# I/O chip datasheet check (CORE PCB rev 0.2), 31 Oct 2026

The vendor PDFs could not be saved: this environment's network policy refuses direct
downloads from analog.com, ti.com, infineon.com, st.com and farnell.com (proxy 403). Each datasheet below
was read through a web reader instead, and the facts were extracted from it. Treat these as
extracts and confirm them against the PDF when one is to hand.

| Part | Source read | Result |
|---|---|---|
| AD4112 | analog.com data sheet (ad4112.pdf); Linux `drivers/iio/adc/ad7173.c` | Pin table matches the symbol. Pin 24 DNC is tied to AVSS inside; EP to AVSS |
| ADS124S08 | ti.com SBAS660 (ads124s08.pdf) | Pin table matches |
| ISO2H823V2.5 | infineon-iso2h823v2.5-datasheet-en.pdf | Pin table matches. /ODIS is ACTIVE LOW (fixed). Two exposed pads (GND, GNDBB) |
| ISO1I813T | farnell 2333739.pdf (Infineon datasheet) | Pin table matches |
| ISO808A-1 | farnell 3972503.pdf (ST DS14170 Rev 1) | Pin table matches; OUT_EN low disables; FAULT, PGOOD open drain |
| ISO7741 | ti.com iso7741.pdf | Pin table matches; F suffix = outputs low when side 1 is unpowered |
| ISOW7741 | ti.com isow7741.pdf | Pin table matches; VSEL to VISOOUT = 5 V (110 mA), to GND2 = 3.3 V (140 mA); EN_IOx low = outputs high-Z; EN/FLT is at VIO level |
| MAX6369 | analog.com MAX6369-MAX6374.pdf | SET 010 = 30 ms start-up delay and timeout. WDO is open drain and PULSES about 100 ms, then re-arms (so the board now latches it) |

## AD4112 facts for the driver (node/drivers/drv_ad4112.c)

From the datasheet and the Linux ad7173 driver (supports AD4111/AD4112):

- SPI mode 3 (CPOL 1, CPHA 1). SCLK high and low at least 25 ns each. The board runs it at 4 MHz through the ISOW7741.
- Communications: write the address to COMMS (0x00); bit 6 = read (0x40). The DATA read command is 0x44. Reset = 64 SCLKs with DIN high and CS low.
- Registers: STATUS 0x00 (read), ADCMODE 0x01 (reset 0x2000), IFMODE 0x02, REGCHECK 0x03, DATA 0x04, GPIOCON 0x06, ID 0x07, CH0-15 0x10-0x1F, SETUPCON0-7 0x20-0x27, FILTCON0-7 0x28-0x2F, OFFSET0-7 0x30-0x37 (reset 0x800000), GAIN0-7 0x38-0x3F.
- **ID = 0x30DX: mask 0xFFF0, expect 0x30D0** (the driver has 0x30E0, which is wrong).
- CHx: bit 15 CH_EN, bits 14:12 SETUP_SEL, INPUT[9:0] = AINPOS[9:5] and AINNEG[4:0]. VINx = x, VINCOM = 0x10, temperature sensor = 17/18, REF+/- = 21/22.
  - VIN0..VIN5 against VINCOM: 0x010, 0x030, 0x050, 0x070, 0x090, 0x0B0 (the driver's codes are right).
  - IIN0..IIN3: 0x1E8, 0x1C9, 0x1AA, 0x18B (right).
  - Pair VINCOM only with the divided inputs (VIN0-7 all are on AD4112).
- SETUPCONx: bit 12 BI_UNIPOLAR, bits 11:10 REFBUF+/-, bits 9:8 INBUF, bits 5:4 REF_SEL (00 external REF+/REF-, 01 external 2, 10 internal 2.5 V, 11 AVDD-AVSS).
  - Voltage channels: 0x1320 (bipolar, input buffers on, internal reference): right.
  - **Current channels: the datasheet says to DISABLE the input buffers.** Use a second setup, 0x1020, and give the IIN channels SETUP_SEL 1.
- ADCMODE: bit 15 REF_EN, bit 13 SING_CYC, bits 6:4 MODE (0 continuous, 1 single: AD717x family values, not in the extract; 4 internal offset (zero-scale) cal, 5 internal full-scale cal, 6 system zero, 7 system full), bits 3:2 CLOCKSEL (00 internal). 0x8000 is right. The board ties REFOUT to REF+, so REF_EN must be 1 whichever REF_SEL is used.
- IFMODE: bit 6 DATA_STAT appends the status byte; its 4 LSBs are the channel; STATUS bit 7 = /RDY.
- FILTCONx: bit 15 SINC3_MAP, bit 11 ENHFILTEN, bits 10:8 ENHFILT, bit 6 ORDER, bits 5:0 ODR. Sinc5+sinc1 ODR codes (SPS): 0-5 6211, 6 5181, 7 4444, 8 3115, 9 2597, 10 1007, 11 503.8, 12 381, 13 200.3, 14 100.5, 15 59.52, **16 49.68**, 17 20.01, 18 16.33, 19 10, 20 5, 21 2.5, 22 1.25.
  - **The driver's FILT_V 0x0508 gives ODR 8, about 3.1 kSPS, not 50 SPS.** For about 50 SPS use 0x0510 (ODR 16, 49.68 SPS, which notches 50 Hz mains). For 50 and 60 Hz rejection use the enhanced filter, ENHFILTEN = 1.
- Transfer functions, N = 24, VREF = 2.5 V:
  - Voltage inputs (internal ÷10 divider, ±20 V range): bipolar code = 2^23 × (VIN × 0.1 / VREF + 1).
  - Current inputs (internal 50 Ω): bipolar code = 2^23 × (IIN × 50 / VREF + 1). The current inputs have their own factory gain value (0x5XXXX0) in GAINx.
- Board mapping (CORE_PCB.md 14):
  - IIN0-3: 4-20 mA ch 1-4.
  - VIN0/1: ch 5/6 on 250 Ω shunts.
  - VIN2: level.
  - VIN3: mass flow.
  - VIN4: the 10 V supply ÷2.
  - VIN5: the level 5 V.

## Other facts the drivers rely on

- **ISO2H823V2.5:**
  - SEL high = serial. MS1:MS0 = 11 = mode 3 (CRC, register access).
  - SYNC has a pull-up, and a low lasting more than about 300 µs trips the chip's watchdog. RST (pin 43) has a pull-down.
  - CLKADJ: 10 kΩ high-precision resistor. IADJ: 6.81 kΩ only. OLADJ: 2.3k-25k (0.5-5 mA open-load threshold).
  - Caps: VDDIO 1 µF, VCORE 470 nF.
- **ISO1I813T:**
  - SPI modes (MS1, MS0): 00 = 8-bit, daisy chain; 01 = 16-bit, daisy chain; 10 = 16-bit; 11 = 24-bit with CRC. Max 5 MHz.
  - DC_ENA low when VBB is fed. Rosc 22.1k gives about 500 kHz. VBB 9.6-35 V. TS: type 2 = 33k, type 3 = 330k.
- **ISO808A-1:**
  - SPI, OUT_EN active-high enable (low disables).
  - FAULT and PGOOD are open drain, active low. VDD 2.75-5.5 V.
- **ADS124S08:**
  - CLK to DGND gives the internal 4.096 MHz oscillator. Max SCLK 10 MHz.
  - /RESET is active low. REFOUT needs 1-47 µF.

## ADS124S08 facts (checked 31 Oct against SBAS660, Zephyr ads1x4s0x, variegated-ads124s08 crate)

- SYS 0x09, reset 0x10: SYS_MON[7:5] (000 off, 001 PGA short, 010 internal temperature sensor, 011 AVDD/4, 100 DVDD/4, 101/110/111 burn-out 0.2/1/10 µA), CAL_SAMP[4:3], TIMEOUT bit 2, CRC bit 1, SENDSTAT bit 0.
- Internal temperature sensor: 129 mV at 25 °C, 403 µV/°C. Burn-out sources 0.2/1/10 µA (±8/4/2 %).
- STATUS 0x01: POR bit 7, /RDY bit 6, P/N rail flags bits 5:2, REF_L1 bit 1, REF_L0 bit 0.
- DATARATE 0x04: G_CHOP 7, CLK 6, MODE 5 (1 = single shot), FILTER 4 (1 = low latency), DR[3:0] (0000 2.5, 0001 5, 0010 10, 0011 16.6, 0100 20, 0101 50, 0110 60, 0111 100, 1000 200 ... 1101 4000 SPS).
- PGA 0x03: PGA_EN[4:3] 01 = on, 00 = bypass; GAIN[2:0]. REF 0x05: REFSEL 00 REFP0/N0, 01 REFP1/N1, 10 internal; REFCON 00 off, 01 on, 10 always on; buffer bits are active-low disables.
- SPI mode 1 (CPOL 0, CPHA 1); SCLK at most 10 MHz. CLK to DGND = internal 4.096 MHz.
- NOT verified (register-map and CRC pages missing from every copy read): the CRC-8 seed and coverage (x^8+x^2+x+1, seed 0xFF, STATUS + data is the usual statement) and the DEV_ID code. Confirm on the bench: a wrong seed fails every frame, which the driver reports as IOB_TS_ADC.
