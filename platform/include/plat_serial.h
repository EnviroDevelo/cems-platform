/* plat_serial.h - the unit's serial number, changed only with developer authority
 * (decision D25).
 *
 * The serial (parameter 50) names the unit in the web page, emails, backup and CSV
 * file names and its HTTPS certificate. No ordinary write changes it, at any login
 * level and from any route (touchscreen, web parameter page, Modbus, a restored
 * backup): the parameter carries PF_DEV. The only way in is plat_serial_assign()
 * with a token signed by the developer key, a P-256 key separate from the
 * technicians' firmware-signing key (D14), whose private half stays with the
 * developer. The token is an ECDSA P-256 signature over the SHA-256 of
 *
 *     CEMS-SERIAL v1 uid=<the chip's 96-bit unique ID, 24 hex digits> old=<serial now> new=<serial wanted>
 *
 * so it works on that one chip, once (after the change "old" no longer matches).
 * Made off the unit with tools/serial_token.py. Firmware built without a developer
 * key (dev_pubkey.c, from tools/fwkey.py devpubkey) cannot change the serial at all.
 * The request also needs a factory login. Every change, and every refused token,
 * goes to the event log.
 *
 * Threading: any task (the change itself is queued to the scan like any write). */
#ifndef PLAT_SERIAL_H
#define PLAT_SERIAL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SERIAL_MSG_MAX 96u

enum { SER_OK = 0, SER_E_NO_KEY = -1, SER_E_LEVEL = -2, SER_E_SIG = -3, SER_E_SAME = -4, SER_E_BUSY = -5 };

extern const uint8_t dev_pubkey[65];        /* developer public key, uncompressed P-256 (dev_pubkey.c) */
extern const bool    dev_pubkey_present;

void        plat_serial_uid(uint8_t uid[12]);                 /* the chip's unique ID */
void        plat_serial_uid_hex(char out[25]);
uint32_t    plat_serial(void);                                /* the serial now */
bool        plat_serial_key_present(void);
/* The exact text the developer signs to change the serial to new_serial. */
size_t      plat_serial_message(uint32_t new_serial, char *buf, size_t cap);
/* Change the serial: sig = 64 bytes r||s. level = the requester's login (factory needed).
 * SER_OK: queued, applied at the end of the next scan and saved with the factory data. */
int         plat_serial_assign(uint32_t new_serial, const uint8_t sig[64], uint8_t src, uint8_t level);
const char *plat_serial_err(int rc);

/* Is sig (64 bytes r||s) the developer key's signature over msg? false also when this build has
 * no developer key. Shared by the serial and the credential recovery token (web_auth.h). */
bool        plat_dev_token_ok(const char *msg, size_t n, const uint8_t sig[64]);

/* For tests: use this key instead of dev_pubkey (NULL = back to the built-in one). */
void plat_serial_key_override_for_test(const uint8_t *q65);
/* P-256 signature check (fw_sig.c). */
bool plat_p256_verify(const uint8_t q65[65], const uint8_t hash[32], const uint8_t sig[64]);
#endif
