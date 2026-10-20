/* http_api.c - see http_api.h */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http_api.h"
#include "param.h"
#include "param_store.h"
#include "plat_diag.h"
#include "bsp.h"
#include "log_csv.h"
#include "fw_update.h"
#include "tls.h"
#include "plat_alarm.h"
#include "mail.h"
#include "plat_clock.h"
#include "plat_health.h"
#include "plat_serial.h"

extern uint16_t plat_log_interval_s;

extern const uint8_t  web_index_gz[];      /* web_page.c, generated from web/index.html */
extern const uint32_t web_index_gz_len;

static http_cfg_t cfg;

static size_t blen(const char *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
static uint8_t    blob_buf[4096];          /* one block value at a time (HTTP task only) */

void http_api_init(const http_cfg_t *c) { cfg = *c; }

/* ---------------- parsing ---------------- */
static bool ieq(const char *a, const char *b, size_t n) {
    for (size_t k = 0; k < n; ++k) if (tolower((unsigned char)a[k]) != tolower((unsigned char)b[k])) return false;
    return true;
}

int http_parse_head(const char *buf, uint32_t len, http_req_t *r) {
    const char *end = NULL;
    for (uint32_t k = 0; k + 3 < len; ++k)
        if (buf[k] == '\r' && buf[k + 1] == '\n' && buf[k + 2] == '\r' && buf[k + 3] == '\n') { end = buf + k; break; }
    if (!end) return len > 4096 ? -1 : 0;
    memset(r, 0, sizeof *r);
    /* request line: METHOD SP target SP HTTP/1.x */
    const char *p = buf, *le = memchr(buf, '\r', (size_t)(end - buf + 2));
    const char *sp1 = memchr(p, ' ', (size_t)(le - p));
    if (!sp1 || sp1 - p >= (long)sizeof r->method) return -1;
    memcpy(r->method, p, (size_t)(sp1 - p));
    const char *t = sp1 + 1, *sp2 = memchr(t, ' ', (size_t)(le - t));
    if (!sp2 || le - sp2 < 9 || memcmp(sp2 + 1, "HTTP/1.", 7) != 0) return -1;
    const char *q = memchr(t, '?', (size_t)(sp2 - t));
    const char *pe = q ? q : sp2;
    if (pe - t >= (long)sizeof r->path || pe == t || *t != '/') return -1;
    memcpy(r->path, t, (size_t)(pe - t));
    if (q) { if (sp2 - q - 1 >= (long)sizeof r->query) return -1; memcpy(r->query, q + 1, (size_t)(sp2 - q - 1)); }
    r->keep_alive = sp2[8] == '1';                     /* HTTP/1.1: persistent unless the client says close */
    /* headers */
    for (p = le + 2; p < end;) {
        const char *he = memchr(p, '\r', (size_t)(end - p + 2));
        const char *colon = memchr(p, ':', (size_t)(he - p));
        if (colon) {
            size_t kl = (size_t)(colon - p);
            const char *v = colon + 1; while (v < he && *v == ' ') v++;
            size_t vl = (size_t)(he - v);
            if (kl == 14 && ieq(p, "content-length", 14)) {
                char num[12] = { 0 };
                if (vl == 0 || vl >= sizeof num) return -1;
                memcpy(num, v, vl);
                for (size_t k = 0; k < vl; ++k) if (!isdigit((unsigned char)num[k])) return -1;
                r->content_length = (uint32_t)strtoul(num, NULL, 10); r->has_length = true;
            } else if (kl == 4 && ieq(p, "host", 4)) {
                size_t n = vl < sizeof r->host - 1 ? vl : sizeof r->host - 1;
                memcpy(r->host, v, n); r->host[n] = 0;
            } else if (kl == 10 && ieq(p, "connection", 10)) {
                for (const char *c = v; c + 5 <= he; ++c) {
                    if (ieq(c, "close", 5)) r->keep_alive = false;
                    if (c + 10 <= he && ieq(c, "keep-alive", 10)) r->keep_alive = true;
                }
            } else if (kl == 6 && ieq(p, "x-cems", 6)) {
                r->csrf = vl == 1 && v[0] == '1';
            } else if (kl == 17 && ieq(p, "transfer-encoding", 17)) {
                return -1;                                          /* no chunked uploads */
            } else if (kl == 6 && ieq(p, "cookie", 6)) {
                for (const char *c = v; c + 4 <= he; ++c)
                    if ((c == v || c[-1] == ' ' || c[-1] == ';') && memcmp(c, "sid=", 4) == 0) {
                        const char *s0 = c + 4, *s1 = s0;
                        while (s1 < he && *s1 != ';' && *s1 != ' ') s1++;
                        if (s1 - s0 == AUTH_TOKEN_LEN) memcpy(r->sid, s0, AUTH_TOKEN_LEN);
                        break;
                    }
            }
        }
        p = he + 2;
    }
    return (int)(end - buf + 4);
}

/* ---------------- responses ---------------- */
static const char *reason(int st) {
    switch (st) {
    case 200: return "OK"; case 202: return "Accepted"; case 400: return "Bad Request"; case 401: return "Unauthorized";
    case 403: return "Forbidden"; case 404: return "Not Found"; case 405: return "Method Not Allowed";
    case 409: return "Conflict"; case 413: return "Payload Too Large"; case 429: return "Too Many Requests";
    case 500: return "Internal Server Error"; case 503: return "Service Unavailable"; default: return "Error";
    }
}

static void head(http_out_t *o, int st, const char *ctype, const char *extra) {
    char l[64];
    snprintf(l, sizeof l, "HTTP/1.1 %d %s\r\n", st, reason(st));
    out_str(o, l);
    out_str(o, "Content-Type: "); out_str(o, ctype); out_str(o, "\r\n");
    bool len = extra && strstr(extra, "Content-Length:");
    out_str(o, o->keep ? (len ? "Connection: keep-alive\r\n" : "Connection: keep-alive\r\nTransfer-Encoding: chunked\r\n")
                       : "Connection: close\r\n");
    out_str(o, "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n");
    if (extra) out_str(o, extra);
    out_str(o, "\r\n");
    out_head_done(o, len);
}

static void json_error(http_out_t *o, int st, const char *err, long id) {
    head(o, st, "application/json", NULL);
    out_str(o, "{\"ok\":false,\"error\":"); json_str(o, err);
    if (id >= 0) { out_str(o, ",\"id\":"); json_int(o, id); }
    out_str(o, "}");
}

void http_redirect_https(const http_req_t *r, http_out_t *o) {
    char host[64], loc[200];
    size_t k = 0;
    for (const char *p = r->host; *p && *p != ':' && k + 1 < sizeof host; ++p)    /* drop the port */
        if (isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '[' || *p == ']') host[k++] = *p;
    host[k] = 0;
    if (!k) { http_simple(o, 400, "use https://"); return; }
    snprintf(loc, sizeof loc, "Location: https://%s%s\r\nContent-Length: 0\r\n", host, r->path[0] == '/' ? r->path : "/");
    char l[48];
    snprintf(l, sizeof l, "HTTP/1.1 301 Moved Permanently\r\n");
    out_str(o, l);
    out_str(o, "Cache-Control: no-store\r\nConnection: close\r\n");
    out_str(o, loc);
    out_str(o, "\r\n");
    out_flush(o);
}

void http_simple(http_out_t *o, int st, const char *msg) { json_error(o, st, msg, -1); out_end(o); }

static const char *perr_name(param_err_t e) {
    switch (e) {
    case PERR_NO_ID: return "unknown"; case PERR_READONLY: return "readonly"; case PERR_ACCESS: return "access";
    case PERR_MODE: return "mode"; case PERR_TYPE: return "type"; case PERR_RANGE: return "range";
    case PERR_INTERLOCK: return "interlock"; case PERR_BUSY: return "busy"; default: return "error";
    }
}
static int perr_status(param_err_t e) {
    switch (e) {
    case PERR_NO_ID: return 404; case PERR_READONLY: case PERR_ACCESS: return 403;
    case PERR_MODE: case PERR_INTERLOCK: return 409; case PERR_BUSY: return 503; default: return 400;
    }
}

static const char *ptype_name(const param_def_t *d) {
    static const char *n[] = { "bool", "int", "uint", "float", "bytes", "text", "text16", "array" };
    return d->ptype <= PT_ARRAY ? n[d->ptype] : "?";
}
static const char *ctype_name(uint8_t ct) {
    static const char *n[] = { "bool", "u8", "i8", "u16", "i16", "u32", "i32", "f32", "blob" };
    return ct <= CT_BLOB ? n[ct] : "?";
}
static uint8_t elem_pt(uint8_t ct) {
    return ct == CT_F32 ? PT_FLOAT : ct == CT_BOOL ? PT_BOOL : (ct == CT_I8 || ct == CT_I16 || ct == CT_I32) ? PT_INT : PT_UINT;
}

static void json_val(http_out_t *o, uint8_t pt, plat_val_t v) {
    switch (pt) {
    case PT_BOOL:  json_bool(o, v.b); break;
    case PT_INT:   json_int(o, v.i); break;
    case PT_UINT:  json_uint(o, v.u); break;
    case PT_FLOAT: json_float(o, v.f); break;
    default:       out_str(o, "null");
    }
}

/* ---------------- GET handlers ---------------- */
static void level_name_json(http_out_t *o, uint8_t l) {
    static const char *n[] = { "view", "operator", "admin", "factory" };
    json_str(o, l <= 3 ? n[l] : "view");
}

static void get_info(const http_req_t *r, http_out_t *o, uint32_t now) {
    plat_val_t v;
    uint32_t left = 0;
    uint8_t lvl = auth_session(r->sid, now, &left);
    scan_stats_t ss = { 0 };
    if (cfg.scan) ss = scan_stats(cfg.scan);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"product\":"); json_str(o, cfg.product);
    out_str(o, ",\"fw\":"); json_str(o, cfg.fw_version);
    out_str(o, ",\"serial\":"); json_uint(o, param_read(PARAM_ID_SERIAL, &v) ? v.u : 0);
    out_str(o, ",\"hw_rev\":"); json_uint(o, param_read(PARAM_ID_HW_REV, &v) ? v.u : 0);
    out_str(o, ",\"uptime_s\":"); json_uint(o, now / 1000u);
    out_str(o, ",\"mode\":"); json_str(o, param_ctrl_mode() == PARAM_MODE_REMOTE ? "remote" : "local");
    out_str(o, ",\"link_lost\":"); json_bool(o, param_remote_link_lost());
    out_str(o, ",\"tls_fingerprint\":"); json_str(o, tls_fingerprint());
    { tls_stats_t t = tls_stats();
      out_str(o, ",\"tls\":{\"handshakes\":"); json_uint(o, t.handshakes);
      out_str(o, ",\"resumed\":"); json_uint(o, t.resumed);
      out_str(o, ",\"failed\":"); json_uint(o, t.failed);
      out_str(o, ",\"pool_peak\":"); json_uint(o, t.pool_peak); out_str(o, "}"); }
    out_str(o, ",\"ip_conflict\":"); json_bool(o, (plat_net_status_word & NETST_CONFLICT) != 0);
    out_str(o, ",\"cal_valid\":"); json_bool(o, param_store_calibration_valid());
    { alarm_summary_t s; alarm_summary(&s);
      out_str(o, ",\"alarms\":{\"active\":"); json_uint(o, s.active);
      out_str(o, ",\"unacked\":"); json_uint(o, s.unacked);
      out_str(o, ",\"worst\":"); if (s.worst >= 0) json_str(o, alarm_sev_name((uint8_t)s.worst)); else out_str(o, "null");
      out_str(o, "}"); }
    {   plat_health_t h = plat_health;
        char why[64] = "";
        bool low = plat_health_low(&h, why, sizeof why);
        out_str(o, ",\"health\":{\"known\":"); json_bool(o, h.valid);
        out_str(o, ",\"heap_size\":"); json_uint(o, h.heap_size);
        out_str(o, ",\"heap_free\":"); json_uint(o, h.heap_free);
        out_str(o, ",\"heap_min\":"); json_uint(o, h.heap_min);
        out_str(o, ",\"low\":"); json_bool(o, low);
        out_str(o, ",\"why\":"); json_str(o, why);
        out_str(o, ",\"stacks\":{");
        for (unsigned i = 0; i < h.n_tasks && i < PLAT_HEALTH_TASKS; ++i) {
            if (i) out_str(o, ",");
            h.task[i].name[sizeof h.task[i].name - 1] = 0;
            json_str(o, h.task[i].name); out_str(o, ":"); json_uint(o, h.task[i].free_words);
        }
        out_str(o, "}}"); }
    out_str(o, ",\"store_status\":"); json_uint(o, param_store_status());
    out_str(o, ",\"log_status\":"); json_uint(o, plat_log_status());
    out_str(o, ",\"default_passwords\":"); json_bool(o, auth_any_default());
    out_str(o, ",\"login_locked_s\":"); json_uint(o, (auth_locked_ms(now) + 999u) / 1000u);
    out_str(o, ",\"session\":{\"level\":"); level_name_json(o, lvl);
    out_str(o, ",\"write_s_left\":"); json_uint(o, left / 1000u); out_str(o, "}");
    out_str(o, ",\"scan\":{\"scans\":"); json_uint(o, ss.scans);
    out_str(o, ",\"max_exec_us\":"); json_uint(o, ss.max_exec_us);
    out_str(o, ",\"overruns\":"); json_uint(o, ss.overruns);
    out_str(o, ",\"late_starts\":"); json_uint(o, ss.late_starts); out_str(o, "}");
    out_str(o, ",\"last_fault\":{\"code\":"); json_uint(o, plat_diag.fault);
    out_str(o, ",\"name\":"); json_strn(o, plat_diag.fault_name, blen(plat_diag.fault_name, sizeof plat_diag.fault_name));
    out_str(o, ",\"boots\":"); json_uint(o, plat_diag.boots); out_str(o, "}}");
}

