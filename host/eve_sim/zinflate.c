/* zinflate.c - a small zlib (RFC 1950/1951) decoder for the BT817 simulator's
 * CMD_INFLATE. Decodes a whole buffer; reports "need more" when the input ends early,
 * so the caller can retry once more bytes have arrived. Written for tests: clear
 * rather than fast. */
#include <string.h>
#include "zinflate.h"

typedef struct {
    const uint8_t *in; size_t inlen, pos; uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outcap, outlen;
    int short_input;
} st_t;

static int bits(st_t *s, int need) {
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->inlen) { s->short_input = 1; return -1; }
        v |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return (int)(v & ((1u << need) - 1u));
}

typedef struct { short count[16]; short symbol[320]; } huff_t;

static int decode(st_t *s, const huff_t *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -2;
}

static int construct(huff_t *h, const short *length, int n) {
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int s = 0; s < n; ++s) h->count[length[s]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; ++len) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int s = 0; s < n; ++s) if (length[s]) h->symbol[offs[length[s]]++] = (short)s;
    return left;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(st_t *s, const huff_t *lc, const huff_t *dc) {
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return sym;
        if (sym < 256) {
            if (s->outlen >= s->outcap) return -3;
            s->out[s->outlen++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -4;
            int e = bits(s, lext[sym]); if (e < 0) return -1;
            int len = lbase[sym] + e;
            int ds = decode(s, dc); if (ds < 0) return ds;
            if (ds >= 30) return -4;
            int de = bits(s, dext[ds]); if (de < 0) return -1;
            size_t dist = (size_t)(dbase[ds] + de);
            if (dist > s->outlen) return -5;
            if (s->outlen + (size_t)len > s->outcap) return -3;
            for (int k = 0; k < len; ++k, ++s->outlen) s->out[s->outlen] = s->out[s->outlen - dist];
        }
    }
}

static int fixed(st_t *s) {
    static huff_t lc, dc;
    static int built;
    if (!built) {
        short l[288];
        int k = 0;
        for (; k < 144; ++k) l[k] = 8;
        for (; k < 256; ++k) l[k] = 9;
        for (; k < 280; ++k) l[k] = 7;
        for (; k < 288; ++k) l[k] = 8;
        construct(&lc, l, 288);
        for (k = 0; k < 30; ++k) l[k] = 5;
        construct(&dc, l, 30);
        built = 1;
    }
    return codes(s, &lc, &dc);
}

static int dynamic(st_t *s) {
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320];
    huff_t lc, dc;
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return -6;
    int k = 0;
    for (; k < ncode; ++k) { int b = bits(s, 3); if (b < 0) return -1; lengths[order[k]] = (short)b; }
    for (; k < 19; ++k) lengths[order[k]] = 0;
    if (construct(&lc, lengths, 19) != 0) return -7;
    for (k = 0; k < nlen + ndist;) {
        int sym = decode(s, &lc);
        if (sym < 0) return sym;
        if (sym < 16) { lengths[k++] = (short)sym; continue; }
        short len = 0;
        int rep;
        if (sym == 16) { if (k == 0) return -8; len = lengths[k - 1]; rep = bits(s, 2); if (rep < 0) return -1; rep += 3; }
        else if (sym == 17) { rep = bits(s, 3); if (rep < 0) return -1; rep += 3; }
        else { rep = bits(s, 7); if (rep < 0) return -1; rep += 11; }
        if (k + rep > nlen + ndist) return -9;
        while (rep--) lengths[k++] = len;
    }
    int e = construct(&lc, lengths, nlen);
    if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return -10;
    e = construct(&dc, lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return -11;
    return codes(s, &lc, &dc);
}

int zinflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap, size_t *inused, size_t *outlen) {
    st_t s = { .in = in, .inlen = inlen, .out = out, .outcap = outcap };
    if (inlen < 2) return 1;
    if ((in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return -1;
    s.pos = 2;
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0) return 1;
        int rc;
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.pos + 4 > inlen) return 1;
            unsigned len = in[s.pos] | (in[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + len > inlen) return 1;
            if (s.outlen + len > outcap) return -3;
            memcpy(out + s.outlen, in + s.pos, len);
            s.pos += len; s.outlen += len; rc = 0;
        } else if (type == 1) rc = fixed(&s);
        else if (type == 2) rc = dynamic(&s);
        else return -1;
        if (rc == -1 && s.short_input) return 1;
        if (rc != 0) return -1;
    } while (!last);
    if (s.pos + 4 > inlen) return 1;                     /* Adler-32 trailer */
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < s.outlen; ++k) { a = (a + out[k]) % 65521u; b = (b + a) % 65521u; }
    uint32_t want = (uint32_t)in[s.pos] << 24 | (uint32_t)in[s.pos + 1] << 16 | (uint32_t)in[s.pos + 2] << 8 | in[s.pos + 3];
    if (((b << 16) | a) != want) return -1;
    *inused = s.pos + 4;
    *outlen = s.outlen;
    return 0;
}
