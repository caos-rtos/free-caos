// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : chacha20.h
 * @brief          : ChaCha20 stream cipher and HChaCha20 (RFC 8439)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Software ChaCha20, the same on every family (no crypto IP implements it).
 *
 *   ChaCha20    RFC 8439 2.3/2.4    256-bit key, 96-bit nonce, 32-bit counter
 *   HChaCha20   draft-irtf-cfrg-xchacha 2.2   key + 128-bit nonce -> subkey
 *
 * Two ways to set the nonce:
 *   ca_chacha20_init()     IETF nonce of 12 bytes and a starting counter
 *   ca_chacha20_init64()   WireGuard: 32 zero bits || 64-bit little-endian
 *                          counter (the packet counter, not the block one),
 *                          block counter 0
 *
 * The working state and the keystream block live in the context, never on
 * the stack (crypto rule 19/11): the caller picks heap, static or its own
 * state. Enabled by EN_CHACHA20_POLY1305; known answers by
 * EN_CHACHA20_POLY1305_TEST (ca_chacha20poly1305_selftest).
 ******************************************************************************
 */

#ifndef CRYPTO_CHACHA20_H
#define CRYPTO_CHACHA20_H

#include "common.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define CHACHA20_KEY_SIZE       32      /* Key in bytes */
#define CHACHA20_NONCE_SIZE     12      /* IETF nonce in bytes */
#define CHACHA20_BLOCK_SIZE     64      /* Keystream block in bytes */
#define HCHACHA20_NONCE_SIZE    16      /* HChaCha20 nonce in bytes */
#define HCHACHA20_OUT_SIZE      32      /* HChaCha20 subkey in bytes */

/* ============================================================================
 * DATA TYPES
 * ========================================================================== */

typedef struct {
    u32 state[16];                      /* constants | key | counter | nonce */
    u32 work[16];                       /* working state of one block */
    u8  stream[CHACHA20_BLOCK_SIZE];    /* keystream of the current block */
} ca_chacha20_ctx_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Loads key, IETF nonce and starting block counter (RFC 8439 2.3).
 * @param ctx     Context (not NULL)
 * @param key     CHACHA20_KEY_SIZE bytes
 * @param nonce   CHACHA20_NONCE_SIZE bytes, never repeated with the same key
 * @param counter First block counter (0, or 1 for the AEAD payload)
 */
void ca_chacha20_init(ca_chacha20_ctx_t *ctx, const u8 *key, const u8 *nonce, u32 counter);

/**
 * @brief Loads key and a 64-bit nonce, block counter 0 (WireGuard).
 *        Same as ca_chacha20_init() with nonce = 4 zero bytes || LE64(nonce).
 * @param ctx   Context (not NULL)
 * @param key   CHACHA20_KEY_SIZE bytes
 * @param nonce 64-bit counter of the upper layer
 */
void ca_chacha20_init64(ca_chacha20_ctx_t *ctx, const u8 *key, u64 nonce);

/**
 * @brief XORs len bytes of keystream into in -> out (encrypts and decrypts).
 *
 * Every call starts on a NEW block: the unused tail of the last block is
 * discarded, so a stream split across calls must be split on multiples of
 * CHACHA20_BLOCK_SIZE. The AEAD relies on it (block 0 = Poly1305 key, the
 * payload from block 1). in and out may be the same buffer.
 *
 * @param ctx Initialized context; the block counter advances
 * @param out Output, len bytes
 * @param in  Input, len bytes
 * @param len Number of bytes
 */
void ca_chacha20(ca_chacha20_ctx_t *ctx, u8 *out, const u8 *in, u32 len);

/**
 * @brief HChaCha20: derives a 256-bit subkey from key and a 128-bit nonce
 *        (the first step of XChaCha20).
 * @param wk    Working context from the caller; zeroed on return
 * @param out   HCHACHA20_OUT_SIZE bytes
 * @param key   CHACHA20_KEY_SIZE bytes
 * @param nonce HCHACHA20_NONCE_SIZE bytes
 */
void ca_hchacha20(ca_chacha20_ctx_t *wk, u8 *out, const u8 *key, const u8 *nonce);

#endif /* CRYPTO_CHACHA20_H */
