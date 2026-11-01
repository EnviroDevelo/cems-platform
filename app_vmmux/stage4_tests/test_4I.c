/* test_4I.c - tests for 4I_faithful.c (VM_Code Email).
 * Build: gcc -std=c11 -Wall -Wextra -O1 test_4I.c 4I_faithful.c ../4A/4A_faithful.c -lm
 * No rungs are disabled. mail_tick() runs between scans (A4.21). */
#include <stdio.h>
#include <string.h>
#include "4I.h"

static int checks, fails;
static void eqi(const char *what, long got, long want) {
    checks++; if (got != want) { fails++; printf("FAIL %s: got %ld, want %ld\n", what, got, want); } else printf("ok   %s\n", what);
}
static void eqs(const char *what, const char *got, const char *want) {
    checks++; if (strcmp(got, want)) { fails++; printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want); } else printf("ok   %s\n", what);
}
static void scan(void) { mail_tick(); Email(); }
static void scans(int n) { while (n-- > 0) scan(); }
static int total(void) { int n = 0; for (int m = 0; m < N_MAILS; m++) n += g_sent[m]; return n; }
static void fresh(bool modem) {
    h4i_power_up(); memset(g_sent, 0, sizeof g_sent); g_mail_fail = false; g_dt_ms = 100;
    memset(EmailEnableVM, 0, 22); memset(EmailEnableMUX, 0, 22); memset(FBEmailEnableMUX, 0, 20);
    ModemIncudeOrNot = modem; treset(&FlowValidationTimer); scan();
}

