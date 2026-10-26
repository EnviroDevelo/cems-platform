/* web_auth.h - passwords and web sessions (design spec "Network security":
 * login on the web interface; write access re-authenticates after inactivity;
 * 3-4 simultaneous sessions).
 *
 * Three password levels: operator, admin, factory (plat_level_t 1..3). Each is
 * stored as PBKDF2-HMAC-SHA256(password, salt | level, AUTH_KDF_ITER) with a random
 * per-unit salt (CIP-20.3), in FRAM, as platform retained blocks that are NOT part of
 * the readable parameter table. A password set by older firmware (one SHA-256 of
 * salt | level | password) still works and is re-stored the slow way at its next good
 * login. New admin and factory passwords need AUTH_PW_MIN_HIGH characters; the operator
 * PIN for the touchscreen stays at AUTH_PW_MIN. An all-zero hash means "factory default password" (DECISION D9:
 * operator 1111, admin 2222, factory 9999; the web page warns while any default
 * is in use). "Restore defaults" returns the passwords to these defaults.
 *
 * Viewing needs no login. A session (cookie) carries a level and is valid for
 * writes for AUTH_IDLE_MS after the login or the last write, then the user logs
 * in again. Five wrong passwords in a row lock login for 30 s. The touchscreen
 * UI uses auth_check() with the same passwords.
 *
 * Credential recovery (CIP-19.3, Gary 26 Oct: by developer token). A unit whose
 * passwords are lost (the factory one included) can have all three put back to the
 * defaults with a token from the developer: an ECDSA P-256 signature, by the developer
 * key that also signs serial numbers (plat_serial.h), over
 *     CEMS-RESET v1 uid=<the chip's unique ID, 24 hex digits> n=<resets so far>
 * so it works on that one chip, once. Made with tools/reset_token.py. Needs no login
 * (whoever asks is locked out), counts as a failed login when wrong, and is logged.
 * The count is kept in the factory region (survives restore defaults). */
#ifndef WEB_AUTH_H
#define WEB_AUTH_H
#include <stdint.h>
#include <stdbool.h>

#ifndef AUTH_SESSIONS
#define AUTH_SESSIONS 4
#endif
#ifndef AUTH_IDLE_MS
#define AUTH_IDLE_MS (10u * 60u * 1000u)
#endif
#define AUTH_TOKEN_LEN 32            /* hex characters */
#define AUTH_PW_MIN 4
#define AUTH_PW_MIN_HIGH 8           /* new admin and factory passwords */
#define AUTH_PW_MAX 32
#ifndef AUTH_KDF_ITER
#define AUTH_KDF_ITER 4096u          /* PBKDF2 rounds: about 50 ms a check on the H573 */
#endif
#define AUTH_RESET_MSG_MAX 64u

typedef enum { AUTH_OK = 0, AUTH_BAD, AUTH_LOCKED, AUTH_INVALID, AUTH_NO_KEY } auth_rc_t;

void      auth_init(void);                         /* after param_store_boot: creates the salt on first use */
bool      auth_check(uint8_t level, const char *password);
bool      auth_is_default(uint8_t level);
bool      auth_any_default(void);
auth_rc_t auth_set_password(uint8_t level, const char *password);   /* the caller has checked authority */

auth_rc_t auth_login(uint8_t level, const char *password, uint32_t now_ms, char token[AUTH_TOKEN_LEN + 1]);
uint8_t   auth_session(const char *token, uint32_t now_ms, uint32_t *ms_left);   /* level, 0 = none/expired */
void      auth_touch(const char *token, uint32_t now_ms);        /* a write was made: restart the idle time */
void      auth_logout(const char *token);
uint32_t  auth_locked_ms(uint32_t now_ms);                       /* remaining lockout, 0 = not locked */
void      auth_reset_sessions(void);

/* Credential recovery: the text the developer signs, and the reset itself (sig = 64 bytes
 * r||s). AUTH_OK: all passwords are the defaults again and every session is ended. */
size_t    auth_reset_message(char *buf, size_t cap);
auth_rc_t auth_reset_with_token(const uint8_t sig[64], uint32_t now_ms);

void    auth_pbkdf2_for_test(const char *pw, const uint8_t salt[16], uint8_t level, uint32_t iter, uint8_t out[32]);
void    auth_set_legacy_for_test(uint8_t level, const char *pw);
uint8_t auth_kdf_for_test(uint8_t level);

#endif
