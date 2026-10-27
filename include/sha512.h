// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : sha512.h
 * @brief          : SHA-512 software implementation (FIPS 180-4)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Pure software SHA-512: the STM32 HASH accelerator (ca_hash_*) covers
 * only SHA-1 and SHA-256, while Ed25519 (RFC 8032) requires SHA-512 both in
 * key derivation and signing.
 *
 * One-shot usage:
 *   ca_sha512(msg, len, digest);
 *
 * Incremental usage (for multi-part messages, e.g., Ed25519 dom2):
 *   ca_sha512_init(&ctx);
 *   ca_sha512_update(&ctx, part1, n1);
 *   ca_sha512_update(&ctx, part2, n2);
 *   ca_sha512_final(&ctx, digest);
 ******************************************************************************
 */

#ifndef CRYPTO_SHA512_H
#define CRYPTO_SHA512_H

#include "common.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define SHA512_DIGEST_SIZE      64      /* Digest in bytes */
#define SHA512_BLOCK_SIZE       128     /* Compression block in bytes */

/* ============================================================================
 * DATA TYPES
 * ========================================================================== */

typedef struct {
    u64 state[8];                   /* Internal state H0..H7 */
    u64 bitlen_lo;                  /* Message length in bits (low part) */
    u64 bitlen_hi;                  /* Message length in bits (high part) */
    u8  buffer[SHA512_BLOCK_SIZE];  /* Partial block in accumulation */
    u32 buflen;                     /* Valid bytes in buffer */
    u64 W[80];                      /* Message schedule — in ctx, not on stack:
                                     * 640B that would burden the task stack
                                     * (Ed25519 calls SHA-512 deeply). */
} ca_sha512_ctx_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Initializes the context with FIPS 180-4 IV values.
 * @param ctx Context to initialize (not NULL)
 */
void ca_sha512_init(ca_sha512_ctx_t *ctx);

/**
 * @brief Accumulates data in the context. Can be called multiple times.
 * @param ctx  Initialized context
 * @param data Data to absorb (can be NULL if len == 0)
 * @param len  Number of bytes
 */
void ca_sha512_update(ca_sha512_ctx_t *ctx, const u8 *data, u32 len);

/**
 * @brief Applies padding and produces the final digest.
 * @param ctx    Context (zeroed on exit: contains sensitive state)
 * @param digest Output buffer, SHA512_DIGEST_SIZE bytes
 */
void ca_sha512_final(ca_sha512_ctx_t *ctx, u8 *digest);

/**
 * @brief One-shot calculation: init + update + final.
 * @param data   Message
 * @param len    Length in bytes
 * @param digest Output buffer, SHA512_DIGEST_SIZE bytes
 */
void ca_sha512(const u8 *data, u32 len, u8 *digest);

#if EN_SHA512_TEST
/**
 * @brief Executes NIST test vectors (FIPS 180-4) and prints the result to stdout.
 *
 * Covers: empty message, "abc", 2-block vector, 1,000,000 'a's,
 * incremental/one-shot equivalence and padding boundaries (111..129 bytes).
 *
 * @return Number of failed tests (0 = all passed)
 */
u32 ca_sha512_selftest(void);

/**
 * @brief Registers the CLI command "sha512" (test | help).
 *        Can be called multiple times: registration occurs only once.
 */
void ca_sha512_test_init(void);
#endif /* EN_SHA512_TEST */

#endif /* CRYPTO_SHA512_H */
