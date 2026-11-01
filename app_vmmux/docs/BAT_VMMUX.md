# VM/MUX bench acceptance test (BAT) procedure

Rev B, 21 Oct 2026 (Rev B: the new screens; VM manual route held, I-12; time left, F8). For the first bench test of the Valve Manager / Multiplexor application on
real hardware. Each step says what to do, what to watch and what passes. Every behaviour here is
already covered by a host test (`tests/vm_tests.c`, named in brackets); the BAT confirms it on the
board with real I/O, real timing and the real touchscreen, web page and Modbus.

## 0. Set-up

| Item | Needs |
|---|---|
| Board | the MVP board (or the DK with the I/O board) with 20 DI, 20 DO, 2 AI (4-20 mA), 2 RTD wired as in 10_IO_Map |
| Indication | a lamp or LED on every DO used (at least DO1-DO8); switches on DI1-DI6 |
| Signals | a 4-20 mA source on AI1; a PT100 (or decade box) on RTD1 |
| PC | browser on the web page; a Modbus TCP master (e.g. Modbus Poll / mbpoll) on port 502 |
| Firmware | `app/vmmux` build with every `vm_config.h` switch at its default; note the commit on the sheet |
| Start | blank FRAM (new unit), unit in LOCAL mode, logged in as admin on the touchscreen |

Record for every step: pass / fail, time, and anything unexpected. A failure is a finding, not a
reason to stop, unless an output is driven that should not be.

## 1. Power-up and blank unit
1. Power up with blank FRAM. Pass: no output on; CALIBRATION INVALID alarm shown (no calibration
   saved yet, Q6); all VM/MUX counts read 0; MUX enabled (`mux_enable` 2906 = 1). [t_power_up_defaults]
2. Power-cycle. Pass: same state, no output pulses during start-up (watch the lamps).

## 2. Configuration (settings, interlocks, entry behaviour)
1. Set MaxNumberOfValves = 10, NumInterconnections = 1, FirstInlets = 3, FirstOutlets = 2,
   NumberOfMUXValves = 4. Pass: accepted; MUX shows inlets 1-3 (4 valves = 3 inlets + purge).
2. Try FirstInlets = 11 (above MaxValveDOnumber) and NumberOfMUXValves = 13 (above
   MaxNumberOfValves). Pass: refused on touch and web; Modbus not involved. [t_store_fits_and_keeps]
3. Map VM DOInlet = 0,1,2, DOOutlet[3..4] = 3,4 (0-based; outlets follow the inlets), MUX
   DOSingularInlet = 5,6,7 (1-based), DOPurgeInlet = 8. [t_vm_modbus_run_and_link_loss]
4. Power-cycle. Pass: every value kept.

## 3. Discrete inputs and DI alarms
1. Toggle DI1..DI20 one at a time with no function set. Pass: input status follows each, nothing
   else happens. [t_di_mapping]
2. Set VM DI4 function = Alarm, alarm enabled, condition = on TRUE, description "Door open".
   Close DI4. Pass: DI_ALARM_04 raised within about 30 ms with the text "Door open"; event log
   entry; open DI4: alarm clears, stays unacknowledged until acknowledged (no login needed).
   [t_alarms]
3. MUX DI function "Stop Seq." on DI5: tested in 6.4.

## 4. Analog and temperature inputs
1. AI1 enabled, Low point 0, High point 100. Inject 4, 12, 20 mA. Pass: 0, 50, 100 (Low at 4 mA,
   High at 20 mA, open item 9 fix). Remove the source. Pass: AI1_FAULT alarm; logged value BAD.
   [t_ai_scaling_fixed, t_alarms]
2. Range alarm: RangeMin 10, RangeMax 90, alarm enabled. Go to 95 for 5 s then back. Pass: no
   alarm (10 s delay). Hold 95 for 15 s. Pass: AI1_RANGE after about 10 s.
3. RTD1 with TemperatureShift +5 (0.5 degree): set 100.0 ohm (0 degC). Pass: reading 0.5. Range
   alarm with delay 10 s and hysteresis 20 (2.0 degree): cross the limit and come back by 1
   degree. Pass: the alarm stays until 2 degrees inside. [t_rtd_through_logic]
4. Power-cycle. Pass: enables, limits, shift, calibration points, names and units kept (Q5, Q6).
   Then "restore defaults": calibration points and shift kept, user settings back to defaults.
   [t_q5_q6_retained]

## 5. Valve Manager
1. Manual mode (Valve Mgr tab): Manual mode, Yes, Start, touch inlet 2 then outlet 1. Pass: DO2
   and DO4 on and **stay on** (I-12; the original held them for one scan), inlet 2 and outlet 1
   lit; Time left counts down from the manual timer (F8); "firmware update refused: Valve Manager
   manual mode is running". Manual mode again: valves off. [t_panel, t_screen_values]
2. Modbus mode: press Modbus Mode on the touchscreen, switch to REMOTE. From the master: holding
   10-11 = 3,600,000 (ModbusTimer.Preset, U32 high word first), holding 6-7 = 2, 1 (inlet 2,
   outlet 1), coil 1 = 1. Pass: DO2 and DO4 on; the master's time-left register (ProcessTimer)
   counts down from 3,600,000 (F8). [t_vm_modbus_run_and_link_loss]
