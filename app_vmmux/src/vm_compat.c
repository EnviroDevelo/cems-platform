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

/* UTF-16 text (NUL-terminated within n units) -> UTF-8 bytes, NUL padded to cap. A character
 * that does not fit whole is left out; surrogate pairs are copied as U+FFFD. */
static void utf16_to_utf8(uint8_t *dst, size_t cap, const uint16_t *src, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n && src[i] != 0; i++) {
        uint32_t c = src[i];
        if (c >= 0xD800u && c <= 0xDFFFu) c = 0xFFFDu;
        uint8_t b[3]; size_t k;
        if (c < 0x80u)       { b[0] = (uint8_t)c; k = 1; }
        else if (c < 0x800u) { b[0] = (uint8_t)(0xC0u | (c >> 6)); b[1] = (uint8_t)(0x80u | (c & 0x3Fu)); k = 2; }
        else { b[0] = (uint8_t)(0xE0u | (c >> 12)); b[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
               b[2] = (uint8_t)(0x80u | (c & 0x3Fu)); k = 3; }
        if (o + k > cap) break;
        memcpy(dst + o, b, k); o += k;
    }
    memset(dst + o, 0, cap - o);
}

static void name_to_buf(uint8_t *buf, size_t cap, const char *name)
{
    size_t n = strlen(name); if (n > cap) n = cap;
    memcpy(buf, name, n); memset(buf + n, 0, cap - n);
}

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
    /* The description buffers the master reads (registers 973 on, 49 N2). The old panel copied
     * each text into its buffer when it was entered (TagToBuffer). Q3 (Gary 20 Oct: "forget what
     * UniLogic did, recommend"): text on Modbus is UTF-8, NUL padded, two bytes per register,
     * first byte in the high half (the platform default, as Modbus device identification and
     * most PLC string blocks do). Copied here every scan from the description parameters. */
#if VM_FIX_MUX_MODBUS_NAMES
    /* 47 M8: the MUX valve-name registers (767-950) were never written (ModbusValveNamesMUX is
     * empty), so the master read zeros. Now they carry the MUX inlet names, as the VM ones carry
     * the VM names. ValveNameMUX8 keeps its 8 bytes (the map's irregular entry), so that name is
     * cut to 8 characters there. */
    name_to_buf(vmx_ValveNameMUX1, sizeof vmx_ValveNameMUX1, SingularInlet[0]);
    name_to_buf(vmx_ValveNameMUX2, sizeof vmx_ValveNameMUX2, SingularInlet[1]);
    name_to_buf(vmx_ValveNameMUX3, sizeof vmx_ValveNameMUX3, SingularInlet[2]);
    name_to_buf(vmx_ValveNameMUX4, sizeof vmx_ValveNameMUX4, SingularInlet[3]);
    name_to_buf(vmx_ValveNameMUX5, sizeof vmx_ValveNameMUX5, SingularInlet[4]);
    name_to_buf(vmx_ValveNameMUX6, sizeof vmx_ValveNameMUX6, SingularInlet[5]);
    name_to_buf(vmx_ValveNameMUX7, sizeof vmx_ValveNameMUX7, SingularInlet[6]);
    name_to_buf(vmx_ValveNameMUX8, sizeof vmx_ValveNameMUX8, SingularInlet[7]);
    name_to_buf(vmx_ValveNameMUX9, sizeof vmx_ValveNameMUX9, SingularInlet[8]);
    name_to_buf(vmx_ValveNameMUX10, sizeof vmx_ValveNameMUX10, SingularInlet[9]);
    name_to_buf(vmx_ValveNameMUX11, sizeof vmx_ValveNameMUX11, SingularInlet[10]);
    name_to_buf(vmx_ValveNameMUX12, sizeof vmx_ValveNameMUX12, SingularInlet[11]);
    name_to_buf(vmx_ValveNameMUX13, sizeof vmx_ValveNameMUX13, SingularInlet[12]);
    name_to_buf(vmx_ValveNameMUX14, sizeof vmx_ValveNameMUX14, SingularInlet[13]);
    name_to_buf(vmx_ValveNameMUX15, sizeof vmx_ValveNameMUX15, SingularInlet[14]);
    name_to_buf(vmx_ValveNameMUX16, sizeof vmx_ValveNameMUX16, SingularInlet[15]);
    name_to_buf(vmx_ValveNameMUX17, sizeof vmx_ValveNameMUX17, SingularInlet[16]);
    name_to_buf(vmx_ValveNameMUX18, sizeof vmx_ValveNameMUX18, SingularInlet[17]);
    name_to_buf(vmx_ValveNameMUX19, sizeof vmx_ValveNameMUX19, SingularInlet[18]);
#endif
#if VM_FIX_TIME_LEFT
    /* F8: the time left of the running VM mode (the idle schedule step zeroed it) */
    if (CurrentMode == 1)      { ProcessTimer.Preset = ModbusTimer.Current; ProcessTimer2.Preset = ModbusTimer2.Current; }
    else if (CurrentMode == 3) { ProcessTimer.Preset = ManualTimer.Current; ProcessTimer2.Preset = ManualTimer2.Current; }
#endif
#define DESC(BUF, TXT) utf16_to_utf8(BUF, sizeof BUF, TXT, sizeof TXT / sizeof TXT[0])
#include "vm_compat_desc.inc"
#undef DESC
}
