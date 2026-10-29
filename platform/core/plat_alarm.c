/* plat_alarm.c - the alarm manager (plat_alarm.h). */
#include "plat_alarm.h"
#include "plat_state.h"
#include "plat_power.h"
#include "plat_cal.h"
#include "plat_selftest.h"
#include "modbus.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "param.h"
#include "param_store.h"
#include "plat_log.h"
#include "plat_port.h"
#include "plat_cmdq.h"
#include "fw_update.h"
#include "bsp.h"
#include "ul_system.h"
#include "plat_health.h"

plat_health_t plat_health;

bool plat_health_low(const plat_health_t *h, char *why, unsigned cap) {
    if (!h->valid) return false;
    if ((h->heap_free || h->heap_min) && h->heap_min < PLAT_HEAP_LOW) {   /* (0/0: not known, e.g. the PC run) */ if (why && cap) snprintf(why, cap, "heap: %lu bytes left at worst", (unsigned long)h->heap_min); return true; }
    for (unsigned i = 0; i < h->n_tasks && i < PLAT_HEALTH_TASKS; ++i)
        if (h->task[i].free_words < PLAT_STACK_LOW) {
            if (why && cap) snprintf(why, cap, "stack of %s: %u words left at worst", h->task[i].name, h->task[i].free_words);
            return true;
        }
    return false;
}

/* Display working, or none fitted. The UI task provides the real one. */
__attribute__((weak)) bool plat_ui_healthy(void) { return true; }

