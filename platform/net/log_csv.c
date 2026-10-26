/* log_csv.c - see log_csv.h */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "log_csv.h"
#include "plat_cmdq.h"

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char *put_uint(char *p, unsigned long long v, int width) {
    char tmp[24]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n < width) tmp[n++] = '0';
    while (n) *p++ = tmp[--n];
    return p;
}

void log_fmt_time(char *buf, size_t cap, uint32_t t, bool set) {
    if (cap < 24) { if (cap) buf[0] = 0; return; }
    char *p = buf;
    if (!set) {
        memcpy(p, "uptime ", 7); p += 7;
        p = put_uint(p, t, 1);
        *p++ = 's';
        *p = 0;
        return;
    }
    unsigned y, mo, d, h, mi, s;
    plat_civil_from_unix(t, &y, &mo, &d, &h, &mi, &s);
    p = put_uint(p, y, 4); *p++ = '-'; p = put_uint(p, mo, 2); *p++ = '-'; p = put_uint(p, d, 2);
    *p++ = ' ';
    p = put_uint(p, h, 2); *p++ = ':'; p = put_uint(p, mi, 2); *p++ = ':'; p = put_uint(p, s, 2);
    *p = 0;
}

void log_fmt_fixed(char *buf, size_t cap, double v, unsigned dec) {
    if (cap < 32) { if (cap) buf[0] = 0; return; }
    if (!isfinite(v)) { buf[0] = 0; return; }
    if (dec > 6) dec = 6;
    double scale = 1.0;
    for (unsigned k = 0; k < dec; ++k) scale *= 10.0;
    if (fabs(v) * scale >= 9.0e15) { fmt_float(buf, cap, v); return; }
    long long r = llround(v * scale);
    char *p = buf;
    if (r < 0) { *p++ = '-'; r = -r; }
    unsigned long long sc = (unsigned long long)scale;
    unsigned long long ip = (unsigned long long)r / sc, fp = (unsigned long long)r % sc;
    p = put_uint(p, ip, 1);
    if (dec) { *p++ = '.'; p = put_uint(p, fp, (int)dec); }
    *p = 0;
}

const char *log_src_name(uint8_t s) {
    switch (s) {
    case PLAT_SRC_LOCAL:    return "touchscreen";
    case PLAT_SRC_WEB:      return "web";
    case PLAT_SRC_MODBUS:   return "modbus";
    case PLAT_SRC_INTERNAL: return "unit";
    default:                return "?";
    }
}

const char *log_level_name(uint8_t l) {
    switch (l) {
    case PLAT_LVL_VIEW:     return "";
    case PLAT_LVL_OPERATOR: return "operator";
    case PLAT_LVL_ADMIN:    return "admin";
    case PLAT_LVL_FACTORY:  return "factory";
    default:                return "?";
    }
}

static void num_text(char *b, size_t cap, double v, bool ip) {
    if (!isfinite(v)) { snprintf(b, cap, "-"); return; }
    if (ip && v >= 0 && v <= 4294967295.0) {
        uint32_t u = (uint32_t)v;
        snprintf(b, cap, "%u.%u.%u.%u", (unsigned)(u >> 24), (unsigned)((u >> 16) & 255u), (unsigned)((u >> 8) & 255u), (unsigned)(u & 255u));
    } else if (v == floor(v) && fabs(v) < 1e15) {
        char *p = b;                                  /* whole numbers in full, not 3.2e+09 */
        if (cap < 24) { b[0] = 0; return; }           /* (newlib-nano's printf has no %lld) */
        if (v < 0) { *p++ = '-'; v = -v; }
        p = put_uint(p, (unsigned long long)v, 1);
        *p = 0;
    } else {
        fmt_float(b, cap, v);
    }
}