static void get_params(http_out_t *o) {
    head(o, 200, "application/json", NULL);
    out_str(o, "[");
    for (size_t i = 0; i < param_count(); ++i) {
        const param_def_t *d = param_at(i);
        if (i) out_str(o, ",");
        out_str(o, "{\"id\":"); json_uint(o, d->id);
        out_str(o, ",\"name\":"); json_str(o, d->name);
        out_str(o, ",\"type\":"); json_str(o, ptype_name(d));
        if (d->unit) { out_str(o, ",\"unit\":"); json_str(o, d->unit); }
        uint8_t pt = d->ptype;
        if (pt == PT_ARRAY) { out_str(o, ",\"elem\":"); json_str(o, ctype_name(d->ctype)); pt = elem_pt(d->ctype); }
        if (pt <= PT_FLOAT && pt != PT_BOOL) {
            out_str(o, ",\"min\":"); json_val(o, pt, d->vmin);
            out_str(o, ",\"max\":"); json_val(o, pt, d->vmax);
        }
        if (d->ptype <= PT_FLOAT) { out_str(o, ",\"def\":"); json_val(o, d->ptype, d->vdef); }
        if (PARAM_IS_BLOB(d)) { out_str(o, ",\"size\":"); json_uint(o, d->size); }
        out_str(o, ",\"level\":"); json_uint(o, d->level);
        out_str(o, ",\"flags\":["); bool f = false;
        static const struct { uint16_t bit; const char *n; } fl[] = {
            { PF_RO, "ro" }, { PF_PERSIST, "persist" }, { PF_FACTORY, "factory" }, { PF_ANY_MODE, "any_mode" },
            { PF_LOCAL_ONLY, "local_only" }, { PF_NO_MODBUS, "no_modbus" }, { PF_CONFIRM, "confirm" }, { PF_SECRET, "secret" },
            { PF_DEV, "ro" }, { PF_DEV, "dev" } };
        for (size_t k = 0; k < sizeof fl / sizeof fl[0]; ++k)
            if (d->flags & fl[k].bit) { if (f) out_str(o, ","); json_str(o, fl[k].n); f = true; }
        out_str(o, "]}");
    }
    out_str(o, "]");
}