static const alarm_def_t core_defs[] = {
    { .id = ALARM_ID_CAL_INVALID, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "CAL_INVALID",
      .text = "No valid calibration data: measurement blocked",
      .help = "Neither copy of the factory data in the settings memory is valid (new unit, or memory fault). Calibrate at factory login, or restore a backup taken from this unit." },
    { .id = ALARM_ID_PARAM_WRITE_FAIL, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "PARAM_WRITE_FAIL",
      .text = "Settings memory fault: changes may not be kept", .on_delay_ms = 2000,
      .help = "The settings memory (FRAM) did not verify a write, or is not fitted (RAM fallback). Check the FRAM; changes made now may be lost at power-off." },
    { .id = ALARM_ID_LOG_STORAGE_FAULT, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "LOG_STORAGE_FAULT",
      .text = "Log memory fault: records are not being kept",
      .help = "The log flash failed an erase, write or read-back. Records are not being kept. Replace the core board if it persists." },
    { .id = ALARM_ID_IP_CONFLICT, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "IP_CONFLICT",
      .text = "IP address in use by another device: network off",
      .help = "Another device answers at this unit's static IP address; the plant port is off until it is free. Change one of the addresses." },
    { .id = ALARM_ID_REMOTE_LINK_LOST, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "REMOTE_LINK_LOST",
      .text = "Remote control: no messages from the control system",
      .help = "In REMOTE mode and no Modbus or web traffic from the control system for the timeout (parameter). Check the network and the PLC." },
    { .id = ALARM_ID_UNEXPECTED_RESET, .severity = AL_SEV_WARNING, .flags = AL_LATCH, .route = AR_MODBUS, .name = "UNEXPECTED_RESET",
      .text = "The unit restarted itself (watchdog or supply dip)",
      .help = "The watchdog or a supply dip restarted the unit. The last fault on the System screen says which task or check stopped. Acknowledge to clear." },
    { .id = ALARM_ID_SAFE_MODE, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "SAFE_MODE",
      .text = "Restarted repeatedly: safe mode, application stopped",
      .help = "The unit restarted repeatedly within a short time, so it started without the application (outputs off). Read the last fault, then power-cycle." },
    { .id = ALARM_ID_SUPPLY, .severity = AL_SEV_WARNING, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "SUPPLY",
      .text = "Supply voltage outside 19-30 V", .on_delay_ms = 2000, .off_delay_ms = 2000,
      .help = "The 24 V supply measured outside 19-30 V. Check the power supply and wiring." },
    { .id = ALARM_ID_CAL_DUE, .severity = AL_SEV_WARNING, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "CAL_DUE",
      .text = "Calibration due",
      .help = "A calibrated channel is past its calibration interval. Calibrate it (Calibration page or screen)." },
    { .id = ALARM_ID_HW_REV, .severity = AL_SEV_WARNING, .route = AR_MODBUS, .name = "HW_REV",
      .text = "Board revision differs from the factory record",
      .help = "The board's revision straps differ from the hardware revision in the factory record. Check that the core board was not swapped; update the record at factory login." },
    { .id = ALARM_ID_SELFTEST, .severity = AL_SEV_WARNING, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "SELFTEST",
      .text = "Start-up self-test failed: see System information",
      .help = "A start-up self-test failed (System information lists which). See docs/FAULT_CODES.md, self-test checks." },
    { .id = ALARM_ID_FW_ROLLED_BACK, .severity = AL_SEV_WARNING, .flags = AL_LATCH, .route = AR_MODBUS, .name = "FW_ROLLED_BACK",
      .text = "Firmware update failed its trial and was undone",
      .help = "A new firmware image restarted during its trial, so the previous image was put back. Acknowledge; report the version to the manufacturer." },
    { .id = ALARM_ID_NOTIFY_FAIL, .severity = AL_SEV_WARNING, .route = AR_MODBUS, .name = "NOTIFY_FAIL",
      .text = "Alarm email could not be sent",
      .help = "An alarm email could not be sent after retries. Check the mail server settings and the network." },
    { .id = ALARM_ID_RESOURCE_LOW, .severity = AL_SEV_WARNING, .flags = AL_LATCH | AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS,
      .name = "RESOURCE_LOW", .text = "Memory running low: tell the manufacturer", .on_delay_ms = 10000,
      .help = "Free memory (heap or a task's stack) fell below its margin. Note the System screen figures and tell the manufacturer." },
    { .id = ALARM_ID_DISPLAY_FAULT, .severity = AL_SEV_WARNING, .route = AR_MODBUS, .name = "DISPLAY_FAULT",
      .text = "Touchscreen not responding", .on_delay_ms = 5000,
      .help = "The touchscreen stopped answering. Check its ribbon cable; the unit keeps measuring and the web page still works." },
    { .id = ALARM_ID_IOB_LOST, .severity = AL_SEV_FAULT, .flags = AL_LATCH | AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "IOB_LOST",
      .text = "I/O board not responding",
      .help = "An enabled I/O board does not answer. Check its power, its address switch and the I/O network cable." },
    { .id = ALARM_ID_IOB_CONFIG, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "IOB_CONFIG",
      .text = "I/O board wrong type or channel settings conflict",
      .help = "An I/O board is not the type expected, or two channel settings conflict. Check the board type and the I/O settings." },
    { .id = ALARM_ID_IOB_OUT_FAULT, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "IOB_OUT_FAULT",
      .text = "Output fault: open circuit, short or overheating", .on_delay_ms = 1000,
      .help = "An I/O board output reports an open circuit, a short or overheating. Check the load and its wiring." },
    { .id = ALARM_ID_IOB_IN_FAULT, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "IOB_IN_FAULT",
      .text = "Input fault: wire break", .on_delay_ms = 1000,
      .help = "An I/O board input reports a wire break or an input chip fault. Check the sensor wiring." },
    { .id = ALARM_ID_IOB_SUPPLY, .severity = AL_SEV_FAULT, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "IOB_SUPPLY",
      .text = "I/O board field 24 V supply missing", .on_delay_ms = 500,
      .help = "An I/O board's field 24 V supply is missing. Check its field supply terminals." },
    { .id = ALARM_ID_IOB_NODE_RESET, .severity = AL_SEV_WARNING, .flags = AL_LATCH, .route = AR_MODBUS, .name = "IOB_NODE_RESET",
      .text = "I/O board restarted by its watchdog",
      .help = "An I/O board was restarted by its own watchdog. Acknowledge; report it if it repeats." },
    { .id = ALARM_ID_IOB_TEMP_FAULT, .severity = AL_SEV_ALARM, .flags = AL_NOTIFY, .route = AR_EMAIL | AR_MODBUS, .name = "IOB_TEMP_FAULT",
      .text = "Temperature input fault: open, short or out of range", .on_delay_ms = 3000,
      .help = "A temperature input is open, shorted, out of range, or its converter failed. Check the sensor and wiring." },
};
#define N_CORE (sizeof core_defs / sizeof core_defs[0])

