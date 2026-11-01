/* 4I_faithful.c - rung-by-rung C of VM_Code Email (Stage 4I).
 *
 * Purpose: to pin down exactly what the ORIGINAL UniLogic code does, so it can be tested.
 * Not the code for the new firmware (the new spec has no email; see 49_Email.md).
 * Source: stage0/project_db/ladder/VM_Code__Email.txt (28 rungs, 1, 7, 11, 15, 25-28 empty, none disabled).
 * Conventions as in 4A-4H: blocks pass power on (E4), compare blocks pass power only when true (E5),
 * one edge memory per element updated every scan (E7), parallel branches top first with a
 * "joins wire" tail run once with OR power (11 6.1). Comments "R<n>" give the rung.
 */
#include <string.h>
#include "ul_lf.h"
#include "4I.h"

/* app_vmmux: g_dt_ms and RTC_Time are the platform's (ul_system.h). */

/* ---------------- Send Mail model ----------------
 * Level-sensitive (11d s10: a new send on every scan the rung is powered). A request clears
 * Success and Failed and sets Status 1 (ASSUMPTION A4.21); mail_tick() ("between scans") completes
 * it: Success and Status 0, or Failed and Status -2 (timeout) when g_mail_fail is set. */
mail_status_t Mail[N_MAILS];
int g_sent[N_MAILS];
bool g_mail_fail;
static bool pending_[N_MAILS];
static void send_mail(int m) { Mail[m].Success = Mail[m].Failed = false; Mail[m].Status = 1; pending_[m] = true; g_sent[m]++; }
void mail_tick(void) {
    for (int m = 0; m < N_MAILS; m++) if (pending_[m]) {
        pending_[m] = false;
        if (g_mail_fail) { Mail[m].Failed = true; Mail[m].Status = -2; } else { Mail[m].Success = true; Mail[m].Status = 0; }
    }
}
const char *const MailName[N_MAILS] = {
    "Alarm0_VM", "Alarm1_VM", "Alarm2_VM", "Alarm3_VM", "Alarm4_VM", "Alarm5_VM", "Alarm6_VM", "Alarm7_VM",
    "Alarm8_VM", "Alarm9_VM", "Alarm10_VM", "Alarm11_VM", "Alarm12_VM", "Alarm13_VM", "Alarm14_VM", "Alarm15_VM",
    "Alarm16_VM", "Alarm17_VM", "Alarm18_VM", "Alarm19_VM", "Alarm20_VM", "Alarm21_VM", "Test",
    "Alarm22", "Alarm23", "Alarm24", "Alarm25", "Alarm26", "Alarm27", "Alarm28", "Alarm29",
    "Alarm0_MUX", "Alarm1_MUX", "Alarm2_MUX", "Alarm3_MUX", "Alarm4_MUX", "Alarm5_MUX", "Alarm6_MUX", "Alarm7_MUX",
    "Alarm8_MUX", "Alarm9_MUX", "Alarm10_MUX", "Alarm11_MUX", "Alarm12_MUX", "Alarm13_MUX", "Alarm14_MUX", "Alarm15_MUX",
    "Alarm16_MUX", "Alarm17_MUX", "Alarm18_MUX", "Alarm19_MUX", "Alarm20_MUX", "Alarm21_MUX",
    "FBAlarm0", "FBAlarm1", "FBAlarm2", "FBAlarm3", "FBAlarm4", "FBAlarm5", "FBAlarm6", "FBAlarm7", "FBAlarm8", "FBAlarm9",
    "FBAlarm10", "FBAlarm11", "FBAlarm12", "FBAlarm13", "FBAlarm14", "FBAlarm15", "FBAlarm16", "FBAlarm17", "FBAlarm18", "FBAlarm19",
    "Flow validation alarm" };

/* ---------------- tags: defined once in src/vm_tags.c (app_vmmux) ---------------- */

