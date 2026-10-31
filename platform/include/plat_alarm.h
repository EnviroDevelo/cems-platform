/* plat_alarm.h - the alarm manager (design spec "Alarms"; Analysis 70_Alarms_Logging
 * section 8, findings K1-K12).
 *
 * One table of alarm definitions and one state per alarm. Producers only report a
 * condition; the manager owns the delays, the latch, the acknowledge and the state,
 * so the old program's two layers (source bits plus the panel's own alarm table,
 * whose texts drifted onto the wrong alarms: K1, K2) become one.
 *
 * IDs: 1..99 platform (ALARM_ID_* below), 100..255 application.
 *
 * States (ISA-18.2 style):
 *   normal            condition absent, nothing to acknowledge
 *   active, unacked   condition present (after the on-delay), not yet acknowledged
 *   active, acked     condition present, acknowledged
 *   cleared, unacked  condition gone (after the off-delay) before anyone acknowledged:
 *                     stays in the list until acknowledged, so a short alarm is not missed
 * AL_LATCH: an active alarm stays active after its condition goes until acknowledged
 *   (the old FlowValidationAlarm). AL_NO_ACK: a status alarm, shown while active,
 *   never waits for an acknowledge (digital stop, analyser not OK: K8).
 * Acknowledge never changes a condition, never clears a source and never re-sends
 * anything (K7, N3): it only moves the alarm's state. One alarm by ID, or all.
 *
 * Every raise, clear and acknowledge goes to the event log with the source and login
 * level (K11, K12). Optional per-alarm parameters: an enable (false suppresses the
 * alarm, e.g. a range alarm switched off; retained, so it survives a power cut: K4),
 * an editable description (stored with the settings, so it cannot land on another
 * alarm: K1, K2) and an on-delay in seconds (valve feedback settling time: N11).
 *
 * Threading
 *   alarm_set(), alarm_set_range()   any task (a byte store); normally the scan logic
 *   alarm_scan()                     scan task, once per scan (plat_sys pre_logic)
 *   alarm_ack()                      scan task (param_apply calls it for PARAM_ID_ALARM_ACK);
 *                                    other tasks post that parameter instead
 *   alarm_list(), alarm_summary(),
 *   alarm_get()                      any task: a consistent copy
 */
#ifndef PLAT_ALARM_H
#define PLAT_ALARM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "plat_io.h"

#ifndef PLAT_ALARM_MAX
#define PLAT_ALARM_MAX 128u          /* platform (incl. the 24 digital input alarms) + application definitions */
#endif
#define ALARM_ID_MAX      255u
#define ALARM_ID_APP_FIRST 100u
#define ALARM_ACK_ALL     0xFFFFu    /* PARAM_ID_ALARM_ACK value: acknowledge every alarm */
#define ALARM_BIT_WORDS   16u        /* PARAM_ID_ALARM_*_BITS: bit (id % 16) of word (id / 16) */

/* severities: the colour in the lists and the header */
enum { AL_SEV_STATUS = 0, AL_SEV_WARNING = 1, AL_SEV_ALARM = 2, AL_SEV_FAULT = 3 };

/* Priority tiers (Gary 31 Oct: assigned per alarm, each application configures its own;
 * ISA-18.2 / EEMUA 191 style). Parameter 52 holds one byte per alarm ID: AL_PRIO_DEFAULT =
 * the definition's tier (from its severity), 1..5 = that tier, AL_PRIO_OFF = switched off (not
 * for AL_STOPS or AL_NO_OFF alarms: they say why measurement stopped or can't be trusted). The tier sets the order in the lists,
 * the colour (severity: Critical and High red, Medium alarm, Low warning, Info status), the
 * per-tier counts (54, 55) and, for an alarm whose own actions are not set (46), the tier's
 * actions (53). Tier names are editable (90-94). */
enum { AL_PRIO_DEFAULT = 0, AL_PRIO_CRITICAL = 1, AL_PRIO_HIGH = 2, AL_PRIO_MEDIUM = 3, AL_PRIO_LOW = 4,
       AL_PRIO_INFO = 5, AL_PRIO_OFF = 6 };
#define AL_TIERS       5u
#define AL_TIER_NAME   17u       /* bytes, with the NUL */