typedef struct {
    bool     active, unacked, enabled;
    bool     range_hi;          /* alarm_set_range: which side it went out on (deadband) */
    uint32_t since;             /* ms the condition has differed from 'active' (0 = not timing) */
    uint32_t t_raise, up_raise;
} al_state_t;

static const alarm_def_t *defs[PLAT_ALARM_MAX];
static size_t n_defs;
static uint8_t idx_of[ALARM_ID_MAX + 1];          /* 0xFF = none */
static volatile uint8_t cond[PLAT_ALARM_MAX];     /* written by producers */
static uint8_t latch_pulse[PLAT_ALARM_MAX];       /* one-shot conditions (reset cause) */
static al_state_t st[PLAT_ALARM_MAX];

/* published through the parameter table (param.c binds them) */
uint16_t plat_alarm_active_count, plat_alarm_unack_count;
uint16_t plat_alarm_active_bits[ALARM_BIT_WORDS], plat_alarm_unack_bits[ALARM_BIT_WORDS];
uint16_t plat_alarm_common;                         /* ACOM_* */
uint8_t  plat_alarm_route[ALARM_ID_MAX + 1];        /* AR_SET | actions, per ID (persisted) */
uint8_t  plat_alarm_relay_do[ALARM_RELAYS];         /* digital output 1..32 for relay k (0 = not used) */
uint8_t  plat_alarm_relay_mode[ALARM_RELAYS];       /* AR_MODE_* */
bool     plat_alarm_relay_inv[ALARM_RELAYS];        /* fail-safe: energised when healthy */
static uint8_t relay_on;

void (*alarm_on_change)(const alarm_def_t *d, uint8_t what);
extern void (*param_on_alarm_ack)(uint16_t id, uint8_t src, uint8_t level);

static int idx(uint16_t id) { return id <= ALARM_ID_MAX && idx_of[id] < n_defs ? idx_of[id] : -1; }

static int add(const alarm_def_t *d, bool app) {
    if (!d->id || d->id > ALARM_ID_MAX || (app && d->id < ALARM_ID_APP_FIRST) || idx_of[d->id] != 0xFF
        || n_defs >= PLAT_ALARM_MAX || d->severity > AL_SEV_FAULT || !d->name)
        return d->id ? d->id : -1;
    if (d->text_param) {
        const param_def_t *p = param_find(d->text_param);
        if (!p || (p->ptype != PT_TEXT && p->ptype != PT_TEXT16)) return d->id;
    }
    if (d->enable_param) { const param_def_t *p = param_find(d->enable_param); if (!p || PARAM_IS_BLOB(p)) return d->id; }
    if (d->delay_param) {
        const param_def_t *p = param_find(d->delay_param);
        if (!p || PARAM_IS_BLOB(p) || p->ptype == PT_FLOAT || p->ptype == PT_BOOL) return d->id;
    }
    idx_of[d->id] = (uint8_t)n_defs;
    defs[n_defs++] = d;
    return 0;
}

static void ack_hook(uint16_t id, uint8_t src, uint8_t level) { (void)alarm_ack(id, src, level); }

int alarm_init(const alarm_def_t *app_defs, size_t n_app, uint8_t reset_cause) {
    memset(idx_of, 0xFF, sizeof idx_of);
    n_defs = 0;
    for (size_t k = 0; k < N_CORE; ++k) { int e = add(&core_defs[k], false); if (e) return e; }
    for (size_t k = 0; k < n_app; ++k) { int e = add(&app_defs[k], true); if (e) return e; }
    alarm_reset_for_test();
    if (reset_cause == BSP_RESET_IWDG || reset_cause == BSP_RESET_WWDG || reset_cause == BSP_RESET_BROWNOUT)
        latch_pulse[idx_of[ALARM_ID_UNEXPECTED_RESET]] = 1;
    param_on_alarm_ack = ack_hook;
    return 0;
}

