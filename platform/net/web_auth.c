/* web_auth.c - see web_auth.h */
#include <string.h>
#include "web_auth.h"
#include "param_store.h"
#include "plat_sha256.h"
#include "plat_port.h"
#include "bsp.h"

#define LEVELS 3
#define FAIL_LIMIT 5
#define LOCK_MS 30000u

static uint8_t a_salt[16];
static uint8_t a_hash[LEVELS][32];
static const char *const defaults[LEVELS] = { "1111", "2222", "9999" };

static const retain_def_t blocks[] = {
    { .id = 900, .size = sizeof a_salt, .ptr = a_salt, .flags = RF_SECRET, .name = "auth_salt" },
    { .id = 901, .size = sizeof a_hash, .ptr = a_hash, .flags = RF_SECRET, .name = "auth_hash" },
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

static void hash_pw(uint8_t level, const char *pw, uint8_t out[32]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, a_salt, sizeof a_salt);
    sha256_update(&s, &level, 1);
    sha256_update(&s, pw, strlen(pw));
    sha256_final(&s, out);
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
    uint8_t h[32];
    hash_pw(level, pw, h);
    return same(h, a_hash[level - 1], 32);
}

auth_rc_t auth_set_password(uint8_t level, const char *pw) {
    if (!valid_level(level) || !pw) return AUTH_INVALID;
    size_t n = strlen(pw);
    if (n < AUTH_PW_MIN || n > AUTH_PW_MAX) return AUTH_INVALID;
    for (size_t k = 0; k < n; ++k) if (pw[k] < 0x21 || pw[k] > 0x7E) return AUTH_INVALID;   /* printable, no spaces */
    uint8_t h[32];
    hash_pw(level, pw, h);
    if (all_zero(h, 32)) return AUTH_INVALID;          /* would read as "default" (never in practice) */
    /* the scan copies this block for the store: don't let it see half a hash */
    PLAT_CRIT_ENTER(); memcpy(a_hash[level - 1], h, 32); PLAT_CRIT_EXIT();
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
