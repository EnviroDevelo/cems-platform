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

#endif