void alarm_reset_for_test(void) {
    PLAT_CRIT_ENTER();
    memset(st, 0, sizeof st);
    for (size_t i = 0; i < PLAT_ALARM_MAX; ++i) { cond[i] = 0; latch_pulse[i] = 0; st[i].enabled = true; }
    PLAT_CRIT_EXIT();
    plat_alarm_active_count = plat_alarm_unack_count = 0;
    plat_alarm_common = 0; relay_on = 0;
    memset(plat_alarm_active_bits, 0, sizeof plat_alarm_active_bits);
    memset(plat_alarm_unack_bits, 0, sizeof plat_alarm_unack_bits);
}

void alarm_set(uint16_t id, bool c) {
    int i = idx(id);
    if (i >= 0) cond[i] = c ? 1u : 0u;
}

void alarm_set_range(uint16_t id, float v, float lo, float hi, float db) {
    int i = idx(id);
    if (i < 0 || isnan(v)) return;
    if (!(db >= 0.0f)) db = 0.0f;
    bool c;
    if (!cond[i]) {
        c = v < lo || v > hi;
        if (c) st[i].range_hi = v > hi;           /* scan-task producer: its own bookkeeping */
    } else {                                      /* raised: back inside by the deadband to go */
        c = st[i].range_hi ? v > hi - db : v < lo + db;
        if (!c && (v < lo || v > hi)) { c = true; st[i].range_hi = v > hi; }   /* jumped to the other side */
    }
    cond[i] = c ? 1u : 0u;
}

/* ---------------- scan ---------------- */
static bool param_bool(uint16_t id, bool dflt) {
    plat_val_t v;
    if (!id || !param_read(id, &v)) return dflt;
    const param_def_t *d = param_find(id);
    return d && d->ctype == CT_F32 ? v.f != 0.0f : v.u != 0;
}

static uint32_t on_delay(const alarm_def_t *d) {
    plat_val_t v;
    if (d->delay_param && param_read(d->delay_param, &v)) {
        const param_def_t *p = param_find(d->delay_param);
        int64_t s = p->ptype == PT_INT ? (int64_t)v.i : (int64_t)v.u;
        if (s < 0) s = 0;
        if (s > 86400) s = 86400;
        return (uint32_t)s * 1000u;
    }
    return d->on_delay_ms;
}

static uint32_t unix_now(void) {
    if (RTC_Date.Year < 2000 || !RTC_Date.Month) return 0;
    return plat_unix_from_civil(RTC_Date.Year, RTC_Date.Month, RTC_Date.Day_In_Month,
                                RTC_Time.Hours, RTC_Time.Minutes, RTC_Time.Seconds);
}

__attribute__((weak)) void plat_alarm_email(const alarm_def_t *d, uint8_t what, uint32_t t_unix, uint32_t up_s) {
    (void)d; (void)what; (void)t_unix; (void)up_s;
}

static uint8_t route_of(size_t i) {
    uint8_t r = plat_alarm_route[defs[i]->id];
    return r & AR_SET ? (uint8_t)(r & ~AR_SET) : defs[i]->route;
}
uint8_t alarm_route(uint16_t id) { int i = idx(id); return i < 0 ? 0 : route_of((size_t)i); }

static void tell(size_t i, uint8_t what, uint8_t src, uint8_t level) {
    const alarm_def_t *d = defs[i];
    static const uint16_t code[] = { 0, LOG_EV_ALARM_RAISE, LOG_EV_ALARM_CLEAR, LOG_EV_ALARM_ACK };
    plat_log_event(code[what], src, level, d->id, NAN, NAN, d->name);
    if (what != ALARM_TR_ACK && (route_of(i) & AR_EMAIL)) plat_alarm_email(d, what, unix_now(), st[i].up_raise);
    if ((d->flags & AL_NOTIFY) && alarm_on_change) alarm_on_change(d, what);
}

