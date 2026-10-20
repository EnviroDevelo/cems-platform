/* 4I.h - Stage 4I email: VM_Code Email (28 rungs). Links with ../4A/4A_faithful.c (Num to ASCII).
 * The 74 email definitions are listed in email_definitions.csv (database order = mail index here). */
#ifndef STAGE4I_H
#define STAGE4I_H
#include "vm_types.h"   /* shared types and system tags (app_vmmux) */
#include "4A.h"

extern uint32_t g_dt_ms;
extern rtc_time_t RTC_Time;

/* ---- Send Mail model (11d section 10). Each definition has its auto-created struct. ---- */
#define N_MAILS 74
enum { MAIL_VM0 = 0, MAIL_TEST = 22, MAIL_ANALOG0 = 23, MAIL_TEMP0 = 27, MAIL_MUX0 = 31, MAIL_FB0 = 53, MAIL_FLOW = 73 };
typedef struct { bool Success, Failed; int16_t Status; } mail_status_t;
extern mail_status_t Mail[N_MAILS];
extern const char *const MailName[N_MAILS];
extern int g_sent[N_MAILS];          /* test support: requests per definition */
extern bool g_mail_fail;             /* test support: the next completions fail (status -2) */
void mail_tick(void);                /* between scans: every queued send completes */

/* ---- tags ---- */
extern bool Send_Mail, ModemIncudeOrNot;
extern char HourString1[5], MinuteString1[5]; extern int16_t HourStatus1, MinuteStatus1;
extern bool AlarmsVM[22], AlarmsMUX[22], FBAlarmsMUX[20], EmailEnableVM[22], EmailEnableMUX[22], FBEmailEnableMUX[20];
extern float AnalogSensor[4], RangeMax[4], RangeMin[4];
extern int16_t TemperatureSensor[4], MaxTemperatureRange[4], MinTemperatureRange[4];
extern ul_timer_t AnalogTimer[4], TempTimer[4], FlowValidationTimer;   /* AnalogTimer0..3, TempTimer0..3 */
extern bool AnalogSensorAlarmBit[4], TempSensorAlarmBit[4], AnalogAlarmEmailEnabled[4], TemperatureAlarmEmailEnabled[4];
extern bool EmailCantbeSent, AcknowledgeBit, AcknEmailSentError, FlowValidationAlarm;   /* AcknowledgeBit = tag "Acknowledge Bit" */
extern int16_t MailStatus, MailStatusCode;                                              /* "Mail Status", "Mail Status Code" */

void Email(void);
void treset(ul_timer_t *t);
void h4i_power_up(void);             /* test support: RAM tags and edge memories to power-up values */
#endif