void log_event_text(const log_event_t *e, char *what, size_t wcap, char *detail, size_t dcap) {
    static const char *const nm[] = {
        [LOG_EV_POWER_UP] = "Power-up", [LOG_EV_PARAM] = "Setting changed", [LOG_EV_MODE] = "Mode",
        [LOG_EV_RESTORE_DEFAULTS] = "Defaults restored", [LOG_EV_TIME_SET] = "Clock set", [LOG_EV_LOGIN] = "Login",
        [LOG_EV_LOGIN_FAIL] = "Login failed", [LOG_EV_LOGOUT] = "Logout", [LOG_EV_PASSWORD] = "Password changed",
        [LOG_EV_RESTORE_BACKUP] = "Backup restored", [LOG_EV_ALARM_RAISE] = "Alarm", [LOG_EV_ALARM_CLEAR] = "Alarm cleared",
        [LOG_EV_ALARM_ACK] = "Alarm acknowledged", [LOG_EV_FW_UPDATE] = "Firmware", [LOG_EV_FW_ROLLBACK] = "Firmware rolled back",
        [LOG_EV_LOG_FAULT] = "Log fault", [LOG_EV_STORE_FAULT] = "Storage fault", [LOG_EV_LOST] = "Events lost",
        [LOG_EV_IP_CONFLICT] = "Address conflict", [LOG_EV_TLS_KEY] = "New HTTPS key",
        [LOG_EV_MAIL] = "Alarm email", [LOG_EV_SERIAL] = "Serial number", [LOG_EV_IOB] = "I/O board",
        [LOG_EV_STATE] = "State", [LOG_EV_POWER_FAIL] = "Power failing", [LOG_EV_POWER_RESTORED] = "Power restored",
        [LOG_EV_SERVICE] = "Service", [LOG_EV_CAL] = "Calibration", [LOG_EV_SELFTEST] = "Self-test" };
    static const char *const reset[] = { "", "power-on", "reset pin", "software reset", "watchdog", "window watchdog", "brown-out" };
    const char *w = e->code < sizeof nm / sizeof nm[0] && nm[e->code] ? nm[e->code] : (e->code >= LOG_EV_APP ? "Application" : "Event");
    snprintf(what, wcap, "%s", w);
    char a[24], b[24];
    bool ip = e->code == LOG_EV_PARAM && (strstr(e->text, "_ip") || !strcmp(e->text, "net_mask") || !strcmp(e->text, "net_gw"));
    num_text(a, sizeof a, e->old_v, ip);
    num_text(b, sizeof b, e->new_v, ip);
    switch (e->code) {
    case LOG_EV_PARAM: snprintf(detail, dcap, "%s (%lu): %s -> %s", e->text, (unsigned long)e->arg, a, b); break;
    case LOG_EV_MODE: snprintf(detail, dcap, "%s -> %s", e->old_v != 0.0 ? "REMOTE" : "LOCAL", e->new_v != 0.0 ? "REMOTE" : "LOCAL"); break;
    case LOG_EV_POWER_UP: snprintf(detail, dcap, "%s, %s", e->arg < 7u ? reset[e->arg] : "reset", e->text); break;
    case LOG_EV_TIME_SET:
        if (isfinite(e->old_v)) snprintf(detail, dcap, "stepped %ld s", (long)(e->new_v - e->old_v));
        else snprintf(detail, dcap, "clock set");
        break;
    case LOG_EV_PASSWORD: snprintf(detail, dcap, "%s", log_level_name((uint8_t)e->arg)); break;
    case LOG_EV_IP_CONFLICT:
        snprintf(detail, dcap, "%u.%u.%u.%u %s", (unsigned)(e->arg >> 24), (unsigned)((e->arg >> 16) & 255u),
                 (unsigned)((e->arg >> 8) & 255u), (unsigned)(e->arg & 255u),
                 e->new_v != 0.0 ? "is used by another device" : "is free again");
        break;
    case LOG_EV_SERIAL:
        if (isfinite(e->new_v)) snprintf(detail, dcap, "%s -> %s: %s", a, b, e->text);
        else snprintf(detail, dcap, "%s: %s", a, e->text);
        break;
    case LOG_EV_IOB: snprintf(detail, dcap, "board %lu: %s", (unsigned long)e->arg, e->text); break;
    case LOG_EV_SELFTEST: snprintf(detail, dcap, e->arg ? "failed: %s" : "%s", e->text); break;
    case LOG_EV_MAIL:
        snprintf(detail, dcap, "%s, %lu alarm event%s", e->text, (unsigned long)e->arg, e->arg == 1 ? "" : "s");
        break;
    default:
        if (e->arg && e->text[0]) snprintf(detail, dcap, "%s #%lu", e->text, (unsigned long)e->arg);
        else if (e->arg) snprintf(detail, dcap, "#%lu", (unsigned long)e->arg);
        else snprintf(detail, dcap, "%s", e->text);
    }
}

/* CSV field: quoted if it contains a comma, quote or line break */
static void csv_field(http_out_t *o, const char *s) {
    bool q = strpbrk(s, ",\"\r\n") != NULL;
    if (!q) { out_str(o, s); return; }
    out_str(o, "\"");
    for (; *s; ++s) { if (*s == '"') out_str(o, "\""); out_bytes(o, s, 1); }
    out_str(o, "\"");
}

static const char *qname(uint8_t q) {
    switch (q) { case LQ_OK: return "OK"; case LQ_FLAG: return "FLAG"; case LQ_BAD: return "BAD"; default: return "N/A"; }
}