/* Actions (routes), per alarm. Each alarm has a default in its definition; the user
 * changes it with parameter 46 (one byte per alarm ID): bit 7 set = configured (bits
 * 0..6 apply), bit 7 clear = use the definition's default. 0x80 = do nothing. */
#define AR_EMAIL    0x01u   /* send an email (raise; clear too if mail_on_clear) */
#define AR_MODBUS   0x02u   /* counted in the Modbus common-alarm word (parameter 45) */
#define AR_RELAY(k) ((uint8_t)(0x04u << (k)))   /* drive alarm relay k (0..3) */
#define AR_SET      0x80u
#define ALARM_RELAYS 4u
/* relay modes (parameter 48) */
enum { AR_MODE_ACTIVE = 0,      /* on while any routed alarm is active */
       AR_MODE_UNACKED = 1,     /* on while any routed alarm waits for an acknowledge (a horn: ack silences it) */
       AR_MODE_ACTIVE_OR_UNACKED = 2 };  /* on until every routed alarm is both cleared and acknowledged (a lamp) */
/* parameter 45 bits */
#define ACOM_MB_ACTIVE   0x0001u  /* a Modbus-routed alarm is active */
#define ACOM_MB_UNACKED  0x0002u  /* a Modbus-routed alarm waits for an acknowledge */
#define ACOM_ANY_ACTIVE  0x0004u
#define ACOM_ANY_UNACKED 0x0008u
#define ACOM_FORCED      0x0010u  /* values are forced on Modbus (commissioning, modbus.h mb_force_*) */
#define ACOM_CRIT_ACTIVE 0x0020u  /* a Critical-tier alarm is active */
#define ACOM_CRIT_UNACKED 0x0040u /* a Critical-tier alarm waits for an acknowledge */
#define ACOM_RELAY(k)    ((uint16_t)(0x0100u << (k)))   /* relay k is in alarm (before inversion) */

/* flags */
#define AL_LATCH   0x01u   /* stays active until acknowledged, even if the condition goes */
#define AL_NO_ACK  0x02u   /* status alarm: no acknowledge, leaves the list when it clears */
#define AL_NOTIFY  0x04u   /* passed to alarm_on_change (notification: email, relay; Stage 8) */
#define AL_STOPS   0x08u   /* while active the instrument is in FAULT: measurement stopped (plat_state.h) */
#define AL_NO_OFF  0x10u   /* its tier can't be set to Off (AL_STOPS alarms never can either) */

typedef struct {
    uint16_t    id;            /* 1..99 platform, 100..255 application */
    uint8_t     severity;      /* AL_SEV_* */
    uint8_t     flags;         /* AL_* */
    uint32_t    on_delay_ms;   /* condition must last this long to raise (0 = at once) */
    uint32_t    off_delay_ms;  /* and be gone this long to clear */
    const char *name;          /* event log and JSON key: [A-Z0-9_], up to 22 characters */
    const char *text;          /* description shown when no text parameter is given */
    uint16_t    text_param;    /* PT_TEXT / PT_TEXT16 parameter with an editable description (0 = none) */
    uint16_t    enable_param;  /* bool parameter: false suppresses the alarm (0 = always enabled) */
    uint16_t    delay_param;   /* integer parameter, s: replaces on_delay_ms (0 = none) */
    uint8_t     route;         /* default actions: AR_EMAIL | AR_MODBUS | AR_RELAY(k) (0 = none) */
    const char *help;          /* what to check, for the service manual and docs/FAULT_CODES.md (may be NULL) */
} alarm_def_t;