static void poll_platform(void) {
    uint32_t ps = param_store_status(), ls = plat_log_status();
    alarm_set(ALARM_ID_CAL_INVALID, !param_store_calibration_valid());
    alarm_set(ALARM_ID_SAFE_MODE, plat_state() == PLAT_ST_SAFE_MODE);
    alarm_set(ALARM_ID_CAL_DUE, plat_cal_any_due());
    alarm_set(ALARM_ID_SELFTEST, (plat_selftest_failed() & ~PLAT_STEST_OWN_ALARM) != 0);
    { plat_val_t hw;                      /* only once there is a factory record to compare with */
      alarm_set(ALARM_ID_HW_REV, fw_board_rev() >= 0 && param_store_calibration_valid() &&
                                 param_read(PARAM_ID_HW_REV, &hw) && hw.u != (uint32_t)fw_board_rev()); }
    { uint32_t mv = plat_supply_mv();
      alarm_set(ALARM_ID_SUPPLY, mv != 0 && (mv < PLAT_SUPPLY_LOW_MV || mv > PLAT_SUPPLY_HIGH_MV)); }
    alarm_set(ALARM_ID_PARAM_WRITE_FAIL, (ps & (PSTAT_WRITE_ERROR | PSTAT_NVM_VOLATILE)) != 0);
    alarm_set(ALARM_ID_LOG_STORAGE_FAULT, (ls & LSTAT_NOR_FAULT) != 0);
    alarm_set(ALARM_ID_IP_CONFLICT, (plat_net_status_word & NETST_CONFLICT) != 0);
    alarm_set(ALARM_ID_REMOTE_LINK_LOST, param_remote_link_lost());
    alarm_set(ALARM_ID_DISPLAY_FAULT, !plat_ui_healthy());
    fw_status_t fw;
    fw_status(&fw);
    alarm_set(ALARM_ID_FW_ROLLED_BACK, fw.rolled_back);
    alarm_set(ALARM_ID_RESOURCE_LOW, plat_health_low(&plat_health, NULL, 0));
}

static void publish(void) {
    uint16_t na = 0, nu = 0, ab[ALARM_BIT_WORDS] = { 0 }, ub[ALARM_BIT_WORDS] = { 0 }, com = 0;
    uint8_t rel = 0;
    for (size_t i = 0; i < n_defs; ++i) {
        uint16_t id = defs[i]->id;
        bool a = st[i].active, u = st[i].unacked;
        if (a) { na++; ab[id / 16u] |= (uint16_t)(1u << (id % 16u)); }
        if (u) { nu++; ub[id / 16u] |= (uint16_t)(1u << (id % 16u)); }
        if (!a && !u) continue;
        uint8_t r = route_of(i);
        com |= (uint16_t)((a ? ACOM_ANY_ACTIVE : 0u) | (u ? ACOM_ANY_UNACKED : 0u));
        if (r & AR_MODBUS) com |= (uint16_t)((a ? ACOM_MB_ACTIVE : 0u) | (u ? ACOM_MB_UNACKED : 0u));
        for (unsigned k = 0; k < ALARM_RELAYS; ++k) {
            if (!(r & AR_RELAY(k))) continue;
            uint8_t m = plat_alarm_relay_mode[k];
            if (m == AR_MODE_UNACKED ? u : m == AR_MODE_ACTIVE_OR_UNACKED ? (a || u) : a) rel |= (uint8_t)(1u << k);
        }
    }
    for (unsigned k = 0; k < ALARM_RELAYS; ++k) if (rel & (1u << k)) com |= ACOM_RELAY(k);
    relay_on = rel;
    if (mb_force_count()) com |= ACOM_FORCED;
    plat_alarm_common = com;
    plat_alarm_active_count = na; plat_alarm_unack_count = nu;
    memcpy(plat_alarm_active_bits, ab, sizeof ab);
    memcpy(plat_alarm_unack_bits, ub, sizeof ub);
}

