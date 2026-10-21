# VM/MUX application screens and record editors

Rev B, 21 Oct 2026: as built in `src/vm_ui.c` on the core platform's screen API (`ui_app.h`,
decision D26). Rev A (20 Oct) was the request to the core thread. The platform draws these the
same way on the touchscreen (tabs before its own Status screen) and on the web page, and every
write goes through the parameter table: login level, LOCAL/REMOTE, "are you sure?" for the
valve-moving commands (`PF_CONFIRM`), and the application's interlocks.

## 1. IDs used by the screens

| Kind | IDs | Where |
|---|---|---|
| Settings (persisted) | 1000-1155, valve names 1200-1258 | `include/vm_params.h` (`VMP_*`), generated |
| New settings | 2900-2906 | `src/vm_params_new.inc` |
| Records (sequences, schedules) | 3123-3125, 3164-3166 | `PARAM_RECORD`, `src/vm_records.c` |
| Modbus views (read) | 4000-4194 | `include/vm_params_modbus.h` (`VMMB_*`) |
| Screen commands (one write = one press of the old button) | 5000-5019 | `include/vm_hmi.h` |
| Screen status (button states, dialogs, enables) | 5100-5141 | `include/vm_hmi.h` |
| Display values (read only, not stored, not on Modbus) | 5150-5159 | `include/vm_ui.h` |

Valve names have consecutive IDs (inlet *i* = 1200 + *i* - 1, outlet 1220 + ..., MUX inlet
1240 + ...) so a grid labels its items with them; a blank name shows the position number.

## 2. Tabs

| Tab | Content |
|---|---|
| **Valve Mgr** | Modbus mode / Scheduler / Manual mode (5000-5002, lit 5100-5102, greyed 5103-5105); Start (5005, shown while 5107); Time left (5150) and, with two interconnections, tube 2 (5151); **Inlets** and **Outlets** grids (below); dialogs: manual mode Yes/No (5106: 5003/5004), no sequence (5108: OK 5008), sequence uses an unfitted valve (5109: OK 5009); readings AI1, AI2 (4145[0..1]), RTD1, RTD2 in degrees (5158, 5159) |
| **Multiplexor** | the three mode buttons (5010-5012, 5120-5122, 5123-5125) and Start (5015, shown while 5127), all hidden while `mux_enable` (2906) is off; step time left (5152); **Inlets** grid (visible 5131[i], open 5132[i], touchable while 5128, tap = 5016 with the inlet number, names 1240+); Purge (5017, lit 5133); dialogs 5126 (5013/5014), 5129 (OK 5018), 5130 (OK 5019); "Multiplexor enabled" |
| **Sequences** | editors: Sequence 1, Schedule 1; Sequence 2 and Schedule 2 while two interconnections; MUX Channels while the MUX is enabled (the MUX calendar only while `VM_MUX_CONTINUOUS` is 0); manual-run times (1088-1090, ms) and the mode at power-up (1057) |
| **Layout** | valve outputs (1091), interconnections (1093), inlets/outlets per tube (1064, 1065, 1120, 1121), back-flush valve and output (1051, 1050), MUX enable (2906), MUX valves (1094), purge output (1055) |
| **Process** | flow validation (1067), its input (1066) and delay (1068), lock valves if the analyser is not OK (1061), close the VM after link loss (2901, min), valve feedback settle time (2900, s) |
| **Inputs** | per analog input: enabled, Low (4 mA) and High (20 mA) points, alarm, range min/max; per RTD: enabled, shift, alarm, range min/max (0.1 degree), alarm delay (2902/2903), hysteresis (2904/2905). The array elements are read-only on the panel until request R6; they are changed on the web page's settings list |
| **Names** | VM inlet and outlet names per picture position (shown up to the positions in use, 5157), MUX inlet names (shown for the fitted inlets) |

## 3. The VM valve grids

The old picture was one row of 9 positions (19 when a tube has more than 9 valves) for the inlets
and one for the outlets: tube 1 from the left, tube 2 from the right (4F1). The grids keep that
row, so a tap writes the same position the old picture did (`vm_inlet_touch` 5006 /
`vm_outlet_touch` 5007): position *i* is shown while 5153[*i*] / 5155[*i*], touchable while
5110[*i*] / 5111[*i*], lit while 5154[*i*] / 5156[*i*] (the route `DO_Assign` drove this scan,
I-13). Tube 2's valves are numbered from the right, as in the original.

## 4. Editors (records)

| ID | Record | Editor |
|---|---|---|
| 3164 / 3165 | `Sequence1` / `Sequence2` | table, up to 20 periods: Name (12), Time (s), Inlet 1..inlets of that tube, Outlet 1..outlets |
| 3166 | `SequenceMUX` | table, up to 20 channels: Inlet (shown 1-based, stored 0-based), Dwell, Sample (s; channel time = dwell + sample), Pre-pump, Pump lead (s) |
| 3123 / 3124 / 3125 | `Schedule1` / `Schedule2` / `ScheduleMUX` | form: from / to year, months, days of the month, weekdays (Sun..Sat), start and end time (h:m:s) |

The editor keeps the numbers inside each column's range (the inlet column's maximum follows the
tube's inlet count). On Save the application's check (`vm_check_record`) sees the whole record: it
writes the row numbers (1..n, 0 after; the period chains end on the first 0) and clears the rows
past the last period, then refuses anything the logic would stall on, with the reason, e.g.
"Period 2: inlet 4 is not fitted (interconnection 1 has 3)". Records are never on Modbus.
Without `PF_ANY_MODE` they are edited in LOCAL from the panel and in REMOTE from the web page.

## 5. Not carried over

Settings/password popups (platform login), email settings (platform mail), the Modbus client test
screen (dropped), the Log screen (platform data log + CSV download), screen saver and brightness
(platform UI; `Touched_Bit` still follows the panel, R3), the old MUX DO-mapping screen's toggles
(they opened valves in any mode, U3).