static void value_json(http_out_t *o, const param_def_t *d) {
    if (!PARAM_IS_BLOB(d)) {
        plat_val_t v;
        if (param_read(d->id, &v)) json_val(o, d->ptype, v); else out_str(o, "null");
        return;
    }
    if (d->size > sizeof blob_buf || param_read_blob(d->id, 0, blob_buf, d->size) != PERR_OK) { out_str(o, "null"); return; }
    switch (d->ptype) {
    case PT_TEXT:  json_strn(o, (const char *)blob_buf, blen((const char *)blob_buf, d->size)); break;
    case PT_TEXT16: {
        static char u8[3 * sizeof blob_buf / 2 + 1];
        if (utf16le_to_utf8(blob_buf, d->size / 2u, u8, sizeof u8) < 0) out_str(o, "null"); else json_str(o, u8);
        break;
    }
    case PT_BYTES: {
        out_str(o, "\"");
        char h[65];
        for (uint16_t k = 0; k < d->size; k += 32) {
            uint16_t n = (uint16_t)(d->size - k < 32 ? d->size - k : 32);
            hex_encode(blob_buf + k, n, h); out_str(o, h);
        }
        out_str(o, "\"");
        break;
    }
    case PT_ARRAY: {
        size_t es = param_ctype_size(d->ctype);
        param_def_t t = *d;
        t.ptype = elem_pt(d->ctype);
        out_str(o, "[");
        for (size_t k = 0; k < d->size / es; ++k) {
            if (k) out_str(o, ",");
            if (d->ctype == CT_BOOL) { json_bool(o, blob_buf[k] != 0); continue; }
            t.ptr = blob_buf + k * es;
            json_val(o, t.ptype, param_tag_get(&t));
        }
        out_str(o, "]");
        break;
    }
    default: out_str(o, "null");
    }
}

static void get_values(const http_req_t *r, http_out_t *o, uint32_t now) {
    char ids[sizeof r->query];
    bool filter = form_get(r->query, strlen(r->query), "ids", ids, sizeof ids);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"t\":"); json_uint(o, now);
    out_str(o, ",\"v\":{");
    bool first = true;
    if (filter) {
        for (char *p = ids; *p;) {
            char *e; unsigned long id = strtoul(p, &e, 10);
            if (e == p) break;
            const param_def_t *d = id <= 0xFFFFu ? param_find((uint16_t)id) : NULL;
            if (d) { if (!first) out_str(o, ","); first = false;
                     out_str(o, "\""); json_uint(o, id); out_str(o, "\":"); value_json(o, d); }
            p = *e == ',' ? e + 1 : e;
            if (*e != ',') break;
        }
    } else {
        for (size_t i = 0; i < param_count(); ++i) {
            const param_def_t *d = param_at(i);
            if (!first) out_str(o, ",");
            first = false;
            out_str(o, "\""); json_uint(o, d->id); out_str(o, "\":"); value_json(o, d);
        }
    }
    out_str(o, "}}");
}

/* ---------------- writes ---------------- */
static bool parse_ipv4(const char *s, uint32_t *out) {
    unsigned long p[4]; char *e = (char *)(uintptr_t)s;
    for (int k = 0; k < 4; ++k) {
        if (!isdigit((unsigned char)*e)) return false;
        p[k] = strtoul(e, &e, 10);
        if (p[k] > 255 || (k < 3 && *e++ != '.')) return false;
    }
    if (*e) return false;
    *out = (uint32_t)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
    return true;
}

/* a text value -> (kind, value) for a scalar entry of type pt */
static bool parse_scalar(uint8_t pt, const char *s, uint8_t *kind, plat_val_t *v) {
    char *e;
    if (!*s) return false;
    switch (pt) {
    case PT_BOOL:
        if (!strcmp(s, "true") || !strcmp(s, "1")) { *kind = PLAT_VAL_BOOL; v->b = true; return true; }
        if (!strcmp(s, "false") || !strcmp(s, "0")) { *kind = PLAT_VAL_BOOL; v->b = false; return true; }
        return false;
    case PT_INT: {
        long x = strtol(s, &e, 10);
        if (*e) return false;
        *kind = PLAT_VAL_I32; v->i = (int32_t)x;
        return x >= -2147483647L - 1 && x <= 2147483647L;
    }
    case PT_UINT: {
        if (strchr(s, '.')) { *kind = PLAT_VAL_U32; return parse_ipv4(s, &v->u); }
        if (*s == '-') return false;
        unsigned long x = strtoul(s, &e, 10);
        if (*e || x > 4294967295UL) return false;
        *kind = PLAT_VAL_U32; v->u = (uint32_t)x;
        return true;
    }
    case PT_FLOAT: {
        float f = strtof(s, &e);
        if (*e || !isfinite(f)) return false;
        *kind = PLAT_VAL_F32; v->f = f;
        return true;
    }
    default: return false;
    }
}

/* an array element value -> bytes */
static bool parse_elem(const param_def_t *d, const char *s, uint8_t *out) {
    uint8_t kind; plat_val_t v;
    uint8_t pt = elem_pt(d->ctype);
    if (!parse_scalar(pt, s, &kind, &v)) return false;
    union { uint8_t b[4]; uint8_t u8; int8_t i8; uint16_t u16; int16_t i16; uint32_t u32; int32_t i32; float f; } t = { { 0 } };
    switch (d->ctype) {
    case CT_BOOL: t.u8 = v.b ? 1 : 0; break;
    case CT_U8:  if (v.u > 255u) return false; t.u8 = (uint8_t)v.u; break;
    case CT_I8:  if (v.i < -128 || v.i > 127) return false; t.i8 = (int8_t)v.i; break;
    case CT_U16: if (v.u > 65535u) return false; t.u16 = (uint16_t)v.u; break;
    case CT_I16: if (v.i < -32768 || v.i > 32767) return false; t.i16 = (int16_t)v.i; break;
    case CT_U32: t.u32 = v.u; break;
    case CT_I32: t.i32 = v.i; break;
    case CT_F32: t.f = v.f; break;
    default: return false;
    }
    memcpy(out, t.b, param_ctype_size(d->ctype));
    return true;
}

#define MAX_WRITES 32
static plat_cmd_t         w_cmds[MAX_WRITES];
static param_blob_write_t w_blobs[MAX_WRITES];
static uint8_t            w_data[MAX_WRITES][PARAM_BLOB_MAX_WRITE];