void alarm_scan(uint32_t now) {
    static uint32_t last_poll;
    if (now - last_poll >= 200u || !last_poll) { last_poll = now ? now : 1u; poll_platform(); }
    if (!now) now = 1;                                     /* 0 means "not timing" */
    for (size_t i = 0; i < n_defs; ++i) {
        const alarm_def_t *d = defs[i];
        al_state_t *s = &st[i];
        bool en = param_bool(d->enable_param, true);
        if (!en) {                                         /* suppressed: out of the list at once */
            if (s->active || s->unacked) {
                bool was = s->active;
                PLAT_CRIT_ENTER(); s->active = s->unacked = false; s->since = 0; s->enabled = false; PLAT_CRIT_EXIT();
                if (was) tell(i, ALARM_TR_CLEAR, PLAT_SRC_INTERNAL, 0);
            }
            s->enabled = false;
            latch_pulse[i] = 0;
            continue;
        }
        s->enabled = true;
        bool c = cond[i] || latch_pulse[i];
        latch_pulse[i] = 0;
        if (c == s->active || (s->active && !c && (d->flags & AL_LATCH) && s->unacked)) {
            s->since = 0;                                  /* steady (or held by the latch) */
            continue;
        }
        if (!s->since) s->since = now;
        uint32_t need = c ? on_delay(d) : d->off_delay_ms;
        if (now - s->since < need) continue;
        s->since = 0;
        if (c) {
            uint32_t tu = unix_now(), up = now / 1000u;
            PLAT_CRIT_ENTER();
            s->active = true;
            s->unacked = !(d->flags & AL_NO_ACK);
            s->t_raise = tu; s->up_raise = up;
            PLAT_CRIT_EXIT();
            tell(i, ALARM_TR_RAISE, PLAT_SRC_INTERNAL, 0);
        } else {
            PLAT_CRIT_ENTER(); s->active = false; PLAT_CRIT_EXIT();
            tell(i, ALARM_TR_CLEAR, PLAT_SRC_INTERNAL, 0);
        }
    }
    publish();
}

static bool ack_one(size_t i, uint8_t src, uint8_t level) {
    al_state_t *s = &st[i];
    if (!s->unacked) return false;
    bool clear = s->active && (defs[i]->flags & AL_LATCH) && !cond[i];
    PLAT_CRIT_ENTER();
    s->unacked = false;
    if (clear) { s->active = false; s->since = 0; }
    PLAT_CRIT_EXIT();
    tell(i, ALARM_TR_ACK, src, level);
    if (clear) tell(i, ALARM_TR_CLEAR, PLAT_SRC_INTERNAL, 0);
    return true;
}

int alarm_ack(uint16_t id, uint8_t src, uint8_t level) {
    if (id == ALARM_ACK_ALL) {
        for (size_t i = 0; i < n_defs; ++i) (void)ack_one(i, src, level);
    } else {
        int i = idx(id);
        if (i < 0) return -1;
        (void)ack_one((size_t)i, src, level);
    }
    publish();
    return 0;
}

void alarm_io_out(plat_io_out_t *out) {
    for (unsigned k = 0; k < ALARM_RELAYS; ++k) {
        uint8_t ch = plat_alarm_relay_do[k];
        if (ch < 1 || ch > PLAT_DO_MAX) continue;
        plat_do_set(out, ch - 1u, ((relay_on >> k) & 1u) != plat_alarm_relay_inv[k]);
    }
}
uint8_t alarm_relays(void) { return relay_on; }

