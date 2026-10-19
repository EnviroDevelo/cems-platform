/* http_util.h - small helpers for the web server: a buffered output stream with
 * JSON writing, form/query decoding, UTF-8 <-> UTF-16, hex, float formatting
 * (newlib-nano's printf has no float support on the target). */
#ifndef HTTP_UTIL_H
#define HTTP_UTIL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int    (*send)(void *ctx, const void *p, size_t n);   /* 0 = sent */
    void    *ctx;
    char     buf[1024];
    size_t   n;
    size_t   total;
    bool     err;
} http_out_t;

void out_init(http_out_t *o, int (*send)(void *, const void *, size_t), void *ctx);
void out_bytes(http_out_t *o, const void *p, size_t n);
void out_str(http_out_t *o, const char *s);
void out_flush(http_out_t *o);

/* JSON values */
void json_str(http_out_t *o, const char *s);                 /* quoted, escaped (UTF-8 passes through) */
void json_strn(http_out_t *o, const char *s, size_t n);
void json_int(http_out_t *o, long v);
void json_uint(http_out_t *o, unsigned long v);
void json_float(http_out_t *o, float v);                     /* 7 significant digits; null if not finite */
void json_bool(http_out_t *o, bool v);

/* float -> text, 7 significant digits, shortest form ("12.5", "0.001", "1.234568e+12") */
void fmt_float(char *buf, size_t cap, double v);

/* application/x-www-form-urlencoded (also query strings): walk pairs in order.
 * Returns false at the end. Values are percent-decoded, '+' = space. */
typedef struct { const char *p, *end; } form_iter_t;
void form_begin(form_iter_t *it, const char *s, size_t len);
bool form_next(form_iter_t *it, char *key, size_t kcap, char *val, size_t vcap);
bool form_get(const char *s, size_t len, const char *key, char *val, size_t vcap);   /* first match */

/* text conversions; return output length, or -1 if invalid or too long */
int utf8_to_utf16le(const char *in, uint8_t *out, size_t cap);     /* BMP only */
int utf16le_to_utf8(const uint8_t *in, size_t units, char *out, size_t cap);
int hex_decode(const char *in, uint8_t *out, size_t cap);
void hex_encode(const uint8_t *in, size_t n, char *out);            /* out: 2n + 1 */

#endif