static void post_write(const http_req_t *r, http_out_t *o, uint8_t lvl, uint32_t now) {
    form_iter_t it;
    char key[16];
    static char val[512];
    size_t ns = 0, nb = 0;
    long cur_id = -1, cur_off = 0;
    bool have_v = false;
    form_begin(&it, (const char *)r->body, r->body_len);
    for (;;) {
        bool more = form_next(&it, key, sizeof key, val, sizeof val);
        if (more && key[0] == 0) { json_error(o, 400, "form", -1); return; }
        bool flush = !more || !strcmp(key, "id");
        if (flush && cur_id >= 0) {
            json_error(o, 400, "no value", cur_id); return;          /* an id without a v */
        }
        if (!more) break;
        if (!strcmp(key, "id")) { char *e; cur_id = strtol(val, &e, 10); if (*e || cur_id < 1 || cur_id > 65535) { json_error(o, 400, "id", -1); return; } cur_off = 0; have_v = false; continue; }
        if (!strcmp(key, "o"))  { char *e; cur_off = strtol(val, &e, 10); if (*e || cur_off < 0 || cur_off > 65535) { json_error(o, 400, "offset", cur_id); return; } continue; }
        if (strcmp(key, "v") != 0 || cur_id < 0 || have_v) { json_error(o, 400, "form", cur_id); return; }
        have_v = true;
        const param_def_t *d = param_find((uint16_t)cur_id);
        if (!d) { json_error(o, 404, "unknown", cur_id); return; }
        if (ns + nb >= MAX_WRITES) { json_error(o, 413, "too many", -1); return; }
        if (!PARAM_IS_BLOB(d)) {
            plat_cmd_t *c = &w_cmds[ns];
            memset(c, 0, sizeof *c);
            c->id = d->id; c->src = PLAT_SRC_WEB; c->level = lvl;
            if (!parse_scalar(d->ptype, val, &c->kind, &c->v)) { json_error(o, 400, "value", cur_id); return; }
            ns++;
        } else {
            uint8_t *buf = w_data[nb];
            int n = -1; bool whole = false; uint32_t off = 0;
            switch (d->ptype) {
            case PT_TEXT:   n = (int)strlen(val); if (n > (int)PARAM_BLOB_MAX_WRITE) n = -1; else memcpy(buf, val, (size_t)n); whole = true; break;
            case PT_TEXT16: n = utf8_to_utf16le(val, buf, PARAM_BLOB_MAX_WRITE); whole = true; break;
            case PT_BYTES:  n = hex_decode(val, buf, PARAM_BLOB_MAX_WRITE); off = (uint32_t)cur_off; break;
            case PT_ARRAY: {
                size_t es = param_ctype_size(d->ctype); n = 0;
                for (char *p = val;;) {
                    char *comma = strchr(p, ',');
                    if (comma) *comma = 0;
                    if ((size_t)n + es > PARAM_BLOB_MAX_WRITE || !parse_elem(d, p, buf + n)) { n = -1; break; }
                    n += (int)es;
                    if (!comma) break;
                    p = comma + 1;
                }
                off = (uint32_t)cur_off * (uint32_t)es;
                break;
            }
            default: break;
            }
            if (n < 0 || off > 65535u) { json_error(o, 400, "value", cur_id); return; }
            w_blobs[nb] = (param_blob_write_t){ .id = d->id, .off = (uint16_t)off, .len = (uint16_t)n, .whole = whole, .data = buf };
            nb++;
        }
        cur_id = -1;
    }
    if (ns + nb == 0) { json_error(o, 400, "nothing", -1); return; }
    /* check everything, then queue */
    for (size_t k = 0; k < ns; ++k) {
        param_err_t e = param_check(w_cmds[k].id, w_cmds[k].kind, w_cmds[k].v, PLAT_SRC_WEB, lvl);
        if (e != PERR_OK) { json_error(o, perr_status(e), perr_name(e), w_cmds[k].id); return; }
    }
    for (size_t k = 0; k < nb; ++k) {
        param_err_t e = param_blob_check(w_blobs[k].id, w_blobs[k].off, w_blobs[k].data, w_blobs[k].len,
                                         w_blobs[k].whole, PLAT_SRC_WEB, lvl);
        if (e != PERR_OK) { json_error(o, perr_status(e), perr_name(e), w_blobs[k].id); return; }
    }
    param_err_t e = param_post_many(w_cmds, ns);
    if (e == PERR_OK) e = param_blob_post_many(w_blobs, nb, PLAT_SRC_WEB, lvl);
    if (e != PERR_OK) { json_error(o, perr_status(e), perr_name(e), -1); return; }
    auth_touch(r->sid, now);
    param_comms_activity(PLAT_SRC_WEB, now);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true,\"queued\":"); json_uint(o, (unsigned long)(ns + nb)); out_str(o, "}");
}

static uint8_t parse_level(const char *s) {
    if (!strcmp(s, "operator") || !strcmp(s, "1")) return PLAT_LVL_OPERATOR;
    if (!strcmp(s, "admin") || !strcmp(s, "2")) return PLAT_LVL_ADMIN;
    if (!strcmp(s, "factory") || !strcmp(s, "3")) return PLAT_LVL_FACTORY;
    return 0;
}

static void post_login(const http_req_t *r, http_out_t *o, uint32_t now) {
    char lv[16], pw[AUTH_PW_MAX + 2];
    form_get((const char *)r->body, r->body_len, "level", lv, sizeof lv);
    form_get((const char *)r->body, r->body_len, "password", pw, sizeof pw);
    uint8_t level = parse_level(lv);
    if (!level) { json_error(o, 400, "level", -1); return; }
    char tok[AUTH_TOKEN_LEN + 1];
    auth_rc_t rc = auth_login(level, pw, now, tok);
    memset(pw, 0, sizeof pw);
    if (rc == AUTH_LOCKED) { plat_log_event(LOG_EV_LOGIN_FAIL, PLAT_SRC_WEB, level, 0, NAN, NAN, "locked"); json_error(o, 429, "locked", -1); return; }
    if (rc != AUTH_OK) { plat_log_event(LOG_EV_LOGIN_FAIL, PLAT_SRC_WEB, level, 0, NAN, NAN, NULL); json_error(o, 401, "password", -1); return; }
    plat_log_event(LOG_EV_LOGIN, PLAT_SRC_WEB, level, 0, NAN, NAN, NULL);
    char cookie[96];
    snprintf(cookie, sizeof cookie, "Set-Cookie: sid=%s; Path=/; HttpOnly; SameSite=Strict\r\n", tok);
    head(o, 200, "application/json", cookie);
    out_str(o, "{\"ok\":true,\"level\":"); level_name_json(o, level);
    out_str(o, ",\"default_password\":"); json_bool(o, auth_is_default(level)); out_str(o, "}");
}

static void post_password(const http_req_t *r, http_out_t *o, uint8_t lvl, uint32_t now) {
    char lv[16], pw[AUTH_PW_MAX + 2];
    form_get((const char *)r->body, r->body_len, "level", lv, sizeof lv);
    form_get((const char *)r->body, r->body_len, "password", pw, sizeof pw);
    uint8_t target = parse_level(lv);
    if (!target) { json_error(o, 400, "level", -1); return; }
    if (lvl < target) { json_error(o, 403, "access", -1); return; }
    auth_rc_t rc = auth_set_password(target, pw);
    memset(pw, 0, sizeof pw);
    if (rc != AUTH_OK) { json_error(o, 400, "password rules", -1); return; }
    plat_log_event(LOG_EV_PASSWORD, PLAT_SRC_WEB, lvl, target, NAN, NAN, NULL);
    auth_touch(r->sid, now);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true}");
}

