/* web_auth.h - passwords and web sessions (design spec "Network security":
 * login on the web interface; write access re-authenticates after inactivity;
 * 3-4 simultaneous sessions).
 *
 * Three password levels: operator, admin, factory (plat_level_t 1..3). Each is
 * stored as SHA-256(salt | level | password) with a random per-unit salt, in
 * FRAM, as platform retained blocks that are NOT part of the readable parameter
 * table. An all-zero hash means "factory default password" (DECISION D9:
 * operator 1111, admin 2222, factory 9999; the web page warns while any default
 * is in use). "Restore defaults" returns the passwords to these defaults.
 *
 * Viewing needs no login. A session (cookie) carries a level and is valid for
 * writes for AUTH_IDLE_MS after the login or the last write, then the user logs
 * in again. Five wrong passwords in a row lock login for 30 s. The touchscreen
 * UI uses auth_check() with the same passwords. */
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
#define AUTH_PW_MAX 32

typedef enum { AUTH_OK = 0, AUTH_BAD, AUTH_LOCKED, AUTH_INVALID } auth_rc_t;

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

#endif