3. Link loss (Q1): set vm_link_close_min = 2. Stop the master's polling (unplug the cable). Pass:
   "remote link lost" after comms_timeout (10 s); valves stay on for 2 min, then DO2/DO4 off and
   the event "Valve Manager Modbus run ended". Any MUX run carries on.
4. Schedule: Sequence1 = 2 periods (inlet 1/outlet 1 for 30 s, inlet 3/outlet 2 for 30 s),
   Schedule1 window covering now. Press Scheduler. Pass: routes in order, then the run repeats while
   inside the window (F7 fix: it must not stand still with no valve open); after the window ends,
   the next day's window starts it again. [t_vm_schedule_repeat] In the Sequence 1 editor the Inlet column
   accepts 1..3 only; Save with a period's inlet left at 0 is refused with "Period 1: inlet 0 is not
   fitted (interconnection 1 has 3)". [t_record_checks, t_panel]
5. Two interconnections, Sequence2 of 10 periods. Pass: runs past period 9 (V7 fix).
   [t_vm_v7_long_tube2_sequence]

## 6. Multiplexor
1. Manual: Manual, Yes, Start, toggle inlet 2. Pass: DO6 on (one scan after the VM outputs, F1);
   inlet 4 cannot be toggled with 4 valves; toggle off; Manual: off. Purge toggle: DO8. [t_mux_manual_run]
2. Loop (Q8): SequenceMUX = inlet 1, inlet 2, inlet 1, each dwell 10 s + sample 20 s. Press
   Scheduler. Pass: DO5 for 30 s, DO6 for 30 s, DO5 for 30 s, then again from channel 1 with no
   gap and no stand-still, for at least three loops (F5, S7, S8 fixes). The calendar does not
   matter. [t_mux_schedule_run, t_mux_repeated_inlet]
3. Stop for service: Scheduler off. Pass: all MUX valves off at once.
4. Digital stop: MUX DI5 function = "Stop Seq.", start the loop, close DI5. Pass: run stops at once,
   DIGITAL_STOP shown (no acknowledge needed), valves stay off while DI5 is closed; open DI5: the loop
   starts again from channel 1. [t_mux_digital_stop]
5. Master enable: during a MUX run set `mux_enable` = 0. Pass: MUX valves off, MUX buttons greyed
   and refused, MUX alarms gone, VM unaffected; power-cycle keeps it off; back on: MUX at rest.
   [t_mux_master_enable]

## 7. Modbus map (existing DAHS configurations)
1. Read holding 0-5 (mode, counts), coils 13-34 (VM DI alarms), holding 961-968 (AnalogSensor,
   floats high word first), 969-972 (temperatures, 0.1 degree), 973-1002 (DI 1 description,
   UTF-8). Pass: values as on the touchscreen. [t_modbus]
2. Name VM inlet 1 "ZERO", leave inlet 2 blank, name MUX inlet 2 "Stack B". Pass: holding 20-29 read
   "ZERO", holding 30-39 read "2" (blank = its number, M10 fix), holding 777-786 read "Stack B" (M8 fix).
3. Write holding 0 (read-only). Pass: exception 02. Write a command coil in LOCAL. Pass: 04.
4. Coil 91 = 1. Pass: every alarm acknowledged. Input registers 0-2 and 10-45: alarm counts and
   bits as on the alarm screen.
5. Device identification (FC 43/14). Pass: "CEMS", "CEMS Supervisor Valve Manager / Multiplexor",
   the firmware revision.

## 8. Data log and events
1. Log interval 10 s, AI1 and RTD1 enabled. Pass: CSV from the web page has AI1, AI2, RTD1, RTD2
   columns, real averages, quality OK; N/A for disabled channels; BAD during a sensor fault; FLAG
   during a purge.
2. Event log has: power-ups, every setting change (old and new value, who, from where), mode
   changes, alarms, link loss.

## 9. Screens (touchscreen and web page)
1. Every tab (Valve Mgr, Multiplexor, Sequences, Layout, Process, Inputs, Names, Status) opens on
   the panel and on the web page with the same content. [t_panel, vm_web_ui_test]
2. Names: name VM inlet 1 "ZERO". Pass: the Valve Mgr inlet grid shows ZERO; a blank name shows
   its number.
3. Two interconnections (2 + 1 inlets on tube 2): the grid shows tube 1 from the left and tube 2
   from the right; touch tube 2's inlet 1 (the right-most). Pass: its configured output.
4. Not logged in: values shown, not changeable; commands refused. REMOTE: the panel cannot save
   a sequence ("Not allowed in REMOTE mode"); the web page can.
5. Inputs tab: array settings (enables, limits) are shown, not changeable on the panel yet (R6);
   change them on the web page's settings list.

## 10. Sign-off
List every failure with its step number and send the sheet back; failures become findings in
`INTEGRATION.md` and get a host test before they are fixed.