/* ---------------- platform alarms ---------------- */
enum {
    ALARM_ID_CAL_INVALID       = 1,   /* no valid factory (calibration) data: measurement blocked */
    ALARM_ID_PARAM_WRITE_FAIL  = 2,   /* settings could not be written to the FRAM */
    ALARM_ID_LOG_STORAGE_FAULT = 3,   /* log flash erase/write/read-back failed */
    ALARM_ID_IP_CONFLICT       = 4,   /* another device answers at our static IP address */
    ALARM_ID_REMOTE_LINK_LOST  = 5,   /* remote control and no remote traffic for the timeout */
    ALARM_ID_UNEXPECTED_RESET  = 6,   /* the watchdog or a brown-out restarted the unit */
    ALARM_ID_FW_ROLLED_BACK    = 7,   /* a firmware update failed its trial and was undone */
    ALARM_ID_DISPLAY_FAULT     = 8,   /* the touchscreen stopped responding */
    ALARM_ID_NOTIFY_FAIL       = 10,  /* an alarm email could not be sent (after retries) */
    ALARM_ID_RESOURCE_LOW      = 11,  /* memory running low (plat_health.h) */
    ALARM_ID_IOB_LOST          = 12,  /* an enabled I/O board does not answer on RS-485 (iobus.h) */
    ALARM_ID_IOB_CONFIG        = 13,  /* an I/O board is the wrong type, or its channel settings conflict */
    ALARM_ID_IOB_OUT_FAULT     = 14,  /* an I/O board output: open circuit, short, overheating, no field supply */
    ALARM_ID_IOB_IN_FAULT      = 15,  /* an I/O board input: wire break or input chip fault */
    ALARM_ID_IOB_SUPPLY        = 16,  /* an I/O board's field 24 V supply is missing */
    ALARM_ID_IOB_NODE_RESET    = 17,  /* an I/O board was restarted by its watchdog */
    ALARM_ID_IOB_TEMP_FAULT    = 18,  /* an I/O board temperature input: open, short, out of range, converter fault */
    ALARM_ID_SAFE_MODE         = 19,  /* restarted repeatedly: safe mode, the application stopped (CIP-08.5) */
    ALARM_ID_SUPPLY            = 20,  /* the 24 V supply outside 19-30 V (plat_power.h) */
    ALARM_ID_CAL_DUE           = 21,  /* a channel's calibration is overdue (plat_cal.h, CIP-18) */
    ALARM_ID_HW_REV            = 22,  /* the board's revision straps disagree with the factory hw_rev (CIP-01.3) */
    ALARM_ID_SELFTEST          = 23,  /* a start-up self-test failed (plat_selftest.h, CIP-11.1) */
    ALARM_ID_QAL3              = 24,  /* an analyser's EN 14181 QAL3 chart is out of control, or a check is due (emis_qal3.h) */
    ALARM_ID_DIA_FIRST         = 30,  /* 30..53: digital input alarm slot s = ID - 30 (below) */
};

/* ---------------- digital input alarms (Gary 30 Oct; docs/MODBUS_STANDARD_BLOCK.md section 7) ----
 * Any digital input can raise an alarm that goes out on Modbus, named by the label typed on the
 * touchscreen's or the web page's alarm setup. PLAT_DIA_SLOTS slots, alarm ALARM_ID_DIA_FIRST + s,
 * each with parameters 340-344 (input, alarm when active or inactive, on-delay, latch, severity)
 * and a label (350 + s, the alarm's description; empty: "Digital input n"). Routing is the usual
 * per-alarm actions (default Modbus). An unused slot never raises; an unreliable input (wire
 * break, I/O board lost) does not raise it either (alarms 12 and 15 cover that) and shows in its
 * status. Modbus discrete inputs, read-only:
 *   7000 + s slot s active, 7032 + s slot s waiting for an acknowledge,
 *   7064 + s slot s input unreliable, 7100 + i digital input i+1 after the filter. */
#define PLAT_DIA_SLOTS   24u
#define PLAT_DIA_LABEL   33u          /* label bytes: 32 characters and the end */
enum { DIA_ST_UNUSED = 0, DIA_ST_NORMAL, DIA_ST_ACTIVE_ACKED, DIA_ST_ACTIVE_UNACKED, DIA_ST_CLEARED_UNACKED,
       DIA_ST_UNRELIABLE };
#define PLAT_DIA_MB_ADDR 7000u        /* discrete inputs 7000-7999: the core's */
#define PLAT_DIA_MB_END  7999u
void alarm_io_in(const plat_io_in_t *in);       /* scan task, after the inputs are read: the slots' conditions */
bool plat_dia_mb_bit(uint16_t addr);             /* discrete input addr (7000-7999) as Modbus serves it */
/* Input n (1..PLAT_DI_MAX) by where it is: "I/O board k input j" or "Input n". */
void plat_dia_input_name(unsigned n, char *buf, size_t cap);

