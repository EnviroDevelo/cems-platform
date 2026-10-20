/* http_util.c - see http_util.h */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "http_util.h"

void out_init(http_out_t *o, int (*send)(void *, const void *, size_t), void *ctx) {
    o->send = send; o->ctx = ctx; o->n = 0; o->total = 0; o->err = false;
    o->keep = o->chunked = o->head_done = false;
}
void out_flush(http_out_t *o) {
    if (o->n && !o->err) {
        if (o->chunked) {
            char h[12];
            int hl = snprintf(h, sizeof h, "%lx\r\n", (unsigned long)o->n);
            if (o->send(o->ctx, h, (size_t)hl) != 0 || o->send(o->ctx, o->buf, o->n) != 0 ||
                o->send(o->ctx, "\r\n", 2) != 0) o->err = true;
        } else if (o->send(o->ctx, o->buf, o->n) != 0) {
            o->err = true;
        }
    }
    o->n = 0;
}
void out_head_done(http_out_t *o, bool has_length) {
    o->head_done = true;
    if (o->keep && !has_length) { out_flush(o); o->chunked = true; }
}
void out_end(http_out_t *o) {
    out_flush(o);
    if (o->chunked && !o->err && o->send(o->ctx, "0\r\n\r\n", 5) != 0) o->err = true;
    o->chunked = false;
}
void out_bytes(http_out_t *o, const void *p, size_t n) {
    const char *s = p;
    o->total += n;
    while (n && !o->err) {
        size_t t = sizeof o->buf - o->n < n ? sizeof o->buf - o->n : n;
        memcpy(o->buf + o->n, s, t); o->n += t; s += t; n -= t;
        if (o->n == sizeof o->buf) out_flush(o);
    }
}
void out_str(http_out_t *o, const char *s) { out_bytes(o, s, strlen(s)); }

void json_strn(http_out_t *o, const char *s, size_t n) {
    out_bytes(o, "\"", 1);
    for (size_t k = 0; k < n; ++k) {
        unsigned char c = (unsigned char)s[k];
        char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; out_bytes(o, e, 2); }
        else if (c < 0x20 || c == 0x7F) { snprintf(e, sizeof e, "\\u%04x", c); out_bytes(o, e, 6); }
        else if (c == '<') out_bytes(o, "\\u003c", 6);          /* never closes a <script> */
        else out_bytes(o, &s[k], 1);
    }
    out_bytes(o, "\"", 1);
}
void json_str(http_out_t *o, const char *s) { if (!s) out_str(o, "null"); else json_strn(o, s, strlen(s)); }
void json_int(http_out_t *o, long v) { char b[16]; snprintf(b, sizeof b, "%ld", v); out_str(o, b); }
void json_uint(http_out_t *o, unsigned long v) { char b[16]; snprintf(b, sizeof b, "%lu", v); out_str(o, b); }
void json_bool(http_out_t *o, bool v) { out_str(o, v ? "true" : "false"); }
void json_float(http_out_t *o, float v) { char b[24]; fmt_float(b, sizeof b, (double)v); out_str(o, b); }

/* ---------------- float formatting ---------------- */
static unsigned long long p10u(int n) { unsigned long long p = 1; while (n-- > 0) p *= 10u; return p; }