static bool wait_job(int32_t *result) {
    for (int k = 0; k < 500; ++k) {                       /* up to ~5 s */
        if (param_store_job_done(result)) return true;
        if (cfg.wait) cfg.wait();
    }
    return false;
}

static void get_backup(http_out_t *o, uint8_t lvl) {
    if (lvl < PLAT_LVL_ADMIN) { json_error(o, 403, "access", -1); return; }
    if (!cfg.scratch || param_store_job_start(true, cfg.scratch, cfg.scratch_size, lvl) != 0) { json_error(o, 503, "busy", -1); return; }
    int32_t n;
    if (!wait_job(&n)) { json_error(o, 503, "timeout", -1); return; }  /* (a late job finishes harmlessly) */
    if (n <= 0) { json_error(o, 500, "nothing stored", -1); return; }
    plat_val_t s;
    char extra[160];
    snprintf(extra, sizeof extra, "Content-Length: %ld\r\nContent-Disposition: attachment; filename=\"cems-%lu.bkp\"\r\n",
             (long)n, (unsigned long)(param_read(PARAM_ID_SERIAL, &s) ? s.u : 0));
    head(o, 200, "application/octet-stream", extra);
    out_bytes(o, cfg.scratch, (size_t)n);
}

static void post_restore(const http_req_t *r, http_out_t *o, uint8_t lvl) {
    if (lvl < PLAT_LVL_ADMIN) { json_error(o, 403, "access", -1); return; }
    if (param_store_job_start(false, (uint8_t *)(uintptr_t)r->body, r->body_len, lvl) != 0) { json_error(o, 503, "busy", -1); return; }
    int32_t rc;
    if (!wait_job(&rc)) { json_error(o, 503, "timeout", -1); return; }
    if (rc != 0) { json_error(o, 400, rc == -5 ? "nothing to restore at this level" : "invalid file", -1); return; }
    plat_log_event(LOG_EV_RESTORE_BACKUP, PLAT_SRC_WEB, lvl, 0, NAN, NAN, NULL);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true}");
}

/* ---------------- logs ---------------- */
static log_csv_t csv;        /* HTTP task only */

static bool wait_log(int32_t *result) {
    for (int k = 0; k < 500; ++k) {
        if (plat_log_read_done(result)) return true;
        if (cfg.wait) cfg.wait();
    }
    return false;
}

/* everything queued before this request is in the flash (so a write just made is listed) */
static bool log_sync(void) {
    int32_t r;
    return plat_log_read_start(LOG_RING_EVENTS, 0, 0, NULL) == 0 && wait_log(&r);
}

static uint32_t query_u32(const http_req_t *r, const char *key, uint32_t dflt) {
    char v[16];
    if (!form_get(r->query, strlen(r->query), key, v, sizeof v) || !v[0]) return dflt;
    for (const char *p = v; *p; ++p) if (!isdigit((unsigned char)*p)) return dflt;
    return (uint32_t)strtoul(v, NULL, 10);
}

static void get_log_info(http_out_t *o) {
    (void)log_sync();
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"status\":"); json_uint(o, plat_log_status());
    out_str(o, ",\"interval_s\":"); json_uint(o, plat_log_interval_s);
    static const char *const nm[LOG_RINGS] = { "events", "data" };
    for (uint8_t k = 0; k < LOG_RINGS; ++k) {
        log_ring_info_t inf;
        out_str(o, ",\""); out_str(o, nm[k]); out_str(o, "\":");
        if (!plat_log_info(k, &inf)) { out_str(o, "null"); continue; }
        out_str(o, "{\"slots\":"); json_uint(o, inf.nslots);
        out_str(o, ",\"used\":"); json_uint(o, inf.used);
        out_str(o, ",\"next_seq\":"); json_uint(o, inf.next_seq); out_str(o, "}");
    }
    out_str(o, "}");
}

static void get_log_csv(const http_req_t *r, http_out_t *o, uint8_t ring) {
    log_ring_info_t inf;
    uint32_t chunk = cfg.scratch_size / LOG_SLOT;
    if (!cfg.scratch || chunk == 0 || !log_sync() || !plat_log_info(ring, &inf)) { json_error(o, 503, "log not available", -1); return; }
    plat_val_t s;
    char extra[128];
    snprintf(extra, sizeof extra, "Content-Disposition: attachment; filename=\"cems-%lu-%s.csv\"\r\n",
             (unsigned long)(param_read(PARAM_ID_SERIAL, &s) ? s.u : 0), ring == LOG_RING_DATA ? "data" : "events");
    head(o, 200, "text/csv; charset=utf-8", extra);
    log_csv_begin(&csv, ring, cfg.log_chans, cfg.n_log_chans, plat_log_interval_s,
                  query_u32(r, "from", 0), query_u32(r, "to", 0), o);
    uint32_t pos = inf.oldest, left = inf.used;
    while (left && !o->err) {
        uint32_t n = left < chunk ? left : chunk;
        int32_t got = -1;
        if (plat_log_read_start(ring, pos, n, cfg.scratch) != 0 || !wait_log(&got) || got != (int32_t)n) {
            out_str(o, "\r\n# read error\r\n");
            break;
        }
        log_csv_feed(&csv, cfg.scratch, n, o);
        pos = (pos + n) % inf.nslots;
        left -= n;
    }
}

/* an event's old/new value: whole numbers exactly (a 32-bit serial or IP), others to 7 digits */
static void ev_num(http_out_t *o, double v) {
    if (isfinite(v) && v == floor(v) && fabs(v) <= 4294967295.0) {
        char b[16];
        snprintf(b, sizeof b, "%s%lu", v < 0 ? "-" : "", (unsigned long)fabs(v));
        out_str(o, b);
    } else json_float(o, (float)v);
}

static void get_log_events(const http_req_t *r, http_out_t *o) {
    log_ring_info_t inf;
    uint32_t n = query_u32(r, "n", 50);
    if (!cfg.scratch || !log_sync() || !plat_log_info(LOG_RING_EVENTS, &inf)) { json_error(o, 503, "log not available", -1); return; }
    if (n > 200u) n = 200u;
    if (n > inf.used) n = inf.used;
    if (n > cfg.scratch_size / LOG_SLOT) n = cfg.scratch_size / LOG_SLOT;
    int32_t got = 0;
    if (n && (plat_log_read_start(LOG_RING_EVENTS, (inf.wr + inf.nslots - n) % inf.nslots, n, cfg.scratch) != 0 ||
              !wait_log(&got) || got != (int32_t)n)) { json_error(o, 503, "busy", -1); return; }
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"events\":[");
    bool first = true;
    for (uint32_t k = n; k-- > 0;) {
        log_event_t e;
        if (!plat_log_decode_event(cfg.scratch + k * LOG_SLOT, &e)) continue;
        if (!first) out_str(o, ",");
        first = false;
        char tb[32];
        log_fmt_time(tb, sizeof tb, e.time, e.clock_set);
        out_str(o, "{\"seq\":"); json_uint(o, e.seq);
        out_str(o, ",\"time\":"); json_str(o, tb);
        out_str(o, ",\"t\":"); json_uint(o, e.time);
        out_str(o, ",\"code\":"); json_uint(o, e.code);
        out_str(o, ",\"event\":"); json_str(o, plat_log_event_name(e.code));
        out_str(o, ",\"source\":"); json_str(o, log_src_name(e.src));
        out_str(o, ",\"level\":"); json_str(o, log_level_name(e.level));
        out_str(o, ",\"arg\":"); json_uint(o, e.arg);
        out_str(o, ",\"detail\":"); json_str(o, e.text);
        out_str(o, ",\"old\":"); ev_num(o, e.old_v);
        out_str(o, ",\"new\":"); ev_num(o, e.new_v);
        out_str(o, "}");
    }
    out_str(o, "]}");
}

