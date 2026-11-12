/*
 * mbedtls_config.h - our build of mbedTLS 3.6 (third_party/, fetched by
 * make): a TLS 1.2 client in memory with a client certificate, as an
 * Android Auto head unit needs (android_auto: the phone is the TLS server,
 * the head unit authenticates with its certificate).
 *
 * Used as MBEDTLS_CONFIG_FILE for both builds of the library (Makefile:
 * build/mbedtls-sh4/ for the car, build/mbedtls-host/ for the host tests
 * and tools): add -Isrc/common/mbedtls. The host build adds the server
 * side, which test_android_auto's fake phone uses.
 *
 * Not here on purpose:
 *   - platform entropy: the unit has no /dev/random; tls.cpp adds its own
 *     source (MBEDTLS_NO_PLATFORM_ENTROPY)
 *   - time / dates: the phone's certificate is not verified (the phone does
 *     the verifying), and the unit's clock starts at 1970
 *   - sockets, files, threads, timing, PSA: the library only sees memory
 *   - assembly: there is none for SH-4, and the host build stays the same
 *     code as the car's
 */
#ifndef COMMON_MBEDTLS_CONFIG_H
#define COMMON_MBEDTLS_CONFIG_H

/* system */
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#if !defined(__QNX__)
#define MBEDTLS_SSL_SRV_C              /* the tests' fake phone */
#endif
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* certificates and keys (PEM in memory) */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

/* public key */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* symmetric */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_MODE_CBC

/* hashes */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* random */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* diagnostics: mbedtls_strerror() and the handshake trace (tls.cpp) */
#define MBEDTLS_ERROR_C
#define MBEDTLS_DEBUG_C

#endif