/* ---------------- element models ---------------- */
static void ton(ul_timer_t *t, bool in) {                                             /* TimerOnDelay, A4.10 */
    if (!in) { t->Current = t->Preset; t->Out = false; t->On = false; return; }
    if (!t->On) { t->Current = t->Preset; t->On = true; }
    if (!t->Out) { if (t->Current > g_dt_ms) t->Current -= g_dt_ms; else { t->Current = 0; t->Out = true; } }
}
/* treset (TimerReset): one definition in src/ul_blocks.c (app_vmmux) */
static bool pcont(bool *m, bool b) { bool e = b && !*m; *m = b; return e; }        /* E7 */

static bool m_vm[22], m_mux[22], m_fb[20], m_flow, m_ac[4], m_tc[4];
static bool m_fvm[22], m_fmux[22], m_ffb[20], m_fflow;      /* Failed edges (index 0 of VM and MUX unused, see R18/R19) */

void h4i_power_up(void)
{
    memset(Mail, 0, sizeof Mail); memset(pending_, 0, sizeof pending_);
    memset(m_vm, 0, sizeof m_vm); memset(m_mux, 0, sizeof m_mux); memset(m_fb, 0, sizeof m_fb); m_flow = false;
    memset(m_ac, 0, sizeof m_ac); memset(m_tc, 0, sizeof m_tc);
    memset(m_fvm, 0, sizeof m_fvm); memset(m_fmux, 0, sizeof m_fmux); memset(m_ffb, 0, sizeof m_ffb); m_fflow = false;
    Send_Mail = false; strcpy(HourString1, "4"); strcpy(MinuteString1, "40"); HourStatus1 = MinuteStatus1 = 0;
    memset(AlarmsVM, 0, sizeof AlarmsVM); memset(AlarmsMUX, 0, sizeof AlarmsMUX); memset(FBAlarmsMUX, 0, sizeof FBAlarmsMUX);
    memset(AnalogSensor, 0, sizeof AnalogSensor); memset(RangeMax, 0, sizeof RangeMax); memset(RangeMin, 0, sizeof RangeMin);
    memset(TemperatureSensor, 0, sizeof TemperatureSensor); memset(MaxTemperatureRange, 0, sizeof MaxTemperatureRange);
    memset(MinTemperatureRange, 0, sizeof MinTemperatureRange);
    for (int k = 0; k < 4; k++) { AnalogTimer[k].Preset = TempTimer[k].Preset = 10000; treset(&AnalogTimer[k]); treset(&TempTimer[k]); }
    memset(AnalogSensorAlarmBit, 0, 4); memset(TempSensorAlarmBit, 0, 4);
    memset(AnalogAlarmEmailEnabled, 0, 4); memset(TemperatureAlarmEmailEnabled, 0, 4);       /* RAM in the project */
    EmailCantbeSent = AcknowledgeBit = AcknEmailSentError = FlowValidationAlarm = false; MailStatus = MailStatusCode = 0;
}