static void load_live(log_csv_t *c) {
    c->nch = c->n_live > LOG_MAX_CHANS ? LOG_MAX_CHANS : c->n_live;
    for (unsigned i = 0; i < c->nch; ++i) {
        memset(c->name[i], 0, sizeof c->name[i]);
        memset(c->unit[i], 0, sizeof c->unit[i]);
        if (c->live[i].name) strncpy(c->name[i], c->live[i].name, 16);
        if (c->live[i].unit) strncpy(c->unit[i], c->live[i].unit, 6);
        c->dec[i] = c->live[i].dec;
    }
    c->iv = c->live_iv;
}

void log_csv_begin(log_csv_t *c, uint8_t ring, const plat_log_chan_t *live, unsigned n_live,
                   uint16_t live_iv, uint32_t from, uint32_t to, http_out_t *o) {
    memset(c, 0, sizeof *c);
    c->ring = ring;
    c->from = from;
    c->to = to;
    c->live = live;
    c->n_live = live ? n_live : 0;
    c->live_iv = live_iv;
    if (ring == LOG_RING_EVENTS) out_str(o, "seq,time,event,source,level,id,detail,old,new\r\n");
}

static bool in_range(const log_csv_t *c, uint32_t t) {
    return (c->from == 0 || t >= c->from) && (c->to == 0 || t <= c->to);
}

static void do_schema(log_csv_t *c) {
    const uint8_t *s0 = c->rec;
    uint16_t hash = get16(s0 + 12);
    if (c->schema_ok && hash == c->hash) return;          /* same configuration (e.g. after a restart) */
    c->hash = hash;
    c->iv = get16(s0 + 16);
    c->nch = s0[14] > LOG_MAX_CHANS ? LOG_MAX_CHANS : s0[14];
    for (unsigned ch = 0; ch < c->nch; ++ch) {
        unsigned part = 1u + ch / 2u;
        memset(c->name[ch], 0, sizeof c->name[ch]);
        memset(c->unit[ch], 0, sizeof c->unit[ch]);
        c->dec[ch] = 2;
        if (part >= c->need) continue;
        const uint8_t *e = c->rec + part * LOG_SLOT + 12 + 24u * (ch % 2u);
        memcpy(c->name[ch], e, 16);
        memcpy(c->unit[ch], e + 16, 6);
        c->dec[ch] = e[22];
    }
    c->schema_ok = true;
    c->header_out = false;
}

static void do_data(log_csv_t *c, http_out_t *o) {
    const uint8_t *s0 = c->rec;
    uint16_t hash = get16(s0 + 12);
    if (!c->schema_ok || hash != c->hash) {               /* schema record gone (overwritten) */
        if (c->live && plat_log_schema_hash(c->live, c->n_live, c->live_iv) == hash) {
            load_live(c);
        } else {
            unsigned n = 0;
            for (unsigned p = 0; p < c->need; ++p) {
                const uint8_t *s = c->rec + p * LOG_SLOT;
                unsigned top = (unsigned)s[14] + s[15];
                if (top > n) n = top;
            }
            c->nch = n > LOG_MAX_CHANS ? LOG_MAX_CHANS : n;
            for (unsigned i = 0; i < c->nch; ++i) {
                char *nm = c->name[i];
                memcpy(nm, "ch", 2);
                char *e = put_uint(nm + 2, i + 1u, 1); *e = 0;
                c->unit[i][0] = 0;
                c->dec[i] = 3;
            }
        }
        c->hash = hash;
        c->schema_ok = true;
        c->header_out = false;
    }
    uint32_t t = get32(s0 + 8);
    if (!in_range(c, t)) return;
    if (!c->header_out) {
        if (c->rows) out_str(o, "\r\n");
        out_str(o, "time");
        for (unsigned i = 0; i < c->nch; ++i) {
            char col[32];
            size_t k = strlen(c->name[i]);
            memcpy(col, c->name[i], k);
            if (c->unit[i][0]) { col[k++] = ' '; col[k++] = '['; size_t u = strlen(c->unit[i]); memcpy(col + k, c->unit[i], u); k += u; col[k++] = ']'; }
            col[k] = 0;
            out_str(o, ",");
            csv_field(o, col);
            memcpy(col, c->name[i], strlen(c->name[i]) + 1);
            strcat(col, " q");
            out_str(o, ",");
            csv_field(o, col);
        }
        out_str(o, "\r\n");
        c->header_out = true;
        c->have_last = false;
    }
    char tb[32];
    bool clock = !(s0[1] & LOG_F_NOCLOCK);
    if (clock && c->have_last && c->iv && t > c->last_t && t - c->last_t > 2u * (uint32_t)c->iv) {
        log_fmt_time(tb, sizeof tb, c->last_t + c->iv, true);    /* power-off gap (CIP-16.6) */
        out_str(o, tb);
        for (unsigned ch = 0; ch < c->nch; ++ch) out_str(o, ",,GAP");
        out_str(o, "\r\n");
        c->rows++;
    }
    c->have_last = clock;
    c->last_t = t;
    log_fmt_time(tb, sizeof tb, t, clock);
    out_str(o, tb);
    for (unsigned ch = 0; ch < c->nch; ++ch) {
        float v = NAN;
        uint8_t q = LQ_NA;
        for (unsigned p = 0; p < c->need; ++p) {
            const uint8_t *s = c->rec + p * LOG_SLOT;
            if (ch >= s[14] && ch < (unsigned)s[14] + s[15] && s[15] <= LOG_VALS_PER_SLOT) {
                unsigned k = ch - s[14];
                uint32_t u = get32(s + 16 + 5u * k);
                memcpy(&v, &u, 4);
                q = s[20 + 5u * k];
                break;
            }
        }
        log_fmt_fixed(tb, sizeof tb, (double)v, c->dec[ch]);
        out_str(o, ",");
        out_str(o, tb);
        out_str(o, ",");
        out_str(o, qname(q));
    }
    out_str(o, "\r\n");
    c->rows++;
}

