/* fw_pubkey.c - the product's firmware-signing public key (decision D14).
 *
 * DEVELOPMENT BUILD: no key, so unsigned images are accepted (and the web page and
 * touchscreen say so). For production, replace this file with the output of
 *     tools/fwkey.py pubkey <private-key.pem> > platform/core/fw_pubkey.c
 * The private key is a tech-level credential: keep it off this repository. */
#include "fw_update.h"

const uint8_t fw_pubkey[65] = { 0 };
const bool    fw_pubkey_present = false;
