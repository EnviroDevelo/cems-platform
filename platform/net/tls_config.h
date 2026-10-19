/* tls_config.h - mbedTLS 3.6 configuration for the instrument's HTTPS server and
 * firmware signature checks (decisions D11, D14). Selected with
 * -DMBEDTLS_CONFIG_FILE='"tls_config.h"'.
 *
 * TLS 1.2 server only, one cipher suite: ECDHE-ECDSA-AES128-GCM-SHA256 on P-256
 * (supported by every current browser). Session tickets, so a browser's repeated
 * connections (the page polls) skip the public-key work after the first. The unit's
 * own key and a self-signed certificate (tls.c). Memory from a fixed static pool, so
 * TLS can never take memory from anything else. */
#ifndef TLS_CONFIG_H
#define TLS_CONFIG_H

/* platform */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C          /* static pool (tls.c) */
#define MBEDTLS_MEMORY_DEBUG                   /* peak use, reported in tls_stats() */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_TIME_ALT               /* uptime seconds, for ticket lifetimes */
#define MBEDTLS_PLATFORM_MS_TIME_ALT            /* mbedtls_ms_time(): bsp_ms() (tls.c) */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT            /* mbedtls_hardware_poll(): the H5's TRNG (bsp_random) */
#define MBEDTLS_DEPRECATED_REMOVED

/* random numbers */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_HMAC_DRBG_C

/* symmetric */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES                  /* tables in flash, not RAM */
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C                        /* (mbedTLS builds SHA-224 with SHA-256) */
#define MBEDTLS_SHA384_C                        /* mail servers' certificates are often signed with SHA-384 */
#define MBEDTLS_SHA512_C
#define MBEDTLS_BASE64_C                        /* SMTP AUTH */

/* public key */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED        /* a mail server with a P-384 key */
#define MBEDTLS_RSA_C                           /* most mail servers have RSA keys (client only) */
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC             /* the same certificate every start (tls.c) */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C

/* certificates */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_CLI_C                       /* alarm email (mail.c) */
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE       /* its fingerprint is what the unit pins */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SESSION_TICKETS
#define MBEDTLS_SSL_TICKET_C
#define MBEDTLS_SSL_CIPHERSUITES MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, \
                                 MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384       /* a browser may send full 16 KB records (uploads) */
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH

#endif