static void do_event(log_csv_t *c, http_out_t *o) {
    log_event_t e;
    if (!plat_log_decode_event(c->rec, &e) || !in_range(c, e.time)) return;
    char b[40];
    char *p = put_uint(b, e.seq, 1); *p = 0;
    out_str(o, b); out_str(o, ",");
    log_fmt_time(b, sizeof b, e.time, e.clock_set);
    if (e.clock_set) { size_t k = strlen(b); b[k] = '.'; p = put_uint(b + k + 1, e.ms % 1000u, 3); *p = 0; }
    out_str(o, b); out_str(o, ",");
    out_str(o, plat_log_event_name(e.code));
    if (e.code >= LOG_EV_APP) { p = put_uint(b, e.code, 1); *p = 0; out_str(o, " "); out_str(o, b); }
    out_str(o, ","); out_str(o, log_src_name(e.src));
    out_str(o, ","); out_str(o, log_level_name(e.level));
    out_str(o, ",");
    if (e.code == LOG_EV_PARAM || e.code == LOG_EV_MODE || e.code == LOG_EV_RESTORE_DEFAULTS ||
        e.code == LOG_EV_ALARM_RAISE || e.code == LOG_EV_ALARM_CLEAR || e.code == LOG_EV_ALARM_ACK ||
        e.code == LOG_EV_POWER_UP || e.code == LOG_EV_LOG_FAULT || e.code == LOG_EV_STORE_FAULT ||
        e.code == LOG_EV_LOST || e.code == LOG_EV_PASSWORD || e.code >= LOG_EV_APP) {
        p = put_uint(b, e.arg, 1); *p = 0; out_str(o, b);
    }
    out_str(o, ",");
    csv_field(o, e.text);
    for (int k = 0; k < 2; ++k) {
        double v = k ? e.new_v : e.old_v;
        out_str(o, ",");
        if (!isfinite(v)) continue;
        if (e.code == LOG_EV_MODE) out_str(o, v != 0.0 ? "REMOTE" : "LOCAL");
        else if (e.code == LOG_EV_TIME_SET) { log_fmt_time(b, sizeof b, (uint32_t)v, true); out_str(o, b); }
        else { fmt_float(b, sizeof b, v); out_str(o, b); }
    }
    out_str(o, "\r\n");
    c->rows++;
}

static void finish(log_csv_t *c, http_out_t *o) {
    uint32_t seq = get32(c->rec + 4);
    c->have = 0;
    if (c->have_seq && seq <= c->last_seq) { c->skipped++; return; }    /* overwritten while reading */
    c->have_seq = true;
    c->last_seq = seq;
    uint8_t type = c->rec[1] & 0x7Fu;
    if (c->ring == LOG_RING_EVENTS) { if (type == LOG_T_EVENT) do_event(c, o); }
    else if (type == LOG_T_SCHEMA) do_schema(c);
    else if (type == LOG_T_DATA) do_data(c, o);
}

void log_csv_feed(log_csv_t *c, const uint8_t *slots, uint32_t n, http_out_t *o) {
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *s = slots + i * LOG_SLOT;
        if (!plat_log_slot_ok(s)) { if (c->have) { c->have = 0; c->skipped++; } continue; }
        uint8_t part = s[2], np = s[3];
        if (np > 17u) { c->skipped++; continue; }
        if (part == 0) {
            if (c->have) c->skipped++;
            memcpy(c->rec, s, LOG_SLOT);
            c->have = 1;
            c->need = np;
        } else if (c->have == part && c->need == np && get32(s + 4) == get32(c->rec + 4)) {
            memcpy(c->rec + part * LOG_SLOT, s, LOG_SLOT);
            c->have++;
        } else {
            if (c->have) c->skipped++;
            c->have = 0;
            continue;
        }
        if (c->have == c->need) finish(c, o);
    }
}