/* the unit's certificate, to import on the site's laptops so browsers stop warning */
static void get_cert(http_out_t *o) {
    size_t n;
    const uint8_t *c = tls_cert(&n);
    if (!n) { json_error(o, 503, "no certificate", -1); return; }
    plat_val_t s;
    char extra[128];
    snprintf(extra, sizeof extra, "Content-Length: %lu\r\nContent-Disposition: attachment; filename=\"cems-%lu.crt\"\r\n",
             (unsigned long)n, (unsigned long)(param_read(PARAM_ID_SERIAL, &s) ? s.u : 0));
    head(o, 200, "application/x-x509-ca-cert", extra);
    out_bytes(o, c, n);
}

/* ---------------- firmware update ---------------- */
static const char *fw_state_name(fw_state_t s) {
    switch (s) { case FW_ST_PENDING: return "pending"; case FW_ST_TRIAL: return "trial";
                 case FW_ST_ROLLED_BACK: return "rolled_back"; default: return "idle"; }
}

static void get_fw(http_out_t *o) {
    fw_status_t st;
    fw_status(&st);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"running\":"); json_str(o, st.running_version);
    out_str(o, ",\"state\":"); json_str(o, fw_state_name(st.state));
    out_str(o, ",\"trial_left_s\":"); json_uint(o, (st.trial_left_ms + 999u) / 1000u);
    out_str(o, ",\"staged\":"); if (st.staged) json_str(o, st.staged_version); else out_str(o, "null");
    out_str(o, ",\"uploading\":"); json_bool(o, st.uploading);
    out_str(o, ",\"received\":"); json_uint(o, st.upload_bytes);
    out_str(o, ",\"total\":"); json_uint(o, st.upload_total);
    out_str(o, ",\"rolled_back\":"); json_bool(o, st.rolled_back);
    out_str(o, ",\"signing_required\":"); json_bool(o, st.signing_required);
    out_str(o, ",\"waiting_local\":"); json_bool(o, st.wait_local);
    out_str(o, ",\"waiting_left_s\":"); json_uint(o, (st.wait_left_ms + 999u) / 1000u);
    out_str(o, ",\"not_safe\":"); json_str(o, st.not_safe);
    { plat_val_t v; out_str(o, ",\"min_version\":"); json_uint(o, param_read(PARAM_ID_FW_MIN_VERSION, &v) ? v.u : 0); }
    out_str(o, ",\"last_error\":"); json_str(o, fw_err_name(st.last_error)); out_str(o, "}");
}

/* ---------------- alarms ---------------- */
static void get_alarms(http_out_t *o) {
    alarm_summary_t s;
    alarm_summary(&s);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"active\":"); json_uint(o, s.active);
    out_str(o, ",\"unacked\":"); json_uint(o, s.unacked);
    out_str(o, ",\"worst\":"); if (s.worst >= 0) json_str(o, alarm_sev_name((uint8_t)s.worst)); else out_str(o, "null");
    out_str(o, ",\"alarms\":[");
    uint64_t pos = 0;
    alarm_view_t v;
    for (bool first = true; alarm_next(&pos, &v); first = false) {
        if (!first) out_str(o, ",");
        out_str(o, "{\"id\":"); json_uint(o, v.id);
        out_str(o, ",\"name\":"); json_str(o, v.name);
        out_str(o, ",\"text\":"); json_str(o, v.text);
        out_str(o, ",\"severity\":"); json_str(o, alarm_sev_name(v.severity));
        out_str(o, ",\"active\":"); json_bool(o, v.active);
        out_str(o, ",\"unacked\":"); json_bool(o, v.unacked);
        out_str(o, ",\"latch\":"); json_bool(o, (v.flags & AL_LATCH) != 0);
        out_str(o, ",\"time\":"); json_uint(o, v.t_raise);
        { char t[32]; log_fmt_time(t, sizeof t, v.t_raise ? v.t_raise : v.up_raise, v.t_raise != 0);
          out_str(o, ",\"since\":"); json_str(o, t); }
        out_str(o, ",\"uptime\":"); json_uint(o, v.up_raise);
        out_str(o, "}");
    }
    out_str(o, "]}");
}

/* every defined alarm with its actions, and the relays: the "Alarm actions" form */
static void get_alarm_config(http_out_t *o) {
    uint8_t route[256], rdo[ALARM_RELAYS], rmode[ALARM_RELAYS], rinv[ALARM_RELAYS];
    if (param_read_blob(PARAM_ID_ALARM_ROUTE, 0, route, sizeof route) != PERR_OK ||
        param_read_blob(PARAM_ID_ALARM_RELAY_DO, 0, rdo, sizeof rdo) != PERR_OK ||
        param_read_blob(PARAM_ID_ALARM_RELAY_MODE, 0, rmode, sizeof rmode) != PERR_OK ||
        param_read_blob(PARAM_ID_ALARM_RELAY_INV, 0, rinv, sizeof rinv) != PERR_OK) { json_error(o, 503, "busy", -1); return; }
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"alarms\":[");
    for (size_t i = 0; i < alarm_count(); ++i) {
        const alarm_def_t *d = alarm_def_at(i);
        alarm_view_t v;
        (void)alarm_get(d->id, &v);
        if (i) out_str(o, ",");
        out_str(o, "{\"id\":"); json_uint(o, d->id);
        out_str(o, ",\"name\":"); json_str(o, d->name);
        out_str(o, ",\"text\":"); json_str(o, v.text);
        out_str(o, ",\"severity\":"); json_str(o, alarm_sev_name(d->severity));
        out_str(o, ",\"route\":"); json_uint(o, alarm_route(d->id));
        out_str(o, ",\"default\":"); json_uint(o, d->route);
        out_str(o, ",\"configured\":"); json_bool(o, (route[d->id] & AR_SET) != 0);
        out_str(o, "}");
    }
    out_str(o, "],\"relays\":[");
    uint8_t on = alarm_relays();
    for (unsigned k = 0; k < ALARM_RELAYS; ++k) {
        if (k) out_str(o, ",");
        out_str(o, "{\"do\":"); json_uint(o, rdo[k]);
        out_str(o, ",\"mode\":"); json_uint(o, rmode[k]);
        out_str(o, ",\"failsafe\":"); json_bool(o, rinv[k] != 0);
        out_str(o, ",\"in_alarm\":"); json_bool(o, (on >> k) & 1u);
        out_str(o, "}");
    }
    out_str(o, "]}");
}

/* id=<n> or id=all. Through the parameter table like the touchscreen and Modbus, so
 * the acknowledge is logged with this login and source. */
static void post_alarm_ack(const http_req_t *r, http_out_t *o, uint8_t lvl) {
    char v[8];
    unsigned long id = ALARM_ACK_ALL;
    if (!form_get((const char *)r->body, r->body_len, "id", v, sizeof v)) { json_error(o, 400, "id", -1); return; }
    if (strcmp(v, "all")) {
        char *e; id = strtoul(v, &e, 10);
        alarm_view_t a;
        if (*e || !v[0] || id > ALARM_ID_MAX || !alarm_get((uint16_t)id, &a)) { json_error(o, 404, "no such alarm", -1); return; }
    }
    param_err_t pe = param_post(PARAM_ID_ALARM_ACK, PLAT_VAL_U32, (plat_val_t){ .u = (uint32_t)id }, PLAT_SRC_WEB, lvl);
    if (pe != PERR_OK) { json_error(o, perr_status(pe), perr_name(pe), -1); return; }
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true}");
}