/* A copy of one alarm, for lists. */
typedef struct {
    uint16_t id;
    uint8_t  severity, flags;  /* severity follows the tier in force */
    uint8_t  priority;         /* AL_PRIO_CRITICAL..AL_PRIO_INFO, or AL_PRIO_OFF */
    bool     active, unacked, cond, enabled;
    uint32_t t_raise;          /* s since 1970 when raised (0 if the clock was not set) */
    uint32_t up_raise;         /* uptime s when raised */
    const char *name;
    const char *help;          /* what to check (the definition's; may be NULL) */
    char     text[64];         /* the description (UTF-8) */
} alarm_view_t;

typedef struct {
    uint16_t active;           /* alarms active (including acknowledged ones) */
    uint16_t unacked;          /* alarms waiting for an acknowledge (active or cleared) */
    uint16_t shown;            /* entries in the list: active or unacked */
    int8_t   worst;            /* highest severity among the shown entries, -1 if none */
    uint8_t  worst_prio;       /* most urgent tier among them, 0 if none */
    bool     worst_unacked;    /* that severity has an unacknowledged entry */
} alarm_summary_t;

/* Build the table from the platform alarms plus the application's. Returns 0, or
 * the first bad ID (duplicate, 0, > 255, an application ID below 100, a missing
 * parameter or a full table). reset_cause (bsp_reset_cause_t) raises
 * UNEXPECTED_RESET after a watchdog or brown-out reset. Call after param_init. */
int  alarm_init(const alarm_def_t *app_defs, size_t n_app, uint8_t reset_cause);

/* Producers. Unknown IDs are ignored. alarm_set_range: the condition is "value
 * outside lo..hi"; once raised it stays until the value is back inside by the
 * deadband (no chatter on a limit: K5). NaN keeps the previous condition. */
void alarm_set(uint16_t id, bool cond);
void alarm_set_range(uint16_t id, float value, float lo, float hi, float deadband);

void alarm_scan(uint32_t now_ms);     /* scan task: platform conditions, delays, states, events */
void alarm_io_out(plat_io_out_t *out); /* scan task, after the application's outputs: the alarm relays */
uint8_t alarm_route(uint16_t id);     /* the actions in force for an alarm (AR_* without AR_SET) */
uint8_t alarm_relays(void);           /* bit k = relay k in alarm (before inversion) */

/* Email (mail.c provides it; weak no-op otherwise): one alarm transition to send. */
void plat_alarm_email(const alarm_def_t *d, uint8_t what, uint32_t t_unix, uint32_t up_s);

/* Acknowledge one alarm or ALARM_ACK_ALL. 0 done (or nothing to acknowledge),
 * -1 unknown ID. Scan task (via PARAM_ID_ALARM_ACK). */
int  alarm_ack(uint16_t id, uint8_t src, uint8_t level);

/* Readers (any task). The entries that are active or unacked, most urgent first
 * (unacked active, then acked active, then cleared unacked; higher severity first;
 * newest first). alarm_next walks them one at a time (start with *pos = 0; false at
 * the end); alarm_list copies up to max after skipping skip (a page). */
bool   alarm_next(uint64_t *pos, alarm_view_t *out);
size_t alarm_list(alarm_view_t *out, size_t skip, size_t max);
bool   alarm_get(uint16_t id, alarm_view_t *out);
void   alarm_summary(alarm_summary_t *s);
bool   alarm_active(uint16_t id);          /* e.g. to mark a logged value FLAG */
size_t alarm_count(void);                  /* definitions */
const alarm_def_t *alarm_def_at(size_t i);
const char *alarm_sev_name(uint8_t sev);
uint8_t alarm_prio(uint16_t id);                       /* tier in force, AL_PRIO_OFF, or 0 if unknown */
uint8_t alarm_prio_default(const alarm_def_t *d);      /* the definition's tier */
bool    alarm_can_switch_off(const alarm_def_t *d);    /* not AL_STOPS or AL_NO_OFF */
const char *alarm_prio_name(uint8_t prio);             /* the tier's (editable) name, "Off", or "?" */

/* Notification hook (Stage 8: email, relay, nothing). Called in the scan task on
 * every transition of an AL_NOTIFY alarm. what: 1 raised, 2 cleared, 3 acknowledged. */
enum { ALARM_TR_RAISE = 1, ALARM_TR_CLEAR = 2, ALARM_TR_ACK = 3 };
extern void (*alarm_on_change)(const alarm_def_t *d, uint8_t what);

/* For tests: forget every state (definitions stay). */
void alarm_reset_for_test(void);

#endif
