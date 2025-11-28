/* Compatibility layer for mbedtls API changes */
#ifndef ALTAIR_MBEDTLS_COMPAT_H
#define ALTAIR_MBEDTLS_COMPAT_H

#include "mbedtls/sha1.h"

/* Provide compatibility for deprecated mbedtls_sha1_ret */
#ifndef mbedtls_sha1_ret
static inline int mbedtls_sha1_ret(const unsigned char *input, size_t ilen, unsigned char output[20]) {
    return mbedtls_sha1(input, ilen, output);
}
#endif

#endif /* ALTAIR_MBEDTLS_COMPAT_H */
