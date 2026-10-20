/* vm_compat.c - keeps the old Modbus coils and buffers meaningful where the platform has
 * taken over the code that used to drive them (VM_OLD_EMAIL = 0, VM_OLD_LOGGING = 0).
 * Runs once per scan after Function1 and the alarm producers. Each item names the old
 * rung it stands in for. */
#include <string.h>
#include "vm_app.h"
#include "vm_tags.h"
#include "vm_params.h"
#include "vm_alarms.h"
#include "plat_alarm.h"

#if !VM_OLD_EMAIL
/* Email rung 20: coil 91 "Acknowledge Bit" acknowledged every alarm (and cleared the source
 * arrays, which DI_Control rebuilds every scan anyway). Now: acknowledge all platform alarms. */
static void ack_all_coil(void)
{
    if (!AcknowledgeBit) return;
    alarm_ack(ALARM_ACK_ALL, PLAT_SRC_MODBUS, PLAT_LVL_OPERATOR);
    AcknowledgeBit = false;
}
#endif

void vm_compat_scan(void)
{
#if !VM_OLD_EMAIL
    ack_all_coil();
    /* Email rung 21: coil 90 cleared EmailCantbeSent. The flag now mirrors the platform's
     * NOTIFY_FAIL alarm (coil 85); the coil is accepted and cleared. */
    EmailCantbeSent = alarm_active(ALARM_ID_NOTIFY_FAIL);
    AcknEmailSentError = false;
    /* Email rung 16: the 10 s out-of-range timers whose .Out the master reads on coils 77-84.
     * Now the platform's range alarms (same 10 s for the analog inputs). Channels 3-4 have no
     * hardware (K10) and stay 0. */
    AnalogTimer[0].Out = alarm_active(VM_AL_AI1_RANGE);
    AnalogTimer[1].Out = alarm_active(VM_AL_AI2_RANGE);
    TempTimer[0].Out = alarm_active(VM_AL_RTD1_RANGE);
    TempTimer[1].Out = alarm_active(VM_AL_RTD2_RANGE);
#endif
#if !VM_OLD_LOGGING
    /* Logging rung 27 / LoadTable rung 6 consumed coils 96 and 98; the old log is gone and its
     * faults are the platform's LOG_STORAGE_FAULT alarm. The coils are accepted and cleared. */
    AcknowledgeCSVReserv = false;
    AckowledgeLoadError = false;
#endif
    /* The description buffers the master reads (registers 973 on, 49 N2): the old panel copied
     * each text into its buffer when it was entered (TagToBuffer, modelled only as a call count
     * in Stage 4C). Copied here every scan from the description parameters: the UTF-16 code
     * units in memory order (little-endian, as the old ARM panel held them), the first 30
     * characters (60 bytes). ASSUMPTION: TagToBuffer of a UTF-16 string copies its code units
     * this way; the register byte order on top of it is the open BUFFER question. */
#define DESC(BUF, TXT) memcpy(BUF, TXT, sizeof BUF < sizeof TXT ? sizeof BUF : sizeof TXT)
#include "vm_compat_desc.inc"
#undef DESC
}
