/* ul_lf.h - C models of the UniLogic ladder blocks used by the Stage 4A utilities.
 *
 * These model the ORIGINAL behaviour so the transliterations in 4A_faithful.c can be
 * run and tested. They are not meant for the new firmware (see 4A_new.c).
 * Every behaviour the UniLogic help does not state is marked ASSUMPTION with the
 * number used in 11_UniLogic_Semantics.md section 6 or 41_Utilities.md section 2.
 * Source of each block's pin meaning: stage1/research/11a and 11c.
 */
#ifndef UL_LF_H
#define UL_LF_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* ---- strings (ASCII). cap = declared length in characters, storage is cap+1 ---- */

/* StoreString: copy, truncate to capacity. ASSUMPTION 6.12 (truncate) */
static inline void lf_store_string(const char *src, char *dst, size_t cap) {
    char tmp[300]; size_t n = strlen(src); if (n > cap) n = cap;
    memcpy(tmp, src, n); tmp[n] = 0; memcpy(dst, tmp, n + 1);
}

/* StrLen: characters, NUL not counted (ASSUMPTION, 11c section 5) */
static inline int32_t lf_strlen(const char *s) { return (int32_t)strlen(s); }

/* InsertToString A, B, offset C -> D (D may be A). 0-based (ASSUMPTION 6.13).
 * Offset > len(A): no change (ASSUMPTION A4.5). Too long: truncate (ASSUMPTION 6.12). */
static inline void lf_insert_to_string(const char *a, const char *b, int off, char *d, size_t cap) {
    char tmp[600]; size_t la = strlen(a), lb = strlen(b);
    if (off < 0 || (size_t)off > la) { if (d != a) lf_store_string(a, d, cap); return; }
    memcpy(tmp, a, off); memcpy(tmp + off, b, lb); memcpy(tmp + off + lb, a + off, la - off);
    tmp[la + lb] = 0; lf_store_string(tmp, d, cap);
}

/* RemoveFromString: remove up to `times` occurrences of `sub`, searching from `start`,
 * left to right, restarting at the removal point (ASSUMPTION, 11c section 4). */
static inline void lf_remove_from_string(const char *a, const char *sub, int start, int times,
                                         char *e, size_t cap) {
    char tmp[300]; lf_store_string(a, tmp, sizeof tmp - 1);
    size_t ls = strlen(sub), la = strlen(tmp);
    if (ls && start >= 0 && (size_t)start <= la) {
        char *p = tmp + start;
        for (int k = 0; k < times; ++k) {
            char *f = strstr(p, sub); if (!f) break;
            memmove(f, f + ls, strlen(f + ls) + 1); p = f;
        }
    }
    lf_store_string(tmp, e, cap);
}

/* StringLeft / StringRight: first / last n characters, clamped (ASSUMPTION, 11c 6-7) */
static inline void lf_string_left(const char *a, int n, char *c, size_t cap) {
    char tmp[300]; size_t la = strlen(a), k = n < 0 ? 0 : (size_t)n; if (k > la) k = la;
    memcpy(tmp, a, k); tmp[k] = 0; lf_store_string(tmp, c, cap);
}
static inline void lf_string_right(const char *a, int n, char *c, size_t cap) {
    char tmp[300]; size_t la = strlen(a), k = n < 0 ? 0 : (size_t)n; if (k > la) k = la;
    memcpy(tmp, a + la - k, k); tmp[k] = 0; lf_store_string(tmp, c, cap);
}

/* ---- byte arrays / buffers (UINT8[n]) ---- */

/* FillArrPart / Fill_Buffer, Overwrite mode: a[off .. off+cnt-1] = v.
 * Range past the end is clipped to the array (ASSUMPTION A4.1). Offset outside: no change. */
static inline void lf_fill_u8(uint8_t *a, size_t n, int off, uint8_t v, int cnt) {
    if (off < 0 || (size_t)off >= n || cnt <= 0) return;
    size_t c = (size_t)cnt; if (c > n - (size_t)off) c = n - (size_t)off;
    memset(a + off, v, c);
}
/* LoadFromArr: index outside the array leaves the result unchanged (ASSUMPTION A4.1) */
static inline void lf_load_u8(const uint8_t *a, size_t n, int idx, uint8_t *out) {
    if (idx >= 0 && (size_t)idx < n) *out = a[idx];
}
/* StoreInArr: index outside the array does nothing (ASSUMPTION A4.1) */
static inline void lf_store_u8(uint8_t *a, size_t n, int idx, uint8_t v) {
    if (idx >= 0 && (size_t)idx < n) a[idx] = v;
}
/* CopyArrPartToArr: overlap-safe (memmove), clipped; start outside -> nothing (ASSUMPTION A4.1) */
static inline void lf_copy_part_u8(const uint8_t *s, size_t sn, int si, int cnt,
                                   uint8_t *d, size_t dn, int di) {
    if (si < 0 || di < 0 || cnt <= 0 || (size_t)si >= sn || (size_t)di >= dn) return;
    size_t c = (size_t)cnt; if (c > sn - si) c = sn - si; if (c > dn - di) c = dn - di;
    memmove(d + di, s + si, c);
}
/* BufferToTag into a string of `cap` characters. DOCUMENTED: nothing happens if the tag is
 * too short. The string becomes exactly those bytes, ending at the first 0 byte
 * (ASSUMPTION A4.4). */
static inline void lf_buffer_to_string(const uint8_t *buf, size_t blen, int off, int len,
                                       char *s, size_t cap) {
    if (len < 0 || (size_t)len > cap) return;
    if (off < 0 || (size_t)off + (size_t)len > blen) return;
    memcpy(s, buf + off, (size_t)len); s[len] = 0;
}

/* ---- numbers ---- */

/* FloatToInt ("Real to INT"): C = integer part (toward zero), D = fraction x 10^B rounded,
 * sign kept. THIRD-PARTY observation (11a 5.8), ASSUMPTION A4.6. REAL is 32-bit float. */
static inline void lf_float_to_int(float a, int b, int64_t *c, int64_t *d) {
    float ip = truncf(a);
    *c = (int64_t)ip;
    *d = llroundf((a - ip) * powf(10.0f, (float)b));
}

#endif
