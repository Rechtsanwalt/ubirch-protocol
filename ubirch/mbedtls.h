// When building for mbed, include this file to enable SHA256 and BASE64 (tests).

#ifndef MBEDTLS_H
#define MBEDTLS_H

#define MBEDTLS_SHA256_C    1
#define MBEDTLS_BASE64_C    1

#include <mbedtls/check_config.h>

#endif //MBEDTLS_H