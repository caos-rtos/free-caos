// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : poly1305.h
 * @brief          : Poly1305 one-time authenticator (RFC 8439 2.5)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * 32-bit implementation (26-bit limbs, 32x32->64 products), constant time.
 * The 32-byte key (r || s) authenticates ONE message only: in the AEAD it is
 * the first ChaCha20 block of each nonce. Enabled by EN_CHACHA20_POLY1305.
 ******************************************************************************
 */

#ifndef CRYPTO_POLY1305_H
#define CRYPTO_POLY1305_H

#include "common.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define POLY1305_KEY_SIZE       32      /* One-time key r || s in bytes */
#define POLY1305_TAG_SIZE       16      /* Tag in bytes */
#define POLY1305_BLOCK_SIZE     16      /* Message block in bytes */

/* ============================================================================
 * DATA TYPES
 * ========================================================================== */

typedef struct {
    u32 r[5];                           /* clamped r, 26-bit limbs */
    u32 h[5];                           /* accumulator, 26-bit limbs */
    u32 pad[4];                         /* s, added at the end */
    u32 leftover;                       /* valid bytes in buffer */
    u8  buffer[POLY1305_BLOCK_SIZE];    /* partial block */
    u8  final;                          /* last (padded) block in progress */
} ca_poly1305_ctx_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Loads the one-time key: r clamped as RFC 8439 2.5, s kept for the end.
 * @param ctx Context (not NULL)
 * @param key POLY1305_KEY_SIZE bytes
 */
void ca_poly1305_init(ca_poly1305_ctx_t *ctx, const u8 *key);

/**
 * @brief Absorbs len bytes. Can be called multiple times, any split.
 * @param ctx Initialized context
 * @param m   Message (can be NULL if len == 0)
 * @param len Number of bytes
 */
void ca_poly1305_update(ca_poly1305_ctx_t *ctx, const u8 *m, u32 len);

/**
 * @brief Pads and processes the last block, writes the tag.
 * @param ctx Context (zeroed on exit: holds the key)
 * @param tag Output, POLY1305_TAG_SIZE bytes
 */
void ca_poly1305_final(ca_poly1305_ctx_t *ctx, u8 *tag);

#endif /* CRYPTO_POLY1305_H */
