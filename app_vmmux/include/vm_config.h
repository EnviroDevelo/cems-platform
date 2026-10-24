/* vm_config.h - compile switches of the VM/MUX application.
 *
 * Rule: the faithful code runs as the original did unless a switch below says
 * otherwise. Every switch names the decision behind it; INTEGRATION.md has the
 * full list (section 4) and the reason for each default. Override with -D. */
#ifndef VM_CONFIG_H
#define VM_CONFIG_H

/* Old email logic (VM Email, 49). APP_INTEGRATION.md section 2: the platform's
 * alarm manager and SMTP client replace it ("Remove the old email logic").
 * 0: Function1 rung 27 does not call Email(); the acknowledge side effects that
 *    mattered (FlowValidationAlarm) are carried by the platform alarm (vm_alarms.c). */
#ifndef VM_OLD_EMAIL
#define VM_OLD_EMAIL 0
#endif

/* Old measurement log (MUX Logging + LoadTable, 48). 70_Alarms_Logging.md section 8:
 * the platform data logger replaces the data table and SD file; it is designed
 * against findings L1-L19, not translated.
 * 0: MUX rung 9 does not call Logging() / LoadTable(); the app logs real readings
 *    through plat_log_put (vm_app.c). */
#ifndef VM_OLD_LOGGING
#define VM_OLD_LOGGING 0
#endif

/* Open item 8 (owner, 18 Oct): the saved project copies never-written TempBoolsDI into
 * DiscreteInputsArray (DI_Assigning rungs 7-8) and has the real inputs in DISABLED rungs
 * 9-10. "The new firmware reads the real inputs DIG_IN_01..20 in the order of the
 * disabled rungs 9-10." 1: rungs 9-10 run (after 7-8, as their order in the original).
 * The other disabled rungs stay off unless ENABLE_DISABLED_RUNGS is defined. */
#ifndef VM_FIX_DI_REAL_INPUTS
#define VM_FIX_DI_REAL_INPUTS 1
#endif

/* Open item 9 (owner accepted 18 Oct): the original RealLinear binding computes
 * raw x (First - Second)/16383 + Second, back to front against the HMI's "Low point" /
 * "High point" labels. "The new firmware scales from Low point to High point."
 * 1: value = raw x (Second - First)/16383 + First. Overflow (status 2) still forces
 * SecondCalibPoint (the high end), as the original. */
#ifndef VM_FIX_AI_SCALING
#define VM_FIX_AI_SCALING 1
#endif

/* Finding F5 (integration, 20 Oct): a MUX sequence that ends inside its start window restarts
 * at once (S3), but its dwell/sample timers were never unpowered, so the restarted run stands
 * with no valve open until the schedule is stopped. Gary (20 Oct): "the MUX should typically be
 * an automated loop", which this breaks. 1: the period timers are reset when a run ends, so a
 * restart begins at period 1 (ScheduleModeMUX rung 14). 0: as the original. */
#ifndef VM_FIX_MUX_RESTART
#define VM_FIX_MUX_RESTART 1
#endif

/* Finding F7 (integration, 21 Oct): the VM version of F5. A VM schedule run that ends inside its
 * window restarts at once (V9) with its period timers still done, so it stands still with the
 * run flag on and never ends: no valve open, and no later window can start it again (a daily
 * calibration would run once only). 1: the period timers are reset when a run ends.
 * 0: as the original. */
#ifndef VM_FIX_VM_RESTART
#define VM_FIX_VM_RESTART 1
#endif

/* Gary (20 Oct): "The MUX has X amount of channels, a time that each of the channels will run
 * for and then this will simply loop until stopped for service." 1: the scheduler mode ignores
 * the calendar window (ScheduleMUX): once started it runs the sequence (channel = period,
 * time = dwell + sample) and starts it again at once, until the Scheduler button, the digital
 * stop input, a pause or the analyser lock stops it. 0: the original daily start window. */