/* ---------------- clock ---------------- */
static void get_time(http_out_t *o) {
    uint32_t t;
    bool set = plat_clock_now(&t);
    char b[32];
    log_fmt_time(b, sizeof b, t, true);
    sntp_status_t s;
    sntp_status(&s);
    plat_val_t v;
    uint32_t ip = param_read(PARAM_ID_SNTP_SERVER, &v) ? v.u : 0;
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"set\":"); json_bool(o, set);
    out_str(o, ",\"unix\":"); json_uint(o, t);
    out_str(o, ",\"text\":"); json_str(o, set ? b : "");
    out_str(o, ",\"utc_offset_min\":"); json_int(o, plat_clock_utc_offset_s() / 60);
    out_str(o, ",\"sntp\":{\"on\":"); json_bool(o, ip != 0);
    out_str(o, ",\"last_ok_s_ago\":"); if (s.last_ok_ms) json_uint(o, (bsp_ms() - s.last_ok_ms) / 1000u); else out_str(o, "null");
    out_str(o, ",\"diff_s\":"); json_int(o, s.last_diff_s);
    out_str(o, ",\"stratum\":"); json_uint(o, s.stratum);
    out_str(o, ",\"steps\":"); json_uint(o, s.steps);
    out_str(o, ",\"tries\":"); json_uint(o, s.tries);
    out_str(o, ",\"error\":"); json_str(o, s.error);
    out_str(o, "}}");
}

/* t=<local seconds since 1970> sets the clock (admin); sync=1 asks the time server now */
static void post_time(const http_req_t *r, http_out_t *o, uint8_t lvl) {
    if (lvl < PLAT_LVL_ADMIN) { json_error(o, 403, "access", -1); return; }
    char v[16] = "";
    if (form_get((const char *)r->body, r->body_len, "sync", v, sizeof v)) {
        plat_val_t pv;
        if (!param_read(PARAM_ID_SNTP_SERVER, &pv) || !pv.u) { json_error(o, 409, "no time server set", -1); return; }
        sntp_request_now();
    } else {
        if (!form_get((const char *)r->body, r->body_len, "t", v, sizeof v)) { json_error(o, 400, "t", -1); return; }
        char *e; unsigned long t = strtoul(v, &e, 10);
        if (*e || !v[0] || plat_clock_request((uint32_t)t, PLAT_SRC_WEB, lvl, "web") != 0) { json_error(o, 400, "range", -1); return; }
    }
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true}");
}

/* ---------------- serial number (developer authority, D25) ---------------- */
static bool parse_u32(const char *v, uint32_t *out) {
    char *e;
    unsigned long n = strtoul(v, &e, 10);
    if (!v[0] || *e || v[0] == '-' || n > 0xFFFFFFFFul) return false;
    *out = (uint32_t)n;
    return true;
}

/* ?new=<serial>: also the exact text the developer signs for that change */
static void get_serial(const http_req_t *r, http_out_t *o) {
    char uid[25], v[16], msg[SERIAL_MSG_MAX];
    plat_serial_uid_hex(uid);
    uint32_t want = 0;
    bool asked = form_get(r->query, strlen(r->query), "new", v, sizeof v) && parse_u32(v, &want);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"serial\":"); json_uint(o, plat_serial());
    out_str(o, ",\"uid\":"); json_str(o, uid);
    out_str(o, ",\"key\":"); json_bool(o, plat_serial_key_present());
    if (asked && plat_serial_message(want, msg, sizeof msg)) {
        out_str(o, ",\"new\":"); json_uint(o, want);
        out_str(o, ",\"message\":"); json_str(o, msg);
    }
    out_str(o, "}");
}

/* serial=<new>&sig=<128 hex digits: r||s>; factory login and a developer token */
static void post_serial(const http_req_t *r, http_out_t *o, uint8_t lvl) {
    char v[16] = "", sh[160] = "";
    uint8_t sig[64];
    uint32_t want;
    if (!form_get((const char *)r->body, r->body_len, "serial", v, sizeof v) || !parse_u32(v, &want)) { json_error(o, 400, "serial", -1); return; }
    if (!form_get((const char *)r->body, r->body_len, "sig", sh, sizeof sh) || strlen(sh) != 128 || hex_decode(sh, sig, sizeof sig) != 64) {
        json_error(o, 400, "token", -1); return;
    }
    int rc = plat_serial_assign(want, sig, PLAT_SRC_WEB, lvl);
    if (rc != SER_OK) {
        int st = rc == SER_E_LEVEL ? 403 : rc == SER_E_SIG ? 403 : rc == SER_E_BUSY ? 503 : 409;
        json_error(o, st, plat_serial_err(rc), PARAM_ID_SERIAL); return;
    }
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true,\"serial\":"); json_uint(o, want); out_str(o, "}");
}

/* ---------------- alarm email ---------------- */
static void hex32(http_out_t *o, const uint8_t *b) {
    char h[96];
    for (int k = 0; k < 32; ++k) snprintf(h + 3 * k, 4, "%02X%s", b[k], k < 31 ? ":" : "");
    json_str(o, h);
}

static void get_mail(http_out_t *o) {
    mail_status_t s;
    mail_status(&s);
    uint8_t pin[32] = { 0 };
    (void)param_read_blob(PARAM_ID_MAIL_PIN, 0, pin, 32);
    bool pinned = false;
    for (int k = 0; k < 32; ++k) pinned |= pin[k] != 0;
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"busy\":"); json_bool(o, s.busy);
    out_str(o, ",\"queued\":"); json_uint(o, s.queued);
    out_str(o, ",\"tries\":"); json_uint(o, s.tries);
    out_str(o, ",\"sent\":"); json_uint(o, s.sent);
    out_str(o, ",\"failed\":"); json_uint(o, s.failed);
    out_str(o, ",\"dropped\":"); json_uint(o, s.dropped);
    out_str(o, ",\"last_ok\":"); json_bool(o, s.last == MAIL_OK);
    out_str(o, ",\"last\":"); json_str(o, s.last_ms ? s.last_text : "");
    out_str(o, ",\"last_s_ago\":"); if (s.last_ms) json_uint(o, (bsp_ms() - s.last_ms) / 1000u); else out_str(o, "null");
    out_str(o, ",\"server_fp\":"); if (s.have_fp) hex32(o, s.server_fp); else out_str(o, "null");
    out_str(o, ",\"pinned\":"); if (pinned) hex32(o, pin); else out_str(o, "null");
    out_str(o, "}");
}

static void post_mail_test(http_out_t *o, uint8_t lvl) {
    if (lvl < PLAT_LVL_ADMIN) { json_error(o, 403, "access", -1); return; }
    if (mail_request_test() != 0) { json_error(o, 409, "mail is off", -1); return; }
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true}");
}

/* trust=1: pin the certificate the server presented on the last connection; trust=0: unpin */
static void post_mail_trust(const http_req_t *r, http_out_t *o, uint8_t lvl) {
    if (lvl < PLAT_LVL_ADMIN) { json_error(o, 403, "access", -1); return; }
    char v[4] = "";
    form_get((const char *)r->body, r->body_len, "trust", v, sizeof v);
    mail_status_t s;
    mail_status(&s);
    uint8_t pin[32] = { 0 };
    if (!strcmp(v, "1")) {
        if (!s.have_fp) { json_error(o, 409, "no server certificate seen yet: send a test email first", -1); return; }
        memcpy(pin, s.server_fp, 32);
    }
    param_err_t pe = param_blob_post(PARAM_ID_MAIL_PIN, 0, pin, 32, false, PLAT_SRC_WEB, lvl);
    if (pe != PERR_OK) { json_error(o, perr_status(pe), perr_name(pe), -1); return; }
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true}");
}

