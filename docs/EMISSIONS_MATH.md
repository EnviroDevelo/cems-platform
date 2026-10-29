# Emissions calculation engine

The CORE turns analyser readings into the values an emission limit is judged on: normalised,
dry, corrected to the reference oxygen, averaged and validated. This document is the design and
the evidence: every formula with its source, the order the steps run in, how quality travels
through them, the averaging rules, and the test vectors that prove the code
(`platform/core/emis.c`, `tests/test_emis.c`).

Code: `platform/include/emis.h` (the engine, no parameters, no HAL), `platform/core/emis_bind.c`
(parameters 400-469, the scan, retained block 910).

## 1. Sources

| Source | What it gives the engine | How it was checked |
|---|---|---|
| Directive 2010/75/EU (IED) Annex VI Part 3 point 1 | Standard conditions for waste plants: 273.15 K, 101.3 kPa, dry, 11 % O2 (3 % for waste oil) | Text read (legislation.gov.uk) |
| IED Annex VI Part 6 point 1.3 | 95 % confidence intervals at the daily ELV: CO 10 %, SO2 20 %, NO2 20 %, dust 30 %, TOC 30 %, HCl 40 %, HF 40 % | Text read |
| IED Annex VI Part 6 point 2.4 | Water vapour need not be measured when the sample is dried before analysis | Text read |
| IED Annex VI Part 6 point 2.7 | Normalise with the Part 7 formula; for hazardous waste with gas cleaning, only when the measured O2 is above the standard | Text read |
| IED Annex VI Part 7 | E_S = (21 - O_S) / (21 - O_M) x E_M | Text read (also the Walloon transposition) |
| IED Annex VI Part 8 point 1.2 | Half-hour and 10-minute averages within the effective operating time, after subtracting the confidence interval; daily averages from those validated averages; at most 5 half-hours a day discarded; at most 10 days a year | Text read |
| IED Annex V Part 3 points 7, 9, 10 | Large combustion plants: O2, T, p, H2O measured; CIs CO 10 %, SO2 20 %, NOx 20 %, dust 30 %; validated hourly averages after CI subtraction; a day with more than 3 invalid hours is invalid; more than 10 such days a year needs action | Text read |
| IED Annex V Part 1 | Reference O2: 6 % solid fuels, 3 % liquid and gaseous fuels, 15 % gas turbines and engines | From the Directive; not re-read for this work |
| EN 14181:2014 clause 6.4 | The QAL2 calibration function y = a + b x is found and applied in the AMS's measuring conditions; results are then taken to standard conditions | From the standard (not freely available); confirm against your copy |
| EN 14181:2014 clause 6.5 | Valid calibration range: 0 to the highest calibrated QAL2 value plus an extension; a new QAL2 when more than 5 % of a week's values are above it for 5 consecutive weeks, or more than 40 % in one week | From the standard; **confirm the extension (this design: 10 % of the ELV, a setting) against your copy**. SEPA's guide QG3 states "the highest value of the SRM (+10 %) dictates the valid calibration range" |
| EN 17255-1:2019 | Data acquisition and handling systems: what a DAHS must calculate, keep and report | Paywalled; the engine follows the IED rules it implements |
| EN 15267-1/-2/-3 | QAL1 certification of the analysers (and, with EN 17255, of DAHS software) | Outside the engine; noted in section 8 |
| NSW EPA, Approved Methods for the Sampling and Analysis of Air Pollutants in NSW (2022), Appendix 1 | Reference conditions 0 degC, 101.3 kPa, dry; eq 1.5 O2 correction; eq 1.6 CO2 correction (x 12 / CO2); eq 1.7 NOx as NO2; eq 2.6 mg/m3 = ppm x MW / 22.4; CEM-1..13 follow US EPA Performance Specifications | Text read (epa.nsw.gov.au) |

## 2. The steps, in order

For each channel, every scan:

```
x       raw reading, the analyser's own unit and conditions          bank 0 (RAW)
        derived channel: x = x_self + x_other (NOx = NO + NO2, both ppm)
y       = a + b x        QAL2 calibration function (EN 14181 6.4)
                         b = 0: no QAL2 yet, y = x, status NO_QAL2
c_std   to the reference temperature and pressure, then to dry        bank 1 (STD)
c_ref   to the reference O2 (or CO2)                                  bank 2 (REF)
m       mass emission kg/h = c_std x Q_N,dry / 1e6                    bank 3 (MASS)
        while the plant operates: time-weighted mean of valid c_ref   bank 4 (RUN)
        at the end of each clock-aligned period: the mean            bank 5 (AVG)
        minus the confidence interval                                bank 6 (VAL)
        at the end of each day: the mean of the day's validated ones bank 7 (DAY)
```

