// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : x25519.h
 * @brief          : Curve25519 field arithmetic + X25519 ECDH (RFC 7748)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Arithmetic on the GF(2^255 - 19) field and scalar multiplication on a
 * Montgomery curve, the basis of the "curve25519-sha256" SSH key exchange.
 *
 * The field uses 10 limbs of alternating 25/26 bits: it avoids 64x64
 * multiplications and remains efficient on Cortex-M without assembly. The
 * fe25519 type and its arithmetic, reused by Ed25519 (Edwards curve on the
 * same field), live in the private header crypto/src/fe25519.h.
 *
 * CONSTANT TIME: the Montgomery ladder has no branches or memory accesses
 * dependent on the secret scalar. Conditional swaps use arithmetic masks
 * (fe25519_cswap), not ifs.
 *
 * Typical usage (SSH server side):
 *   u8 priv[32], pub[32], shared[32];
 *   ca_rng_fill(priv, 32);                        // ephemeral scalar
 *   ca_x25519_base(pub, priv);                    // public key
 *   ca_x25519(shared, priv, peer_pub);            // shared secret
 ******************************************************************************
 */

#ifndef CRYPTO_X25519_H
#define CRYPTO_X25519_H

#include "common.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define X25519_KEY_SIZE     32      /* Scalars and points: 32 bytes */

/* Element of GF(2^255 - 19), 10 limbs (radix 2^25.5). Public only because
 * it is part of the working contexts below; the arithmetic stays private
 * (crypto/src/fe25519.h). */
typedef struct {
    i32 v[10];
} ca_fe25519_t;

/* X25519 working context, given by the caller (heap, static or its own
 * state; on the stack it is the fast choice on H7/F7). Wiped on return. */
typedef struct {
    u8           e[32];                 /* clamped scalar */
    ca_fe25519_t x1, x2, z2, x3, z3, tmp0, tmp1;
} ca_x25519_ctx_t;

/* ============================================================================
 * PUBLIC API — X25519
 * ========================================================================== */

/**
 * @brief Scalar multiplication X25519: out = scalar * point.
 *
 * The scalar is "clamped" according to RFC 7748 (bits 0-2 cleared, bit 255
 * cleared, bit 254 set) before use: the caller can pass 32 raw random bytes.
 *
 * @param out    Shared secret, 32 bytes
 * @param scalar Private scalar, 32 bytes
 * @param point  u-coordinate of the peer point, 32 bytes
 * @return true if the output is not all zeros (small order point),
 *         false otherwise: in a handshake, the result must be rejected.
 */
bool ca_x25519(ca_x25519_ctx_t *ctx, u8 *out, const u8 *scalar, const u8 *point);

/**
 * @brief Public key from base point: out = scalar * 9.
 * @param out    Public key, 32 bytes
 * @param scalar Private scalar, 32 bytes
 */
void ca_x25519_base(ca_x25519_ctx_t *ctx, u8 *out, const u8 *scalar);

/* Field arithmetic (fe25519_*, shared with Ed25519): crypto/src/fe25519.h,
 * private to the crypto sources. */

#if EN_X25519_TEST
/**
 * @brief Executes the RFC 7748 test vectors and prints the result to stdout.
 * @return Number of failed tests (0 = all passed)
 */
u32 ca_x25519_selftest(void);

/**
 * @brief Registers the CLI command "x25519" (test | help).
 */
void ca_x25519_test_init(void);
#endif /* EN_X25519_TEST */

#endif /* CRYPTO_X25519_H */