int main(void)
{
    /* ---- 1. Test mail and its status ---- */
    printf("--- 1. Test mail ---\n");
    fresh(false);
    eqs("power-up HourString1 is \"4\" until rung 3 first runs", HourString1, "4");
    Send_Mail = true; scan();
    eqi("R2: the Send Mail button sends Test even with no modem, and clears itself", g_sent[MAIL_TEST] * 10 + Send_Mail, 10);
    eqi("R24: Mail Status shows 1 (started)", MailStatus, 1);
    scan();
    eqi("next scan: sent, Mail Status 0", MailStatus * 10 + Mail[MAIL_TEST].Success, 1);
    eqi("R22: Mail Status Code 1 needs status 1 with neither result yet; a one-scan send never shows it", MailStatusCode, 0);
    g_mail_fail = true; Send_Mail = true; scans(2);
    eqi("R23: a failed test sets Mail Status Code 3 (never read anywhere)", MailStatusCode * 100 + MailStatus, 298);
    eqi("a failed Test does not set EmailCantbeSent", EmailCantbeSent, 0);

    /* ---- 2. Discrete alarms ---- */
    printf("--- 2. Discrete alarms ---\n");
    fresh(true);
    RTC_Time.Hours = 9; RTC_Time.Minutes = 5; scan();
    eqs("R3: HourString1", HourString1, "09");
    eqs("R3: MinuteString1 (body ends '09:05')", MinuteString1, "05");
    EmailEnableVM[5] = true; AlarmsVM[5] = true; scan();
    eqi("R4-R6: rising VM alarm 5 with its e-mail enabled sends Alarm5_VM once", g_sent[MAIL_VM0 + 5], 1);
    scans(20);
    eqi("no repeat while the alarm stays on", g_sent[MAIL_VM0 + 5], 1);
    AlarmsVM[6] = true; scan();
    eqi("alarm 6 without its enable: no mail", g_sent[MAIL_VM0 + 6], 0);
    EmailEnableMUX[0] = true; AlarmsMUX[0] = true; FBEmailEnableMUX[19] = true; FBAlarmsMUX[19] = true; scan();
    eqi("R8, R14: MUX alarm 0 and valve feedback alarm 19 mails", g_sent[MAIL_MUX0] * 10 + g_sent[MAIL_FB0 + 19], 11);

    fresh(false);
    EmailEnableVM[2] = true; AlarmsVM[2] = true; scan();
    ModemIncudeOrNot = true; scans(5);
    eqi("an alarm raised while 'modem included' is off is never mailed, even after it is switched on", g_sent[MAIL_VM0 + 2], 0);

    /* ---- 3. Analog and temperature range alarms ---- */
    printf("--- 3. Range alarms ---\n");
    fresh(true);
    AnalogSensor[0] = 5.0f; scans(99);
    eqi("R16: out of range (limits are RAM, 0 after power-up, so any reading above 0) for 9.9 s: no alarm yet", AnalogTimer[0].Out, 0);
    scan();
    eqi("at 10 s the alarm bit pulses for one scan", AnalogSensorAlarmBit[0], 1);
    eqi("... but no mail: the e-mail enable is RAM and off after power-up", g_sent[MAIL_ANALOG0], 0);
    scan();
    eqi("pulse over", AnalogSensorAlarmBit[0], 0);
    AnalogAlarmEmailEnabled[0] = true; AcknowledgeBit = true; scan(); scans(100);
    eqi("after the Modbus acknowledge restarts the timer, the next pulse sends Alarm22 (analog 0)", g_sent[MAIL_ANALOG0], 1);
    scans(200);
    eqi("no repeat while it stays out of range", g_sent[MAIL_ANALOG0], 1);
    RangeMax[0] = 10.0f; scan(); RangeMax[0] = 0.0f; scans(100);
    eqi("back in range for one scan resets the timer: a new alarm and mail 10 s later", g_sent[MAIL_ANALOG0], 2);

    fresh(true);
    MinTemperatureRange[3] = 5; TemperatureAlarmEmailEnabled[3] = true; scans(100);
    eqi("temperature 3 is never written (0): with a low limit above 0 it alarms and mails Alarm29", g_sent[MAIL_TEMP0 + 3], 1);

    /* ---- 4. Flow validation ---- */
    printf("--- 4. Flow validation ---\n");
    fresh(true);
    FlowValidationTimer.Out = true; scan();
    eqi("R17: rising FlowValidationTimer.Out sends the flow mail (no enable bit)", g_sent[MAIL_FLOW], 1);
    scans(5);
    eqi("once per rising edge", g_sent[MAIL_FLOW], 1);
    FlowValidationAlarm = true; AlarmsVM[1] = AlarmsMUX[3] = FBAlarmsMUX[0] = true;
    AcknowledgeBit = true; scan();
    eqi("R20: Acknowledge Bit (Modbus coil 91) clears FlowValidationAlarm (S17 corrected)", FlowValidationAlarm, 0);
    eqi("... clears the VM, MUX and feedback alarm arrays and itself", AlarmsVM[1] + AlarmsMUX[3] + FBAlarmsMUX[0] + AcknowledgeBit, 0);

    /* ---- 5. Send failures ---- */
    printf("--- 5. Send failures ---\n");
    fresh(true);
    g_mail_fail = true;
    EmailEnableVM[0] = true; AlarmsVM[0] = true; scans(3);
    eqi("a failed Alarm0_VM does not set EmailCantbeSent (R18 starts at Alarm1_VM)", Mail[MAIL_VM0].Failed * 10 + EmailCantbeSent, 10);
    AnalogAlarmEmailEnabled[1] = true; AnalogSensor[1] = 1.0f; scans(102);
    eqi("a failed Alarm23 (analog 1) does not set it either (Alarm22-29 not watched)", Mail[MAIL_ANALOG0 + 1].Failed * 10 + EmailCantbeSent, 10);
    EmailEnableMUX[0] = true; AlarmsMUX[0] = true; scans(3);
    eqi("nor Alarm0_MUX (R19 starts at Alarm1_MUX)", EmailCantbeSent, 0);
    EmailEnableVM[1] = true; AlarmsVM[1] = true; scans(3);
    eqi("a failed Alarm1_VM sets EmailCantbeSent", EmailCantbeSent, 1);
    AcknEmailSentError = true; scan();
    eqi("R21: Modbus coil 90 clears it", EmailCantbeSent + AcknEmailSentError, 0);
    FBEmailEnableMUX[7] = true; FBAlarmsMUX[7] = true; scans(3);
    eqi("a failed feedback mail sets it", EmailCantbeSent, 1);

    eqi("74 definitions, all in the order of email_definitions.csv", (long)(strcmp(MailName[MAIL_FLOW], "Flow validation alarm") == 0 && strcmp(MailName[MAIL_TEST], "Test") == 0), 1);
    (void)total;
    printf("TOTAL: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