The calibration function goes first because EN 14181 derives it from SRM values converted to the
AMS's measuring conditions (wet, actual O2), so it must be applied to the raw signal before
anything else touches it. The averages are taken of the corrected instantaneous values (each
second's value is normalised and O2-corrected, then averaged), not by correcting averaged
concentrations with an averaged O2.

## 3. Formulas

| # | Step | Formula | Source |
|---|---|---|---|
| F1 | Molar volume | V_m = R T_ref / p_ref; R = 8.314462618 J/(mol K); 22.414 L/mol at 273.15 K, 101.325 kPa | Ideal gas; NSW eq 2.6 uses 22.4 |
| F2 | ppm to mg/m3 | c = ppm x k x M / V_m (k: factor, e.g. 3 for TOC calibrated on propane, expressed as C) | NSW eq 2.6; EN 12619 for TOC as C |
| F3 | Actual to standard (concentration) | c_std = c_act x (T / T_ref) x (p_ref / p) | NSW eq 1.2, 1.3 (inverted for a concentration) |
| F4 | Actual to standard (flow) | Q_std = Q_act x (T_ref / T) x (p / p_ref) | NSW eq 1.2, 1.3 |
| F5 | Wet to dry (concentration, O2) | c_dry = c_wet x 100 / (100 - H2O) | NSW eq 1.1; IED Annex VI Pt 3 pt 1 "after correcting for the water vapour content" |
| F6 | Wet to dry (flow) | Q_dry = Q_wet x (100 - H2O) / 100 | NSW eq 1.1 |
| F7 | O2 reference | c_ref = c_dry x (21 - O_ref) / (21 - O_meas,dry) | IED Annex VI Part 7; NSW eq 1.5 |
| F8 | CO2 reference | c_ref = c_dry x CO2_ref / CO2_meas | NSW eq 1.6 (CO2_ref usually 12 %) |
| F9 | NOx as NO2 | ppm(NO) + ppm(NO2), then F2 with M(NO2) = 46.0055 | NSW eq 1.7 (NO x 46/30 + NO2) |
| F10 | Mass emission | kg/h = c_std (mg/Nm3, dry, not O2-corrected) x Q (Nm3/h, dry) / 1e6 | Mass balance: the O2 correction is a legal normalisation, not real gas |
| F11 | Validated average | v = avg - CI, CI = ci_pct / 100 x ELV_daily; v < 0 reported as 0 (setting) | IED Annex VI Pt 8 1.2, Annex V Pt 3 10 |
| F12 | Daily average | mean of the day's validated short-term averages | IED Annex VI Pt 8 1.2 |

Molar masses to configure (g/mol): NO 30.006, NO2 46.0055 (NOx as NO2), SO2 64.066, CO 28.010,
HCl 36.461, HF 20.006, NH3 17.031, CH4 16.043, N2O 44.013, H2S 34.081, C 12.011 (TOC as C).

The O2 used in F7 is dry. A zirconia probe (in-situ, hot) measures wet O2: configure it as a wet %
channel and the engine dries it with F5 before using it.

## 4. Quality

Every reading arrives with one of the platform's eight quality codes (plat_state.h: valid, stale,
unavailable, out of range, warming up, maintenance, calibration, fault). No new codes.

A calculated value is as bad as the worst thing it was made from (`emis_q_worst`), ranked
FAULT > UNAVAILABLE > STALE > CALIBRATION > MAINTENANCE > WARMING UP > OUT OF RANGE > VALID: a
broken or missing input outranks the instrument's own modes. Inputs per bank:

| Bank | Made from |
|---|---|
| STD | the reading (and the added reading), the H2O reading if wet, T and p if mg/m3 actual or flow actual |
| REF | STD, plus the O2 (or CO2) channel's STD if corrected |
| MASS | STD and the flow channel's STD |