/* MUX findings S7 and S8 (45c): in the original the 20 rows write SingularInletActivate in turn
 * and the last write wins, so an inlet used in two periods is off in the earlier one (S7), and
 * the unused rows (inlet 0) switch the first inlet off every scan, so it can never be scheduled
 * unless all 20 periods are used (S8). With X channels looping (Q8) both matter. 1: the rows of
 * the sequence are ORed per inlet and unused rows are ignored. 0: as the original. */
#ifndef VM_FIX_MUX_VALVE_ASSIGN
#define VM_FIX_MUX_VALVE_ASSIGN 1
#endif

/* VM finding V7 (46d): ScheduleMode rung 11 compares period 9 of interconnection 2 with 8, so a
 * tube-2 sequence of 9 or more periods stalls at period 9 with that route left open until the
 * schedule is stopped. The record check would otherwise have to refuse such sequences.
 * 1: period 9 compares with 9 like every other period. 0: as the original. */
#ifndef VM_FIX_V7
#define VM_FIX_V7 1
#endif

/* 47 M8: the MUX valve-name Modbus registers were never written. 1: they carry the MUX inlet
 * names (vm_compat.c). 0: zeros, as the original. */
#ifndef VM_FIX_MUX_MODBUS_NAMES
#define VM_FIX_MUX_MODBUS_NAMES 1
#endif

/* 47b M10 / M12: a blank VM valve name wrote its number into inlet 1's Modbus name (and left its own
 * buffer with the previous name), and a short name kept the tail of a longer one. 1: each buffer
 * gets its own name, or its number when blank, cleanly. 0: as the original. */
#ifndef VM_FIX_VM_MODBUS_NAMES
#define VM_FIX_VM_MODBUS_NAMES 1
#endif

/* VM finding V21 (46i): Function1 rung 37 clears the four VM selections at the end of every
 * scan. The schedule and Modbus modes rewrite them every scan, but manual mode sets them only
 * on a touch, so a route chosen by hand drives its valves for one scan only: VM manual mode
 * cannot hold a valve open (found again on 21 Oct by the screen test). 1: rung 37 leaves the
 * selections alone while a manual run is on (ManualModeStart); rung 38 (V22) still computes
 * PauseMUX OFF, as the original always did. The run's end (manual timer, Manual button)
 * clears them as before (rung 12). 0: as the original. */
#ifndef VM_FIX_VM_MANUAL_HOLD
#define VM_FIX_VM_MANUAL_HOLD 1
#endif

/* Finding F8 (integration, 21 Oct): ProcessTimer.Preset / ProcessTimer2.Preset, the VM time left
 * shown on the old screen and read by the master (holding registers), are written by the manual
 * (4F4 R10) and Modbus (4G) runs and then, later in the same scan, by the idle schedule step
 * (4F2 step_of: period 1's timer, 0). So they read 0 during manual and Modbus runs.
 * 1: after the logic, they are set from the running mode's own timer (vm_compat.c).
 * 0: as the original. */
#ifndef VM_FIX_TIME_LEFT
#define VM_FIX_TIME_LEFT 1
#endif

#ifndef VM_MUX_CONTINUOUS
#define VM_MUX_CONTINUOUS 1
#endif

/* Which platform output channel (0-based) each valve output drives, VALVE_OUT_01..20 in order.
 * The I/O boards (iobus.h) give 18 channels each: 1-8 bank A (0.6 A, diagnostics), 9-16 bank B
 * (1 A), 17-18 the 230 VAC relays. Default (24 Oct): valves 1-16 on board 1's solid-state
 * outputs (channels 0-15), valves 17-20 on board 2's bank A outputs 1-4 (channels 18-21), so no
 * valve lands on a relay and the relays stay free for the alarm relays. Valves 1-10 were TA30_DO,
 * 11-20 UID_DO. To change it, define VM_VALVE_DO_MAP in the build (20 channel numbers, 0..63). */
#ifndef VM_VALVE_DO_MAP
#define VM_VALVE_DO_MAP { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 18, 19, 20, 21 }
#endif

#endif
