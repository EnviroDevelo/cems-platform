# Mbed TLS 3.6.7 (LTS), subset

From https://github.com/Mbed-TLS/mbedtls, tag `mbedtls-3.6.7`, unchanged. Licence:
Apache-2.0 (LICENSE). Only the 39 library sources that the platform's configuration
(`platform/net/tls_config.h`) actually uses are included, with all public and
internal headers. To update: copy the same files from a newer 3.6.x tag and run the
tests.

Build with `-DMBEDTLS_CONFIG_FILE='"tls_config.h"' -Iplatform/net
-Ithird_party/mbedtls/include -Ithird_party/mbedtls/library`.