void fmt_float(char *buf, size_t cap, double v) {
    if (!isfinite(v)) { snprintf(buf, cap, "null"); return; }
    if (v == 0.0) { snprintf(buf, cap, "0"); return; }
    const int SIG = 7;
    bool neg = v < 0; double a = neg ? -v : v;
    int e = (int)floor(log10(a));
    unsigned long long m = (unsigned long long)llround(a / pow(10.0, e - (SIG - 1)));   /* SIG digits */
    if (m >= p10u(SIG)) { m /= 10u; e++; }                                             /* 9.9999999 -> 10 */
    if (m < p10u(SIG - 1)) {                                                            /* log10 rounded up */
        e--; m = (unsigned long long)llround(a / pow(10.0, e - (SIG - 1)));
        if (m >= p10u(SIG)) { m /= 10u; e++; }
    }
    char digits[24];
    snprintf(digits, sizeof digits, "%lu%06lu", (unsigned long)(m / 1000000u), (unsigned long)(m % 1000000u));
    /* digits holds exactly SIG characters: d.dddddd x 10^e */
    int nd = SIG; while (nd > 1 && digits[nd - 1] == '0') nd--;
    char out[40]; size_t n = 0;
    if (neg) out[n++] = '-';
    if (e < -4 || e >= 9) {
        out[n++] = digits[0];
        if (nd > 1) { out[n++] = '.'; for (int k = 1; k < nd; ++k) out[n++] = digits[k]; }
        n += (size_t)snprintf(out + n, sizeof out - n, "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (e < 0) {
        out[n++] = '0'; out[n++] = '.';
        for (int k = 0; k < -e - 1; ++k) out[n++] = '0';
        for (int k = 0; k < nd; ++k) out[n++] = digits[k];
    } else {
        for (int k = 0; k <= e; ++k) out[n++] = k < SIG ? digits[k] : '0';
        if (nd > e + 1) { out[n++] = '.'; for (int k = e + 1; k < nd; ++k) out[n++] = digits[k]; }
    }
    out[n] = 0;
    snprintf(buf, cap, "%s", out);
}

/* ---------------- forms ---------------- */
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode(const char *s, const char *e, char *out, size_t cap) {
    size_t n = 0;
    while (s < e) {
        char c = *s++;
        if (c == '+') c = ' ';
        else if (c == '%') {
            if (e - s < 2 || hexv(s[0]) < 0 || hexv(s[1]) < 0) return false;
            c = (char)(hexv(s[0]) * 16 + hexv(s[1])); s += 2;
            if (c == 0) return false;                       /* no embedded NULs */
        }
        if (n + 1 >= cap) return false;
        out[n++] = c;
    }
    out[n] = 0;
    return true;
}

void form_begin(form_iter_t *it, const char *s, size_t len) { it->p = s; it->end = s + len; }

bool form_next(form_iter_t *it, char *key, size_t kcap, char *val, size_t vcap) {
    while (it->p < it->end) {
        const char *amp = memchr(it->p, '&', (size_t)(it->end - it->p));
        const char *pe = amp ? amp : it->end;
        const char *eq = memchr(it->p, '=', (size_t)(pe - it->p));
        const char *ke = eq ? eq : pe;
        bool ok = decode(it->p, ke, key, kcap) && decode(eq ? eq + 1 : pe, pe, val, vcap);
        it->p = amp ? amp + 1 : it->end;
        if (ok && key[0]) return true;
        if (!ok) { key[0] = 0; val[0] = 0; return true; }   /* report a bad pair as an empty key */
    }
    return false;
}

bool form_get(const char *s, size_t len, const char *key, char *val, size_t vcap) {
    form_iter_t it; char k[32];
    form_begin(&it, s, len);
    while (form_next(&it, k, sizeof k, val, vcap)) if (strcmp(k, key) == 0) return true;
    val[0] = 0;
    return false;
}

/* ---------------- text ---------------- */
int utf8_to_utf16le(const char *in, uint8_t *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)in;
    size_t n = 0;
    while (*p) {
        uint32_t c;
        if (p[0] < 0x80) { c = p[0]; p += 1; }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            c = (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
            if (c < 0x80) return -1;
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            c = (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
            if (c < 0x800 || (c >= 0xD800 && c <= 0xDFFF)) return -1;
            p += 3;
        } else return -1;                                  /* invalid, or outside the BMP */
        if (n + 2 > cap) return -1;
        out[n++] = (uint8_t)c; out[n++] = (uint8_t)(c >> 8);
    }
    return (int)n;
}

int utf16le_to_utf8(const uint8_t *in, size_t units, char *out, size_t cap) {
    size_t n = 0;
    for (size_t k = 0; k < units; ++k) {
        uint32_t c = (uint32_t)(in[2 * k] | (in[2 * k + 1] << 8));
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
        if (c < 0x80) { if (n + 1 >= cap) return -1; out[n++] = (char)c; }
        else if (c < 0x800) { if (n + 2 >= cap) return -1; out[n++] = (char)(0xC0 | c >> 6); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else { if (n + 3 >= cap) return -1; out[n++] = (char)(0xE0 | c >> 12); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[n] = 0;
    return (int)n;
}

int hex_decode(const char *in, uint8_t *out, size_t cap) {
    size_t l = strlen(in);
    if (l % 2 || l / 2 > cap) return -1;
    for (size_t k = 0; k < l / 2; ++k) {
        int h = hexv(in[2 * k]), lo = hexv(in[2 * k + 1]);
        if (h < 0 || lo < 0) return -1;
        out[k] = (uint8_t)(h * 16 + lo);
    }
    return (int)(l / 2);
}

void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t k = 0; k < n; ++k) { out[2 * k] = d[in[k] >> 4]; out[2 * k + 1] = d[in[k] & 15]; }
    out[2 * n] = 0;
}