The engine also gives OUT OF RANGE itself when: the raw reading is outside the analyser range
setting (status OVER_RANGE / BELOW_RANGE), the dry O2 is above the O2 limit (20 % default:
21 - O2 heads for zero; status O2_HIGH, B_REF is then computed with the limit and is not
averaged), the water content is outside 0-80 %, the CO2 is below 0.5 %, or a result is not finite.
UNAVAILABLE when an input is missing (status INPUT_MISSING).

Only VALID values go into averages. The quality of an average is VALID, or the worst reason seen
while it was being collected; of a day, VALID or the worst reason among its discarded averages.

## 5. Averages and validity

- **Periods** are clock-aligned (local standard time, as the rest of the platform): 10, 30 or 60
  minutes (setting 416), one period for the whole plant. Waste incineration: 30 min; large
  combustion plants: 60 min. The extra 10-minute CO averages of Annex VI Pt 8 1.1(d) need a second
  period on one channel: not built yet (a follow-up if a waste plant needs it).
- **Effective operating time.** Only time when the plant is operating (parameter 422, written by
  the application from its "fuel/waste firing" signal, or by a PLC) counts. A period with no
  operating time has no average and is not a discard. A day with no operating time has no daily
  value and is not an invalid day.
- **Valid short-term average:** the valid time is at least `min_avail` of the operating time in the
  period (setting 417, default 67 % = two thirds). The IED does not fix this number; two thirds is
  the usual national practice (Germany, UK). It is a setting so a permit's own number can be used.
- **Valid day:** no more than `max_discard` short-term averages discarded (setting 418): 5 for
  half-hours (Annex VI Pt 8 1.2), 3 for hours (Annex V Pt 3 10). Parameter 468 counts the days
  invalid this calendar year (IED: more than 10 needs action by the operator).
- **Confidence interval** subtraction is on by default (IED). It is a setting (419) because NSW
  and most US-derived licences compare the unadjusted value with the limit.
- **Clock.** A clock step backwards, or forward past a period, ends the period and day in progress
  where they are. No clock (never set): live values only, no averages.
- **Configuration change.** Any change restarts the engine: the periods in progress are dropped, so
  an average never mixes two configurations. The change is in the event log as a parameter change.
- **Power cycle.** The periods and the day in progress, the last averages and the counters are kept
  in retained block 910 (refreshed once a minute and at power fail), and carry on after a restart
  if the configuration is unchanged. The minutes while the power was off are simply not operating
  time. (On the 32 KB development FRAM there is no room: a restart drops the period in progress.)
- **Valid calibration range** (EN 14181 6.5): 0 to ys,max + cal_ext % of the ELV (settings 439, 421).
  Live bit ABOVE_CAL on each value above it. Each week (Monday to Sunday) the short-term averages
  above it are counted: more than 5 % in each of 5 consecutive weeks, or more than 40 % in one week,
  sets QAL2_REVIEW (latched until the configuration changes, i.e. a new QAL2 is entered).

## 6. Test vectors (tests/test_emis.c)

| # | Input | Expected | Test |
|---|---|---|---|
| 1 | V_m at 273.15 K, 101.325 kPa | 22.41397 L/mol | t_formulas |
| 2 | SO2 100 ppm | 285.831 mg/Nm3 | t_formulas |
| 3 | NO 100 ppm as NO2 | 205.254 mg/Nm3 | t_formulas |
| 4 | CO 50 ppm | 62.483 mg/Nm3 | t_formulas |
| 5 | TOC 10 ppm propane, as C | 16.076 mg C/Nm3 | t_formulas |
| 6 | 100 mg/Nm3 at 7 % O2 to 11 % | 71.429 | t_formulas |
| 7 | 100 wet at 15 % H2O | 117.647 dry | t_formulas |
| 8 | 50 mg/m3 at 150 degC, 100 kPa | 78.484 mg/Nm3 | t_formulas |
| 9 | Hot-wet: NO 150 + NO2 10 ppm wet, QAL2 a = 2, b = 1.05, H2O 12 %, zirconia O2 8 % wet; flow 100 000 m3/h at 150 degC, 100 kPa | O2 dry 9.0909 %; NOx 396.51 mg/Nm3 dry; 332.95 at 11 % O2; flow 56 062.6 Nm3/h dry; 22.230 kg/h | t_chain |
| 10 | 30 min at 40, ELV 50, CI 10 % | avg 40, validated 35 | t_short |
| 11 | 20 min valid, 10 min calibration | kept (2/3) | t_short |
| 12 | 19 min valid, 11 min maintenance | discarded, quality MAINT | t_short |
| 13 | 10 min operating, all valid | kept: the rule is against operating time | t_short |
| 14 | 5 half-hours lost: day valid (mean 35); 6 lost: invalid, counted; plant off all day: neither; LCP 4 hours lost: invalid | t_day |
| 15 | ys,max 30, ELV 50: 36 flagged, 34 not; a week with 50 % of averages above: QAL2 review | t_cal_range |