void Email(void)
{
    int k; bool p, e;
    /* R2: the Send Mail button on EmailSettings (not gated by ModemIncudeOrNot) */
    if (Send_Mail) { send_mail(MAIL_TEST); Send_Mail = false; }
    /* R3: time strings used in every email body */
    if (ModemIncudeOrNot) {
        Num_to_ASCII(RTC_Time.Hours, 3, 0, 2, 0, HourString1, &HourStatus1);
        Num_to_ASCII(RTC_Time.Minutes, 3, 0, 2, 0, MinuteString1, &MinuteStatus1);
        lf_remove_from_string(HourString1, " ", 0, 1, HourString1, 4);
        lf_remove_from_string(MinuteString1, " ", 0, 1, MinuteString1, 4);
    }
    /* R4-R6: VM discrete alarms 0..21 (rising edge, per-alarm e-mail enable) */
    for (k = 0; k < 22; k++) { e = pcont(&m_vm[k], AlarmsVM[k]); if (ModemIncudeOrNot && e && EmailEnableVM[k]) send_mail(MAIL_VM0 + k); }
    /* R8-R10: MUX discrete alarms 0..21 */
    for (k = 0; k < 22; k++) { e = pcont(&m_mux[k], AlarmsMUX[k]); if (ModemIncudeOrNot && e && EmailEnableMUX[k]) send_mail(MAIL_MUX0 + k); }
    /* R12-R14: MUX valve feedback alarms 0..19 */
    for (k = 0; k < 20; k++) { e = pcont(&m_fb[k], FBAlarmsMUX[k]); if (ModemIncudeOrNot && e && FBEmailEnableMUX[k]) send_mail(MAIL_FB0 + k); }
    /* R16: out-of-range timers (10 s) and their one-scan alarm pulses, not gated by ModemIncudeOrNot */
    for (k = 0; k < 4; k++) {
        ton(&AnalogTimer[k], AnalogSensor[k] > RangeMax[k] || AnalogSensor[k] < RangeMin[k]);
        p = AnalogTimer[k].Out; AnalogSensorAlarmBit[k] = p && !m_ac[k]; m_ac[k] = p;          /* positive transition coil */
    }
    for (k = 0; k < 4; k++) {
        ton(&TempTimer[k], TemperatureSensor[k] > MaxTemperatureRange[k] || TemperatureSensor[k] < MinTemperatureRange[k]);
        p = TempTimer[k].Out; TempSensorAlarmBit[k] = p && !m_tc[k]; m_tc[k] = p;
    }
    /* R17 */
    e = pcont(&m_flow, FlowValidationTimer.Out);
    if (ModemIncudeOrNot) {
        for (k = 0; k < 4; k++) if (AnalogSensorAlarmBit[k] && AnalogAlarmEmailEnabled[k]) send_mail(MAIL_ANALOG0 + k);
        for (k = 0; k < 4; k++) if (TempSensorAlarmBit[k] && TemperatureAlarmEmailEnabled[k]) send_mail(MAIL_TEMP0 + k);
        if (e) send_mail(MAIL_FLOW);                     /* no enable bit */
    }
    /* R18: a rising Failed of VM alarm mails 1..21 or the flow mail (not Alarm0_VM, Alarm22-29, Test) */
    p = false;
    for (k = 1; k <= 21; k++) if (pcont(&m_fvm[k], Mail[MAIL_VM0 + k].Failed)) p = true;
    if (pcont(&m_fflow, Mail[MAIL_FLOW].Failed)) p = true;
    if (p) EmailCantbeSent = true;
    /* R19: MUX alarm mails 1..21 and feedback mails 0..19 (not Alarm0_MUX) */
    p = false;
    for (k = 1; k <= 21; k++) if (pcont(&m_fmux[k], Mail[MAIL_MUX0 + k].Failed)) p = true;
    for (k = 0; k < 20; k++) if (pcont(&m_ffb[k], Mail[MAIL_FB0 + k].Failed)) p = true;
    if (p) EmailCantbeSent = true;
    /* R20: "Acknowledge Bit" (Modbus coil 91): clear the three alarm arrays, restart the 8 range timers,
     * clear FlowValidationAlarm, then the bit itself */
    if (AcknowledgeBit) {
        memset(AlarmsVM, 0, sizeof AlarmsVM); memset(AlarmsMUX, 0, sizeof AlarmsMUX); memset(FBAlarmsMUX, 0, sizeof FBAlarmsMUX);
        for (k = 0; k < 4; k++) treset(&AnalogTimer[k]);
        for (k = 0; k < 4; k++) treset(&TempTimer[k]);
        FlowValidationAlarm = false;
        AcknowledgeBit = false;
    }
    /* R21 */
    if (AcknEmailSentError) { EmailCantbeSent = false; AcknEmailSentError = false; }
    /* R22-R24: test mail status (MailStatus is last scan's value in R22) */
    if (!Mail[MAIL_TEST].Success && !Mail[MAIL_TEST].Failed && MailStatus == 1) MailStatusCode = 1;
    if (Mail[MAIL_TEST].Failed) MailStatusCode = 3;
    MailStatus = Mail[MAIL_TEST].Status;
}
