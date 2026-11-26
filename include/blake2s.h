// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : blake2s.h
 * @brief          : BLAKE2s hash (RFC 7693) and HMAC-BLAKE2s (RFC 2104)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 *   BLAKE2s        RFC 7693    digest 1..32 bytes, optional key 0..32 bytes
 *   HMAC-BLAKE2s   RFC 2104    B = 64, L = 32 (WireGuard KDF1/2/3)
 *
 * Keyed BLAKE2s and HMAC-BLAKE2s are different functions: WireGuard uses
 * the first for MAC1/MAC2 and the second for the Noise key derivation.
 *
 * The working buffers (v[16], m[16] of the compression) are in the context,
 * never on the stack (crypto rule 19/11). final() zeroes the context.
 * Enabled by EN_BLAKE2S; known answers by EN_BLAKE2S_TEST, CLI "blake2s"
 * by EN_CLI_BLAKE2S.
 ******************************************************************************
 */

#ifndef CRYPTO_BLAKE2S_H
#define CRYPTO_BLAKE2S_H

#include "common.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define BLAKE2S_BLOCK_SIZE      64      /* Compression block in bytes */
#define BLAKE2S_OUT_SIZE        32      /* Largest digest in bytes */
#define BLAKE2S_KEY_SIZE        32      /* Largest key in bytes */
#define HMAC_BLAKE2S_SIZE       32      /* HMAC-BLAKE2s output in bytes */

/* ============================================================================
 * DATA TYPES
 * ========================================================================== */

typedef struct {
    u8  b[BLAKE2S_BLOCK_SIZE];          /* input buffer */
    u32 h[8];                           /* chained state */
    u32 t[2];                           /* total bytes, 64-bit counter */
    u32 c;                              /* bytes in b[] */
    u32 outlen;                         /* digest size */
    u32 v[16];                          /* compression: working vector */
    u32 m[16];                          /* compression: message words */
} ca_blake2s_ctx_t;

typedef struct {
    ca_blake2s_ctx_t h;                 /* inner, then outer hash */
    u8 k[BLAKE2S_BLOCK_SIZE];           /* key padded to one block */
    u8 inner[BLAKE2S_OUT_SIZE];         /* inner digest */
} ca_hmac_blake2s_ctx_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Initializes the context, with an optional key (keyed BLAKE2s).
 * @param ctx    Context (not NULL)
 * @param outlen Digest size, 1..BLAKE2S_OUT_SIZE
 * @param key    Key (NULL if keylen == 0)
 * @param keylen 0..BLAKE2S_KEY_SIZE
 * @return false on an illegal size (context untouched)
 */
bool ca_blake2s_init(ca_blake2s_ctx_t *ctx, u32 outlen, const u8 *key, u32 keylen);

/**
 * @brief Absorbs len bytes. Can be called multiple times, any split.
 * @param ctx Initialized context
 * @param in  Data (can be NULL if len == 0)
 * @param len Number of bytes
 */
void ca_blake2s_update(ca_blake2s_ctx_t *ctx, const u8 *in, u32 len);

/**
 * @brief Last block and digest (outlen bytes, size given to init).
 * @param ctx Context (zeroed on exit)
 * @param out Output, outlen bytes
 */
void ca_blake2s_final(ca_blake2s_ctx_t *ctx, u8 *out);

/**
 * @brief One-shot: init + update + final.
 * @param ctx    Working context from the caller; zeroed on return
 * @param out    Output, outlen bytes
 * @param outlen 1..BLAKE2S_OUT_SIZE
 * @param in     Data
 * @param len    Data length
 * @param key    Key (NULL if keylen == 0)
 * @param keylen 0..BLAKE2S_KEY_SIZE
 * @return false on an illegal size or key == NULL with keylen > 0
 */
bool ca_blake2s(ca_blake2s_ctx_t *ctx, u8 *out, u32 outlen, const u8 *in, u32 len,
                const u8 *key, u32 keylen);

/**
 * @brief HMAC-BLAKE2s: starts the inner hash. A key longer than one block
 *        (64 bytes) is hashed first, as RFC 2104 says.
 * @param ctx    Context (not NULL)
 * @param key    Key (NULL if keylen == 0)
 * @param keylen Any length
 */
void ca_hmac_blake2s_init(ca_hmac_blake2s_ctx_t *ctx, const u8 *key, u32 keylen);

/**
 * @brief HMAC-BLAKE2s: absorbs len bytes into the inner hash.
 */
void ca_hmac_blake2s_update(ca_hmac_blake2s_ctx_t *ctx, const u8 *in, u32 len);

/**
 * @brief HMAC-BLAKE2s: outer hash and tag.
 * @param ctx Context (zeroed on exit)
 * @param out Output, HMAC_BLAKE2S_SIZE bytes
 */
void ca_hmac_blake2s_final(ca_hmac_blake2s_ctx_t *ctx, u8 *out);

/**
 * @brief HMAC-BLAKE2s one-shot.
 * @param ctx Working context from the caller; zeroed on return
 * @param out Output, HMAC_BLAKE2S_SIZE bytes
 */
void ca_hmac_blake2s(ca_hmac_blake2s_ctx_t *ctx, u8 *out, const u8 *key, u32 keylen,
                     const u8 *in, u32 len);

#if EN_BLAKE2S_TEST
/**
 * @brief Known-answer tests of BLAKE2s (RFC 7693 Appendix B and C, keyed,
 *        block boundaries) and HMAC-BLAKE2s, printed to stdout.
 * @return Number of failed tests (0 = all passed)
 */
u32 ca_blake2s_selftest(void);

/**
 * @brief Registers the CLI command "blake2s" (test | speed).
 *        Can be called multiple times: registration occurs only once.
 */
void ca_blake2s_test_init(void);
#endif /* EN_BLAKE2S_TEST */

#endif /* CRYPTO_BLAKE2S_H */