Also proved: worst-of ranking, every input's quality reaching the right banks, over-range and NaN
inputs, fixed H2O, O2 limit, "only above", CO2 reference, the clock (unset, stepped back, new
year), retention across a power cycle, the configuration check, and the parameter binding.

## 7. Settings (parameters, admin level, persisted, never writable from Modbus)

Plant: 400 enable, 401 T_ref K, 402 p_ref kPa, 403 diluent (0 none, 1 O2, 2 CO2), 404 reference %,
405 O2 limit, 406 correct only above, 407 O2 channel, 408 H2O mode (0 dried sample, 1 measured, 2
fixed), 409 H2O channel, 410 fixed H2O %, 411/412 T and p channels (255 = fixed values 413/414),
415 flow channel, 416 period s, 417 min valid %, 418 max discards a day, 419 subtract CI, 420
validated below zero as zero, 421 calibration range extension % of ELV, 422 plant operating
(operator level, the application or a PLC writes it), 423 RO configuration error.

Per channel (arrays of 32, index = Modbus standard-block slot): 430 source measurement (255 none),
431 unit (1 ppm, 2 %, 3 mg/m3 actual, 4 mg/m3 standard, 5 flow actual, 6 flow standard, 7 plain),
432 wet, 433 O2-corrected, 434 added channel, 435 molar mass, 436 factor, 437/438 QAL2 a and b, 439
ys,max, 440 daily ELV, 441 CI %, 442/443 analyser range.

Results (read only, arrays of 32): 450-457 banks 0-7, 460-465 qualities (raw, calc, mass, average,
day, running), 466 status bits, 467 averages discarded today, 468 days invalid this year, 469 weeks
above the calibration range.

Status bits (466; bits 0-4 belong to the Modbus block: NE 107 and forced): 5 NO_QAL2, 6 ABOVE_CAL,
7 OVER_RANGE, 8 O2_HIGH, 9 INPUT_MISSING, 10 AVG_INCOMPLETE, 11 DAY_INVALID, 12 CI_OFF, 13
FIXED_H2O, 14 BELOW_RANGE, 15 QAL2_REVIEW.

## 8. Australia (NSW) differences

- **Reference conditions** are the same in substance: dry, 0 degC, 101.3 kPa (Approved Methods
  Appendix 1). Set p_ref to 101.3 for an NSW licence if the licence says so (0.03 % difference).
- **Reference diluent** comes from the licence or the POEO (Clean Air) Regulation 2022 for the plant
  type; some use O2, some 12 % CO2 (eq 1.6). Both are supported.
- **No confidence interval subtraction**: NSW compares the measured value with the limit. Set 419 off.
- **CEMS quality assurance** is to US EPA Performance Specifications and Procedures (CEM-1 to
  CEM-13: PS 2 SO2/NOx, PS 3 O2/CO2, PS 4 CO, PS 6 flow, PS 11 dust, PS 12A Hg, PS 18 HCl), with RATA
  and cylinder gas audits, not EN 14181 QAL2. There is no calibration function to apply: leave b = 0
  (status NO_QAL2 is then expected, not a fault) or enter a correction the licence approves.
- **Averaging periods and data capture** come from the licence (often 1 hour or a block average);
  set 416-418 to match.
- Non-detects are reported as "less than the limit of reporting", not zero (Approved Methods 5.4):
  for reporting tools, not the engine.

## 9. Outside the engine

QAL1 (EN 15267 certification of the analysers), QAL3 drift and precision checks (CUSUM or Shewhart
charts on zero and span), the annual surveillance test (AST), and the annual reports are not done
here. The CORE keeps the calibration records (plat_cal.h) and the data log; a QAL3 chart can be
added later on top of the calibration records if a site needs it.

Open points to confirm against a purchased copy of EN 14181:2014: the exact extension of the valid
calibration range (setting 421, default 10 % of the ELV) and whether the weekly test counts
short-term averages (this design) or individual values.
