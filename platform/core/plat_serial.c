/* plat_serial.c - developer-authorised serial number changes (plat_serial.h, D25). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "plat_serial.h"
#include "plat_sha256.h"
#include "plat_log.h"
#include "param.h"
#include "bsp.h"

static const uint8_t *override_q;

void plat_serial_key_override_for_test(const uint8_t *q65) { override_q = q65; }

static const uint8_t *key(void) { return override_q ? override_q : dev_pubkey_present ? dev_pubkey : NULL; }

bool plat_serial_key_present(void) { return key() != NULL; }

void plat_serial_uid(uint8_t uid[12]) { bsp_chip_uid(uid); }

void plat_serial_uid_hex(char out[25]) {
    uint8_t u[12];
    plat_serial_uid(u);
    for (int k = 0; k < 12; ++k) snprintf(out + 2 * k, 3, "%02x", u[k]);
}

uint32_t plat_serial(void) {
    plat_val_t v;
    return param_read(PARAM_ID_SERIAL, &v) ? v.u : 0u;
}

size_t plat_serial_message(uint32_t new_serial, char *buf, size_t cap) {
    char uid[25];
    plat_serial_uid_hex(uid);
    int n = snprintf(buf, cap, "CEMS-SERIAL v1 uid=%s old=%lu new=%lu", uid, (unsigned long)plat_serial(),
                     (unsigned long)new_serial);
    return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}

static void log_it(uint8_t src, uint8_t level, uint32_t old_s, double new_v, const char *why) {
    plat_log_event(LOG_EV_SERIAL, src, level, old_s, (double)old_s, new_v, why);
}

int plat_serial_assign(uint32_t new_serial, const uint8_t sig[64], uint8_t src, uint8_t level) {
    uint32_t old_s = plat_serial();
    if (!key()) { log_it(src, level, old_s, NAN, "no developer key"); return SER_E_NO_KEY; }
    if (level < PLAT_LVL_FACTORY) return SER_E_LEVEL;
    if (new_serial == old_s) return SER_E_SAME;
    char msg[SERIAL_MSG_MAX];
    size_t n = plat_serial_message(new_serial, msg, sizeof msg);
    uint8_t h[32];
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, msg, n);
    sha256_final(&s, h);
    if (!n || !plat_p256_verify(key(), h, sig)) { log_it(src, level, old_s, (double)new_serial, "token refused"); return SER_E_SIG; }
    if (param_post(PARAM_ID_SERIAL, PLAT_VAL_U32, (plat_val_t){ .u = new_serial }, PLAT_SRC_INTERNAL, PLAT_LVL_FACTORY) != PERR_OK)
        return SER_E_BUSY;
    log_it(src, level, old_s, (double)new_serial, "developer token");
    return SER_OK;
}

const char *plat_serial_err(int rc) {
    switch (rc) {
    case SER_OK:       return "ok";
    case SER_E_NO_KEY: return "this firmware has no developer key: the serial cannot be changed";
    case SER_E_LEVEL:  return "factory login needed";
    case SER_E_SIG:    return "token not valid for this unit, this serial and this developer key";
    case SER_E_SAME:   return "that is the serial already";
    default:           return "busy, try again";
    }
}
