# VM/MUX application screens: what the core platform's application-screen API must carry

Rev A, 20 Oct 2026. For the core-platform thread (APP_INTEGRATION.md section 9, request R2).
Source: `Existing Design/Analysis/50_HMI_Map.md` ("Map to the new 1024x600 EVE screen set") and
`stage5/hmi_elements.csv`. Every value below is already a parameter-table entry, so the same
screen can be drawn by the touchscreen and the web page from IDs alone.

## 1. What the application provides today

| Kind | IDs | Where |
|---|---|---|
| Settings (persisted) | 1000-1148 | `include/vm_params.h` (`VMP_*`), generated |
| New settings | 2900-2905 | `src/vm_params_new.inc` |
| Retained blocks (sequences, schedules, names ...) | 3000-3194 | `include/vm_params.h`, not parameters |
| Modbus-visible values (read only, or master commands) | 4000-4194 | `include/vm_params_modbus.h` (`VMMB_*`) |
| **Screen commands** (one write = one press of the old button) | **5000-5019** | `include/vm_hmi.h`, `src/vm_hmi.c` |
| **Screen status** (button states, popups, enables) | **5100-5133** | `include/vm_hmi.h` |
| Alarms | 100-173 | `include/vm_alarms.h` (the platform's alarm screens show them) |

A command is checked against the old button's enable/visibility when it is applied
(`PERR_INTERLOCK` if the button was disabled or hidden). Commands that start valve movement carry
`PF_CONFIRM`. Settings written from any source set the old "...Entered" bits as if typed on the
panel (`vm_hmi.c`, entry emulation), so the screen needs no special handling for them.

## 2. Screens (from 50_HMI_Map, merged VM + MUX)

### 2.1 Home (replaces the four old main screens)
Valve Manager side:
- Mode buttons: `vm_btn_modbus` 5000, `vm_btn_schedule` 5001, `vm_btn_manual` 5002; their state
  5100-5102 (0 = off, 1 = on) and enables 5103-5105.
- Manual-mode dialog: shown while `vm_manual_popup` 5106; Yes 5003, No 5004. Then a Start button,
  shown while `vm_start_visible` 5107, command 5005.
- Pipe picture: inlets 1..`FirstInlets`, outlets 1..`FirstOutlets` (tube 1) and the second tube when
  `NumInterconnections` = 2. Valve touch: `vm_inlet_touch` 5006 / `vm_outlet_touch` 5007 with the
  valve number 1..19; touchable while `vm_inlet_touch_en[i]` 5110 / `vm_outlet_touch_en[i]` 5111.
  Valve state colours: `TubeColor1` / `TubeColor2` (block 3000+ region; to be exposed as read-only
  parameters when the API needs them).
- Remaining time: `VMMB_PROCESSTIMER_PRESET` 4045 (ms), `VMMB_PROCESSTIMER2_PRESET` 4046.
- Dialogs "sequences absent" 5108 (OK 5008) and "incorrect sequences" 5109 (OK 5009).

Multiplexor side:
- Mode buttons 5010-5012, states 5120-5122, enables 5123-5125.
- Manual-mode dialog `mux_manual_popup` 5126: Yes 5013, No 5014; Start (visible 5127) 5015.
- Inlet list: inlet *i* shown while `mux_inlet_visible[i]` 5131, open while `mux_inlet_on[i]`
  5132; toggle with `mux_inlet_toggle` 5016 (value = inlet 1..19) while `mux_inlet_select_en` 5128.
  Purge: state 5133, toggle 5017. Names: `SingularInlet0..18` (parameters 1122+).
- Remaining time: `VMMB_PROCESSTIMERMUX_PRESET` 4124; dialogs 5129 / 5130 with OK 5018 / 5019.

Both: readings `VMMB_ANALOGSENSOR` 4145 (4 floats), `VMMB_TEMPERATURESENSOR` 4146 (4 values,
0.1 degree), local/remote and alarm indicators (platform).

### 2.2 Valve layout (System Config / System Config_MUX, counts part)
`VMP_MAXNUMBEROFVALVES` 1091, `VMP_NUMINTERCONNECTIONS` 1093, `VMP_FIRSTINLETS` 1064,
`VMP_FIRSTOUTLETS` 1065, `VMP_SECONDINLETS` 1120, `VMP_SECONDOUTLETS` 1121,
`VMP_NUMBEROFMUXVALVES` 1094, `VMP_BACKFLUSHEXISTS` 1051. Limits and the tag-dependent maxima are
enforced by the table (interlocks). 0 means "not configured yet" (the original's power-up value).

### 2.3 Valve names
`InletName0..18`, `OutletName0..18` (VM, 10 characters), `SingularInlet0..18` (MUX, 10 characters),
parameters 1069+ / 1095+ / 1122+ (`VMP_INLETNAME0` ...). Operator level (the old panel had no password for them, U8).

### 2.4 Output mapping (old DO-mapping screens and MUX_DOMapping)
`VMP_DOINLET` 1053 and `VMP_DOOUTLET` 1054 (0-based DO per valve position: inlets first, then
outlets, 43c), `VMP_DOSINGULARINLET` 1056 and `VMP_DOPURGEINLET` 1055 (1-based DO), back-flush
`VMP_BACKFLUSHDONUMBER` 1050. The old MUX mapping screen's toggles could open valves in any mode
(U3); the new screen should not offer them.

### 2.5 Process
`VMP_DEFAULTMODEONPOWERUP` 1057, manual timers `VMP_MANUAL_TIMER` 1088 / `..._TIMER2` 1089 /
`..._TIMERMUX` 1090 (ms), flow validation `VMP_FLOWVALIDATION` 1067, `VMP_FLOWANALOGINPUT` 1066,
`VMP_FLOWVALIDATIONTIMER` 1068, `VMP_ENABLELOCKVALVESIFANALYZERNOTOK` 1061, log interval (platform
parameter 30, U2), `vm_link_close_min` 2901.

### 2.6 Inputs
- Discrete (VM and MUX, 22 each): `VMP_DI_FUNCTIONVM` 1058 / `VMP_DI_FUNCTIONMUX` 1052 (combo,
  item lists in `stage5/screens/08..24`), `VMP_ALARMENABLEVM/MUX`, `VMP_ALARMCONDITIONVM/MUX`,
  `VMP_FEEDBACKCONDITIONMUX`, descriptions `Alarm<i> Description[MUX]` (UTF-16, 30 characters).
- Analog: `VMP_ANALOGENABLE` 1142, `VMP_FIRSTCALIBPOINT` 1063 / `VMP_SECONDCALIBPOINT` 1119
  (factory class), `VMP_RANGEMIN/MAX` 1146/1145, `VMP_ANALOGALARMENABLED` 1141.
- Temperature: `VMP_TEMPERATUREENABLE` 1148, `TemperatureShift` (to be exposed),
  `VMP_MIN/MAXTEMPERATURERANGE` 1144/1143, `VMP_TEMPERATUREALARMENABLED` 1147,
  `rtd1/2_alarm_delay_s` 2902/2903, `rtd1/2_alarm_hyst` 2904/2905.

### 2.7 Schedule and sequence editors (not built yet)
The old editors work on copies (`ScheduleHMI`, `HMISequences`, `HMISequenceMUX`) through 7 visible
rows, scroll bits and Save bits (45e, 46f). They need block writes of a whole sequence or schedule,
which is the next application step (it depends on how the API passes structured data). Proposed:
one block parameter per sequence (`Sequence1`, `Sequence2`, `SequenceMUX`) and per schedule
(`Schedule1`, `Schedule2`, `ScheduleMUX`), validated by an application check before it is applied,
instead of the old row-by-row editor bits.

### 2.8 Not carried over
Settings/password popups (platform login), email settings (platform mail), the Modbus client test
screen (dropped), the Log screen (platform data log + CSV download), screen saver and brightness
(platform UI).
