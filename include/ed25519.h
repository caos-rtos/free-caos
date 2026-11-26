// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ed25519.h
 * @brief          : Ed25519 signature scheme (RFC 8032)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Digital signature on twisted Edwards curve, same GF(2^255-19) field as
 * X25519 (arithmetic reused from x25519.h) and SHA-512 as internal hash.
 *
 * In the SSH server stack it covers two distinct roles:
 *   - host key   : the server signs the exchange hash, the client verifies
 *   - user auth  : the client signs, the server verifies (publickey auth)
 *
 * Key format as per RFC 8032 / OpenSSH:
 *   seed       32 bytes raw secret material (from RNG)
 *   public     32 bytes compressed point
 *   signature  64 bytes R (32) || S (32)
 *
 * Note on nomenclature: what OpenSSH saves as a 64-byte "private key"
 * is seed||public. Here the 32-byte seed is kept and the public one is
 * derived when needed.
 *
 * Typical usage (host key generated at startup):
 *   u8 seed[32], pub[32], sig[64];
 *   ca_rng_fill(seed, 32);
 *   ca_ed25519_public_from_seed(pub, seed);
 *   ca_ed25519_sign(sig, msg, msg_len, seed, pub);
 *   bool ok = ca_ed25519_verify(sig, msg, msg_len, pub);
 ******************************************************************************
 */

#ifndef CRYPTO_ED25519_H
#define CRYPTO_ED25519_H

#include "common.h"
#include "sha512.h"
#include "x25519.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define ED25519_SEED_SIZE       32
#define ED25519_PUBLIC_SIZE     32
#define ED25519_SIGNATURE_SIZE  64

/* ============================================================================
 * WORKING CONTEXT - given by the caller to every function below
 *
 * Heap, static or the caller's own state: never needed on the task stack.
 * Every function wipes it before returning. On H7/F7 a context on the stack
 * (DTCM) is the fast choice, in heap (AXI SRAM) the light one for the stack.
 * The member types are the internals of the implementation: do not use them.
 * ========================================================================== */

typedef struct { ca_fe25519_t X, Y, Z, T; } ca_ge_p3_t;          /* extended */
typedef struct { ca_fe25519_t X, Y, Z; } ca_ge_p2_t;             /* projective */
typedef struct { ca_fe25519_t X, Y, Z, T; } ca_ge_p1p1_t;        /* add/dbl result */
typedef struct { ca_fe25519_t YplusX, YminusX, T2d, Z2; } ca_ge_cached_t;

/* r = a * P (constant time) */
typedef struct {
    ca_ge_p3_t     base, result, sum;
    ca_ge_cached_t pc;
    ca_ge_p1p1_t   t, tadd;
} ca_ge_smul_work_t;

/* r = a*A + b*B (verification) */
typedef struct {
    ca_ge_p3_t     bp, acc, tmp;
    ca_ge_cached_t ac, bc;
    ca_ge_p1p1_t   t;
} ca_ge_dsmul_work_t;

typedef struct {
    ca_sha512_ctx_t   hash;
    u8                h[64], nonce[64], hram[64], az[32];
    ca_ge_p3_t        R;
    ca_ge_smul_work_t sm;
} ca_ed25519_sign_work_t;

typedef struct {
    ca_sha512_ctx_t    hash;
    u8                 hram[64], checkr[32];
    ca_ge_p3_t         A;
    ca_ge_p2_t         R;
    ca_ge_dsmul_work_t dsm;
} ca_ed25519_verify_work_t;

typedef struct {
    ca_sha512_ctx_t   hash;
    u8                h[64];
    ca_ge_p3_t        A;
    ca_ge_smul_work_t sm;
} ca_ed25519_pub_work_t;

/* One operation at a time: the three share the memory */
typedef union {
    ca_ed25519_sign_work_t   sign;
    ca_ed25519_verify_work_t verify;
    ca_ed25519_pub_work_t    pub;
} ca_ed25519_ctx_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Derives the public key from the seed.
 * @param public_key Output, 32 bytes
 * @param seed       Secret seed, 32 bytes
 */
void ca_ed25519_public_from_seed(ca_ed25519_ctx_t *ctx, u8 *public_key, const u8 *seed);

/**
 * @brief Generates a key pair using the hardware RNG.
 *
 * Fails if the RNG is not available (e.g. F1xx, lacking the peripheral):
 * in that case it does NOT produce weak keys, it returns false.
 *
 * @param public_key Output, 32 bytes
 * @param seed       Output, 32 bytes — to be kept secure (keys_store)
 * @return true if the RNG provided valid entropy
 */
bool ca_ed25519_keygen(ca_ed25519_ctx_t *ctx, u8 *public_key, u8 *seed);

/**
 * @brief Signs a message (pure Ed25519, RFC 8032 §5.1.6).
 *
 * Deterministic: nonce derived from SHA-512(seed) and the message, no
 * RNG involved — there is no risk of nonce reuse.
 *
 * @param signature  Output, 64 bytes (R || S)
 * @param message    Message to sign
 * @param message_len Length in bytes (0 allowed)
 * @param seed       Secret seed, 32 bytes
 * @param public_key Corresponding public key, 32 bytes
 */
void ca_ed25519_sign(ca_ed25519_ctx_t *ctx, u8 *signature,
                     const u8 *message, u32 message_len,
                     const u8 *seed, const u8 *public_key);

/**
 * @brief Verifies a signature (RFC 8032 §5.1.7).
 *
 * Rejects signatures with non-canonical S (S >= L), as required to avoid
 * malleability: without this check, other equally valid signatures for the
 * same message could be derived from a valid signature.
 *
 * @param signature   Signature, 64 bytes
 * @param message     Message
 * @param message_len Length in bytes
 * @param public_key  Signer's public key, 32 bytes
 * @return true if the signature is valid
 */
bool ca_ed25519_verify(ca_ed25519_ctx_t *ctx, const u8 *signature,
                       const u8 *message, u32 message_len, const u8 *public_key);

#if EN_ED25519_TEST
/**
 * @brief Executes the RFC 8032 test vectors and prints the result to stdout.
 * @return Number of failed tests (0 = all passed)
 */
u32 ca_ed25519_selftest(void);

/**
 * @brief Registers the CLI command "ed25519" (test | keygen | help).
 */
void ca_ed25519_test_init(void);
#endif /* EN_ED25519_TEST */

#endif /* CRYPTO_ED25519_H */
