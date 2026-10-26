/* web_auth.c - see web_auth.h */
#include <string.h>
#include "web_auth.h"
#include "param_store.h"
#include "plat_sha256.h"
#include "plat_port.h"
#include "bsp.h"
#include "plat_serial.h"
#include "plat_cmdq.h"
#include <stdio.h>

#define LEVELS 3
#define FAIL_LIMIT 5
#define LOCK_MS 30000u

static uint8_t a_salt[16];
static uint8_t a_hash[LEVELS][32];
static uint8_t a_kdf[LEVELS];          /* how each hash was made: 0 = one SHA-256 (older firmware), 1 = PBKDF2 */
static uint32_t a_resets;              /* recovery tokens used so far (factory region: survives restore defaults) */
static const char *const defaults[LEVELS] = { "1111", "2222", "9999" };

static const retain_def_t blocks[] = {
    { .id = 900, .size = sizeof a_salt, .ptr = a_salt, .flags = RF_SECRET, .name = "auth_salt" },
    { .id = 901, .size = sizeof a_hash, .ptr = a_hash, .flags = RF_SECRET, .name = "auth_hash" },
    { .id = 908, .size = sizeof a_kdf, .ptr = a_kdf, .flags = RF_SECRET, .name = "auth_kdf" },
    { .id = 909, .size = sizeof a_resets, .ptr = &a_resets, .flags = RF_FACTORY | RF_UNIT, .name = "auth_resets" },
};
const retain_def_t *plat_store_blocks(size_t *n) { *n = sizeof blocks / sizeof blocks[0]; return blocks; }

typedef struct { char tok[AUTH_TOKEN_LEN + 1]; uint8_t level; uint32_t last_ms; bool used; } sess_t;
static sess_t   sess[AUTH_SESSIONS];
static uint8_t  fails;
static uint32_t lock_until;
static bool     locked;

static bool all_zero(const uint8_t *p, size_t n) { uint8_t x = 0; for (size_t k = 0; k < n; ++k) x |= p[k]; return x == 0; }

/* compare without an early exit, so timing does not reveal how much matched */
static bool same(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b; uint8_t d = 0;
    for (size_t k = 0; k < n; ++k) d |= (uint8_t)(x[k] ^ y[k]);
    return d == 0;
}

/* PBKDF2-HMAC-SHA256, one 32-byte block: the password is the HMAC key, salt | level the salt */
static void pbkdf2(const char *pw, const uint8_t salt[16], uint8_t level, uint32_t iter, uint8_t out[32]) {
    uint8_t k[64] = { 0 }, pad[64], u[32];
    size_t n = strlen(pw);
    if (n > 64) { sha256_t s; sha256_init(&s); sha256_update(&s, pw, n); sha256_final(&s, k); }
    else memcpy(k, pw, n);
    sha256_t in, outer;                                /* the keyed inner and outer states, reused every round */
    for (int j = 0; j < 64; ++j) pad[j] = (uint8_t)(k[j] ^ 0x36);
    sha256_init(&in); sha256_update(&in, pad, 64);
    for (int j = 0; j < 64; ++j) pad[j] = (uint8_t)(k[j] ^ 0x5c);
    sha256_init(&outer); sha256_update(&outer, pad, 64);
    static const uint8_t one[4] = { 0, 0, 0, 1 };
    sha256_t s = in;
    sha256_update(&s, salt, 16); sha256_update(&s, &level, 1); sha256_update(&s, one, 4);
    sha256_final(&s, u);
    s = outer; sha256_update(&s, u, 32); sha256_final(&s, u);
    memcpy(out, u, 32);
    for (uint32_t r = 1; r < iter; ++r) {
        s = in; sha256_update(&s, u, 32); sha256_final(&s, u);
        s = outer; sha256_update(&s, u, 32); sha256_final(&s, u);
        for (int j = 0; j < 32; ++j) out[j] ^= u[j];
    }
    memset(k, 0, sizeof k); memset(pad, 0, sizeof pad);
}

static void hash_pw(uint8_t level, const char *pw, uint8_t kdf, uint8_t out[32]) {
    if (kdf) { pbkdf2(pw, a_salt, level, AUTH_KDF_ITER, out); return; }
    sha256_t s;                                        /* how older firmware stored it */
    sha256_init(&s);
    sha256_update(&s, a_salt, sizeof a_salt);
    sha256_update(&s, &level, 1);
    sha256_update(&s, pw, strlen(pw));
    sha256_final(&s, out);
}

static void store_hash(uint8_t level, const uint8_t h[32]) {
    /* the scan copies these blocks for the store: don't let it see half a hash */
    PLAT_CRIT_ENTER(); memcpy(a_hash[level - 1], h, 32); a_kdf[level - 1] = 1; PLAT_CRIT_EXIT();
}

void auth_init(void) {
    if (all_zero(a_salt, sizeof a_salt)) {
        uint8_t s[16];
        bsp_random(s, sizeof s);
        if (all_zero(s, sizeof s)) s[0] = 1;
        PLAT_CRIT_ENTER(); memcpy(a_salt, s, sizeof s); memset(a_hash, 0, sizeof a_hash); PLAT_CRIT_EXIT();
    }
    auth_reset_sessions();
    fails = 0; locked = false;
}

static bool valid_level(uint8_t level) { return level >= 1 && level <= LEVELS; }

bool auth_is_default(uint8_t level) { return valid_level(level) && all_zero(a_hash[level - 1], 32); }
bool auth_any_default(void) { for (uint8_t l = 1; l <= LEVELS; ++l) if (auth_is_default(l)) return true; return false; }

