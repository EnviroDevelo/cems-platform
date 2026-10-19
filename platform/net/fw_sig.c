/* fw_sig.c - ECDSA P-256 check of firmware images (fw_update.h, decision D14). */
#include <string.h>
#include "fw_update.h"
#include "tls.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

static const uint8_t *override_q;

void fw_pubkey_override_for_test(const uint8_t *q65) { override_q = q65; }

bool fw_signing_required(void) { return override_q != NULL || fw_pubkey_present; }

bool fw_signature_check(const uint8_t hash[32], const uint8_t sig[64]) {
    const uint8_t *q = override_q ? override_q : fw_pubkey_present ? fw_pubkey : NULL;
    if (!q) return false;
    tls_crypto_init();                         /* mbedTLS memory pool */
    mbedtls_ecp_group grp;
    mbedtls_ecp_point pt;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&pt);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (!rc) rc = mbedtls_ecp_point_read_binary(&grp, &pt, q, 65);
    if (!rc) rc = mbedtls_ecp_check_pubkey(&grp, &pt);
    if (!rc) rc = mbedtls_mpi_read_binary(&r, sig, 32);
    if (!rc) rc = mbedtls_mpi_read_binary(&s, sig + 32, 32);
    if (!rc) rc = mbedtls_ecdsa_verify(&grp, hash, 32, &pt, &r, &s);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    mbedtls_ecp_point_free(&pt);
    mbedtls_ecp_group_free(&grp);
    return rc == 0;
}