/* ---------------- readers ---------------- */
static void text_of(const alarm_def_t *d, char *out, size_t cap) {
    out[0] = 0;
    if (d->text_param) {
        const param_def_t *p = param_find(d->text_param);
        uint8_t raw[128];
        uint16_t n = p->size < sizeof raw ? p->size : (uint16_t)sizeof raw;
        if (param_read_blob(d->text_param, 0, raw, n) == PERR_OK) {
            size_t k = 0;
            if (p->ptype == PT_TEXT) {
                for (uint16_t j = 0; j < n && raw[j] && k + 1 < cap; ++j) out[k++] = (char)raw[j];
            } else {                                       /* UTF-16LE -> UTF-8 (BMP) */
                for (uint16_t j = 0; j + 1u < n; j = (uint16_t)(j + 2u)) {
                    uint16_t c = (uint16_t)(raw[j] | raw[j + 1] << 8);
                    if (!c) break;
                    if (c < 0x80) { if (k + 1 >= cap) break; out[k++] = (char)c; }
                    else if (c < 0x800) { if (k + 2 >= cap) break; out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
                    else { if (k + 3 >= cap) break; out[k++] = (char)(0xE0 | c >> 12); out[k++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[k++] = (char)(0x80 | (c & 0x3F)); }
                }
            }
            out[k] = 0;
        }
    }
    if (!out[0]) {
        const char *t = d->text ? d->text : d->name;
        size_t k = 0;
        for (; t[k] && k + 1 < cap; ++k) out[k] = t[k];
        out[k] = 0;
    }
}

static void view(size_t i, alarm_view_t *v) {
    const alarm_def_t *d = defs[i];
    PLAT_CRIT_ENTER();
    al_state_t s = st[i];
    PLAT_CRIT_EXIT();
    v->id = d->id; v->severity = d->severity; v->flags = d->flags; v->name = d->name; v->help = d->help;
    v->active = s.active; v->unacked = s.unacked; v->enabled = s.enabled; v->cond = cond[i] != 0;
    v->t_raise = s.t_raise; v->up_raise = s.up_raise;
    text_of(d, v->text, sizeof v->text);
}

/* Order key: lower = more urgent. Unacked active, acked active, cleared unacked;
 * then higher severity; then newest; the index makes every key unique. */
static uint64_t key_of(size_t i, const al_state_t *a) {
    uint64_t g = a->active && a->unacked ? 0u : a->active ? 1u : 2u;
    uint64_t sev = (uint64_t)(AL_SEV_FAULT - defs[i]->severity);
    return g << 48 | sev << 40 | (uint64_t)(0xFFFFFFFFu - a->up_raise) << 8 | i;
}

bool alarm_next(uint64_t *pos, alarm_view_t *out) {
    uint64_t best = UINT64_MAX;
    size_t bi = 0;
    for (size_t i = 0; i < n_defs; ++i) {
        PLAT_CRIT_ENTER();
        al_state_t a = st[i];
        PLAT_CRIT_EXIT();
        if (!a.active && !a.unacked) continue;
        uint64_t k = key_of(i, &a);
        if ((*pos == 0 || k > *pos - 1u) && k < best) { best = k; bi = i; }
    }
    if (best == UINT64_MAX) return false;
    *pos = best + 1u;
    view(bi, out);
    return true;
}

size_t alarm_list(alarm_view_t *out, size_t skip, size_t max) {
    uint64_t pos = 0;
    alarm_view_t v;
    size_t n = 0;
    while (n < max && alarm_next(&pos, &v)) {
        if (skip) { skip--; continue; }
        out[n++] = v;
    }
    return n;
}

bool alarm_get(uint16_t id, alarm_view_t *out) {
    int i = idx(id);
    if (i < 0) return false;
    view((size_t)i, out);
    return true;
}

void alarm_summary(alarm_summary_t *s) {
    memset(s, 0, sizeof *s);
    s->worst = -1;
    for (size_t i = 0; i < n_defs; ++i) {
        PLAT_CRIT_ENTER();
        al_state_t a = st[i];
        PLAT_CRIT_EXIT();
        if (a.active) s->active++;
        if (a.unacked) s->unacked++;
        if (!a.active && !a.unacked) continue;
        s->shown++;
        int8_t sev = (int8_t)defs[i]->severity;
        if (sev > s->worst) { s->worst = sev; s->worst_unacked = a.unacked; }
        else if (sev == s->worst && a.unacked) s->worst_unacked = true;
    }
}

bool alarm_active(uint16_t id) { int i = idx(id); return i >= 0 && st[i].active; }
size_t alarm_count(void) { return n_defs; }
const alarm_def_t *alarm_def_at(size_t i) { return i < n_defs ? defs[i] : NULL; }
const char *alarm_sev_name(uint8_t s) {
    static const char *const n[] = { "status", "warning", "alarm", "fault" };
    return s <= AL_SEV_FAULT ? n[s] : "?";
}