bool auth_check(uint8_t level, const char *pw) {
    if (!valid_level(level) || !pw) return false;
    size_t n = strlen(pw);
    if (n < AUTH_PW_MIN || n > AUTH_PW_MAX) return false;
    if (auth_is_default(level)) {
        char d[AUTH_PW_MAX + 1] = { 0 }, p[AUTH_PW_MAX + 1] = { 0 };
        strcpy(d, defaults[level - 1]); memcpy(p, pw, n);
        return same(d, p, sizeof d);
    }
    uint8_t h[32], kdf = a_kdf[level - 1];
    hash_pw(level, pw, kdf, h);
    if (!same(h, a_hash[level - 1], 32)) return false;
    if (!kdf) { hash_pw(level, pw, 1, h); store_hash(level, h); }   /* re-store it the slow way */
    return true;
}

auth_rc_t auth_set_password(uint8_t level, const char *pw) {
    if (!valid_level(level) || !pw) return AUTH_INVALID;
    size_t n = strlen(pw);
    if (n < AUTH_PW_MIN || n > AUTH_PW_MAX) return AUTH_INVALID;
    for (size_t k = 0; k < n; ++k) if (pw[k] < 0x21 || pw[k] > 0x7E) return AUTH_INVALID;   /* printable, no spaces */
    uint8_t h[32];
    hash_pw(level, pw, 1, h);
    if (all_zero(h, 32)) return AUTH_INVALID;          /* would read as "default" (never in practice) */
    store_hash(level, h);
    return AUTH_OK;
}

/* ---- for tests ---- */
void auth_pbkdf2_for_test(const char *pw, const uint8_t salt[16], uint8_t level, uint32_t iter, uint8_t out[32]) {
    pbkdf2(pw, salt, level, iter, out);
}
void auth_set_legacy_for_test(uint8_t level, const char *pw) {   /* as older firmware stored it */
    uint8_t h[32];
    hash_pw(level, pw, 0, h);
    memcpy(a_hash[level - 1], h, 32); a_kdf[level - 1] = 0;
}
uint8_t auth_kdf_for_test(uint8_t level) { return a_kdf[level - 1]; }

size_t auth_reset_message(char *buf, size_t cap) {
    char uid[25];
    plat_serial_uid_hex(uid);
    int n = snprintf(buf, cap, "CEMS-RESET v1 uid=%s n=%lu", uid, (unsigned long)a_resets);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

auth_rc_t auth_reset_with_token(const uint8_t sig[64], uint32_t now) {
    if (auth_locked_ms(now)) return AUTH_LOCKED;
    if (!plat_serial_key_present()) return AUTH_NO_KEY;
    char msg[AUTH_RESET_MSG_MAX];
    size_t n = auth_reset_message(msg, sizeof msg);
    if (!sig || !plat_dev_token_ok(msg, n, sig)) {
        if (++fails >= FAIL_LIMIT) { locked = true; lock_until = now + LOCK_MS; }
        return AUTH_BAD;
    }
    PLAT_CRIT_ENTER(); memset(a_hash, 0, sizeof a_hash); a_resets++; PLAT_CRIT_EXIT();
    auth_reset_sessions();
    fails = 0;
    return AUTH_OK;
}

uint32_t auth_locked_ms(uint32_t now) {
    if (!locked) return 0;
    int32_t left = (int32_t)(lock_until - now);
    if (left <= 0) { locked = false; fails = 0; return 0; }
    return (uint32_t)left;
}

static const char hexd[] = "0123456789abcdef";

auth_rc_t auth_login(uint8_t level, const char *pw, uint32_t now, char token[AUTH_TOKEN_LEN + 1]) {
    if (auth_locked_ms(now)) return AUTH_LOCKED;
    if (!auth_check(level, pw)) {
        if (++fails >= FAIL_LIMIT) { locked = true; lock_until = now + LOCK_MS; }
        return AUTH_BAD;
    }
    fails = 0;
    sess_t *slot = NULL;
    for (int k = 0; k < AUTH_SESSIONS; ++k) {          /* free slot, else the least recently used */
        if (!sess[k].used) { slot = &sess[k]; break; }
        if (!slot || (uint32_t)(now - sess[k].last_ms) > (uint32_t)(now - slot->last_ms)) slot = &sess[k];
    }
    uint8_t r[AUTH_TOKEN_LEN / 2];
    bsp_random(r, sizeof r);
    for (int k = 0; k < AUTH_TOKEN_LEN / 2; ++k) { slot->tok[2 * k] = hexd[r[k] >> 4]; slot->tok[2 * k + 1] = hexd[r[k] & 15]; }
    slot->tok[AUTH_TOKEN_LEN] = 0;
    slot->level = level; slot->last_ms = now; slot->used = true;
    memcpy(token, slot->tok, AUTH_TOKEN_LEN + 1);
    return AUTH_OK;
}

static sess_t *find(const char *tok) {
    if (!tok || strlen(tok) != AUTH_TOKEN_LEN) return NULL;
    for (int k = 0; k < AUTH_SESSIONS; ++k)
        if (sess[k].used && same(sess[k].tok, tok, AUTH_TOKEN_LEN)) return &sess[k];
    return NULL;
}

uint8_t auth_session(const char *tok, uint32_t now, uint32_t *ms_left) {
    sess_t *s = find(tok);
    if (ms_left) *ms_left = 0;
    if (!s) return 0;
    uint32_t idle = now - s->last_ms;
    if (idle > AUTH_IDLE_MS) { s->used = false; return 0; }
    if (ms_left) *ms_left = AUTH_IDLE_MS - idle;
    return s->level;
}

void auth_touch(const char *tok, uint32_t now) { sess_t *s = find(tok); if (s) s->last_ms = now; }
void auth_logout(const char *tok) { sess_t *s = find(tok); if (s) s->used = false; }
void auth_reset_sessions(void) { memset(sess, 0, sizeof sess); }