static void post_fw_activate(http_out_t *o, uint8_t lvl) {
    if (lvl < PLAT_LVL_FACTORY) { json_error(o, 403, "access", -1); return; }
    fw_err_t e = fw_request_activate_from(PLAT_SRC_WEB, lvl);
    if (e == FW_E_WAIT_LOCAL) {
        head(o, 202, "application/json", NULL);
        out_str(o, "{\"ok\":true,\"restarting\":false,\"waiting_local\":true}");
        return;
    }
    if (e == FW_E_NOT_SAFE) {
        fw_status_t st; fw_status(&st);
        head(o, 409, "application/json", NULL);
        out_str(o, "{\"ok\":false,\"error\":\"not safe\",\"why\":"); json_str(o, st.not_safe); out_str(o, "}");
        return;
    }
    if (e != FW_OK) { json_error(o, e == FW_E_NO_IMAGE ? 404 : 409, fw_err_name(e), -1); return; }
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true,\"restarting\":true}");
}

bool http_wants_stream(const http_req_t *r) {
    return !strcmp(r->method, "POST") && !strcmp(r->path, "/api/fw/upload");
}

/* Read and drop the rest of a refused upload, so the client gets the answer instead
 * of a broken connection (with TLS, closing on unread data resets the connection). */
static void drain(http_read_fn rd, void *ctx, uint32_t left) {
    if (left > 2u * 1024u * 1024u || !cfg.scratch) return;   /* not for absurd lengths */
    while (left) {
        int n = rd(ctx, cfg.scratch, left < cfg.scratch_size ? left : cfg.scratch_size);
        if (n <= 0) return;
        left -= (uint32_t)n;
    }
}

void http_handle_stream(const http_req_t *r, http_read_fn rd, void *ctx, http_out_t *o, uint32_t now) {
    uint8_t lvl = auth_session(r->sid, now, NULL);
    uint32_t rest = r->content_length - r->body_len;
    if (!r->csrf) { drain(rd, ctx, rest); json_error(o, 403, "missing X-CEMS header", -1); out_end(o); return; }
    if (lvl == 0) { drain(rd, ctx, rest); json_error(o, 401, "login", -1); out_end(o); return; }
    if (lvl < PLAT_LVL_FACTORY) { drain(rd, ctx, rest); json_error(o, 403, "access", -1); out_end(o); return; }   /* tech level (D14) */
    fw_err_t e = fw_upload_begin(r->content_length);
    if (e == FW_OK) e = fw_upload_data(r->body, r->body_len);
    uint32_t got = r->body_len;
    while (e == FW_OK && got < r->content_length) {
        uint32_t want = r->content_length - got;
        if (want > cfg.scratch_size) want = cfg.scratch_size;
        int n = (cfg.scratch && want) ? rd(ctx, cfg.scratch, want) : -1;
        if (n <= 0) { fw_upload_abort(); json_error(o, 400, "upload interrupted", -1); out_end(o); return; }
        got += (uint32_t)n;
        e = fw_upload_data(cfg.scratch, (uint32_t)n);
    }
    if (e == FW_OK) e = fw_upload_end();
    else { fw_upload_abort(); drain(rd, ctx, r->content_length - got); }
    if (e != FW_OK) {
        plat_log_event(LOG_EV_FW_UPDATE, PLAT_SRC_WEB, lvl, (uint32_t)e, NAN, NAN, "upload refused");
        json_error(o, e == FW_E_BUSY ? 409 : 400, fw_err_name(e), -1);
        out_end(o);
        return;
    }
    auth_touch(r->sid, now);
    fw_status_t st;
    fw_status(&st);
    plat_log_event(LOG_EV_FW_UPDATE, PLAT_SRC_WEB, lvl, 0, NAN, NAN, st.staged_version);
    head(o, 200, "application/json", NULL);
    out_str(o, "{\"ok\":true,\"staged\":"); json_str(o, st.staged_version); out_str(o, "}");
    out_end(o);
}

/* ---------------- router ---------------- */
void http_handle(const http_req_t *r, http_out_t *o, uint32_t now) {
    bool get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
    uint32_t left;
    uint8_t lvl = auth_session(r->sid, now, &left);

    if (get && (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html"))) {
        char extra[64];
        snprintf(extra, sizeof extra, "Content-Encoding: gzip\r\nContent-Length: %lu\r\n", (unsigned long)web_index_gz_len);
        head(o, 200, "text/html; charset=utf-8", extra);
        out_bytes(o, web_index_gz, web_index_gz_len);
    }
    else if (get && !strcmp(r->path, "/api/info"))   get_info(r, o, now);
    else if (get && !strcmp(r->path, "/api/params")) get_params(o);
    else if (get && !strcmp(r->path, "/api/values")) get_values(r, o, now);
    else if (get && !strcmp(r->path, "/api/backup")) get_backup(o, lvl);
    else if (get && !strcmp(r->path, "/api/log/info")) get_log_info(o);
    else if (get && !strcmp(r->path, "/api/fw")) get_fw(o);
    else if (get && !strcmp(r->path, "/api/alarms")) get_alarms(o);
    else if (get && !strcmp(r->path, "/api/alarms/config")) get_alarm_config(o);
    else if (get && !strcmp(r->path, "/api/mail")) get_mail(o);
    else if (get && !strcmp(r->path, "/api/time")) get_time(o);
    else if (get && !strcmp(r->path, "/api/serial")) get_serial(r, o);
    else if (get && !strcmp(r->path, "/api/tls/cert")) get_cert(o);
    else if (get && !strcmp(r->path, "/api/log/events")) get_log_events(r, o);
    else if (get && !strcmp(r->path, "/api/log/events.csv")) get_log_csv(r, o, LOG_RING_EVENTS);
    else if (get && !strcmp(r->path, "/api/log/data.csv")) get_log_csv(r, o, LOG_RING_DATA);
    else if (post && strncmp(r->path, "/api/", 5) == 0) {
        if (!r->csrf) json_error(o, 403, "missing X-CEMS header", -1);
        else if (!strcmp(r->path, "/api/login")) post_login(r, o, now);
        else if (!strcmp(r->path, "/api/logout")) {
            if (lvl) plat_log_event(LOG_EV_LOGOUT, PLAT_SRC_WEB, lvl, 0, NAN, NAN, NULL);
            auth_logout(r->sid); head(o, 200, "application/json",
                     "Set-Cookie: sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict\r\n"); out_str(o, "{\"ok\":true}"); }
        else if (lvl == 0) json_error(o, 401, "login", -1);
        else if (!strcmp(r->path, "/api/write")) post_write(r, o, lvl, now);
        else if (!strcmp(r->path, "/api/password")) post_password(r, o, lvl, now);
        else if (!strcmp(r->path, "/api/restore")) post_restore(r, o, lvl);
        else if (!strcmp(r->path, "/api/fw/activate")) post_fw_activate(o, lvl);
        else if (!strcmp(r->path, "/api/alarms/ack")) post_alarm_ack(r, o, lvl);
        else if (!strcmp(r->path, "/api/mail/test")) post_mail_test(o, lvl);
        else if (!strcmp(r->path, "/api/time")) post_time(r, o, lvl);
        else if (!strcmp(r->path, "/api/serial")) post_serial(r, o, lvl);
        else if (!strcmp(r->path, "/api/mail/trust")) post_mail_trust(r, o, lvl);
        else json_error(o, 404, "not found", -1);
    }
    else if (!get && !post) json_error(o, 405, "method", -1);
    else json_error(o, 404, "not found", -1);
    out_end(o);
}
