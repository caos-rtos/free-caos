// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : sha256.h
 * @brief          : SHA-256 software implementation (FIPS 180-4)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Pure software SHA-256, used as a fallback for the HASH accelerator on
 * families that do not have it (F1xx/F4xx/F7xx and H747). The ca_hash_*
 * driver falls back to this implementation when ca_hash_hw_start() returns false,
 * so SSH (SHA-256 exchange hash) and key derivation work everywhere.
 *
 * One-shot usage:
 *   ca_sha256(msg, len, digest);
 *
 * Incremental usage:
 *   ca_sha256_init(&ctx);
 *   ca_sha256_update(&ctx, part1, n1);
 *   ca_sha256_update(&ctx, part2, n2);
 *   ca_sha256_final(&ctx, digest);
 ******************************************************************************
 */

#ifndef CRYPTO_SHA256_H
#define CRYPTO_SHA256_H

#include "common.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define SHA256_DIGEST_SIZE      32      /* Digest in bytes */
#define SHA256_BLOCK_SIZE       64      /* Compression block in bytes */

/* ============================================================================
 * DATA TYPES
 * ========================================================================== */

typedef struct {
    u32 state[8];                   /* Internal state H0..H7 */
    u64 bitlen;                     /* Message length in bits (64 bits) */
    u8  buffer[SHA256_BLOCK_SIZE];  /* Partial block in accumulation */
    u32 buflen;                     /* Valid bytes in buffer */
    u32 W[64];                      /* Message schedule — in ctx, not on stack:
                                     * SW compression is deep and on targets
                                     * without HW HASH (F407) it would burden the stack. */
} ca_sha256_ctx_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Initializes the context with the IV values from FIPS 180-4.
 * @param ctx Context to initialize (not NULL)
 */
void ca_sha256_init(ca_sha256_ctx_t *ctx);

/**
 * @brief Accumulates data in the context. Can be called multiple times.
 * @param ctx  Initialized context
 * @param data Data to absorb (can be NULL if len == 0)
 * @param len  Number of bytes
 */
void ca_sha256_update(ca_sha256_ctx_t *ctx, const u8 *data, u32 len);

/**
 * @brief Applies padding and produces the final digest.
 * @param ctx    Context (zeroed on exit: contains sensitive state)
 * @param digest Output buffer, SHA256_DIGEST_SIZE bytes
 */
void ca_sha256_final(ca_sha256_ctx_t *ctx, u8 *digest);

/**
 * @brief One-shot calculation: init + update + final.
 * @param data   Message
 * @param len    Length in bytes
 * @param digest Output buffer, SHA256_DIGEST_SIZE bytes
 */
void ca_sha256(const u8 *data, u32 len, u8 *digest);

#if EN_SHA256_TEST
/**
 * @brief Executes the NIST test vectors (FIPS 180-4) and prints the result to stdout.
 *
 * Covers: empty message, "abc", 2-block vector, 1,000,000 'a's,
 * incremental/one-shot equivalence and padding boundaries (55..65 bytes).
 *
 * @return Number of failed tests (0 = all passed)
 */
u32 ca_sha256_selftest(void);

/**
 * @brief Registers the CLI command "sha256" (test | help).
 *        Can be called multiple times: registration occurs only once.
 */
void ca_sha256_test_init(void);
#endif /* EN_SHA256_TEST */

#endif /* CRYPTO_SHA256_H */
