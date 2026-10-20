/* dev_pubkey.c - the developer public key that authorises serial number changes
 * (plat_serial.h, decision D25). No key here: the serial cannot be changed.
 * Generate the real one (the private key stays with the developer, never here):
 *     tools/fwkey.py new cems-dev-key.pem
 *     tools/fwkey.py devpubkey cems-dev-key.pem > platform/core/dev_pubkey.c */
#include "plat_serial.h"

const uint8_t dev_pubkey[65] = { 0 };
const bool    dev_pubkey_present = false;
