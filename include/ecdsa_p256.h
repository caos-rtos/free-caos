// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ecdsa_p256.h
 * @brief          : ECDSA on NIST P-256 (secp256r1) with SHA-256
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Software ECDSA, the same code on every family. It exists for the TLS 1.3
 * server (CertificateVerify): browsers do not accept Ed25519 certificates,
 * P-256 is the curve they all take.
 *
 * Why not the PKA: on the H563 the PKA runs in limited mode, signature
 * verification only (RM0481 36.3), and a server must SIGN. The other
 * families have no PKA at all.
 *
 * The nonce k is deterministic (RFC 6979, HMAC-SHA256): a weak or repeated
 * RNG output cannot leak the private key, and the result is testable against
 * the published vectors.
 *
 * Formats (big-endian, as in X9.62 / TLS):
 *   private    32 bytes, integer d in [1, n-1]
 *   public     64 bytes X || Y (the 0x04 prefix of the SEC1 form is NOT
 *              included)
 *   signature  64 bytes r || s (raw; TLS wants it DER-encoded, that is up
 *              to the caller)
 *
 * Typical usage:
 *   u8 priv[32], pub[64], sig[64];
 *   ca_ecdsa_p256_ctx_t *c = ca_memory_allocate(sizeof(*c));
 *   ca_ecdsa_p256_keygen(c, pub, priv);
 *   ca_ecdsa_p256_sign(c, sig, msg, len, priv);
 *   bool ok = ca_ecdsa_p256_verify(c, sig, msg, len, pub);
 *   ca_memory_free(c, NULL, true);
 ******************************************************************************
 */

#ifndef CRYPTO_ECDSA_P256_H
#define CRYPTO_ECDSA_P256_H

#include "common.h"
#include "kdf.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define ECDSA_P256_PRIVATE_SIZE    32
#define ECDSA_P256_PUBLIC_SIZE     64
#define ECDSA_P256_SIGNATURE_SIZE  64
#define ECDSA_P256_HASH_SIZE       32   /* SHA-256 digest */
#define ECDSA_P256_LIMBS           8U   /* 32-bit limbs per number */

/* ============================================================================
 * WORKING CONTEXT - given by the caller to every function below
 *
 * Heap, static or the caller's own state: never needed on the task stack.
 * Every function wipes it before returning. On H7/F7 a context on the stack
 * (DTCM) is the fast choice, in heap (AXI SRAM) the light one for the stack;
 * size the task stack for it if you choose the stack.
 * The member types are the internals of the implementation: do not use them.
 * ========================================================================== */

typedef struct {
    u32 x[ECDSA_P256_LIMBS], y[ECDSA_P256_LIMBS], z[ECDSA_P256_LIMBS];
} ca_p256_pt_t;

typedef struct {
    ca_hmac_sha256_ctx_t hmac;          /* RFC 6979 HMAC_DRBG */
    u8   K[32];
    u8   V[32];
    bool started;
} ca_p256_rfc6979_t;

typedef struct {
    u32 d[ECDSA_P256_LIMBS], z[ECDSA_P256_LIMBS], k[ECDSA_P256_LIMBS];
    u32 r[ECDSA_P256_LIMBS], s[ECDSA_P256_LIMBS], km[ECDSA_P256_LIMBS];
    u32 t[ECDSA_P256_LIMBS], u[ECDSA_P256_LIMBS];
    u8  hb[32];
    ca_p256_rfc6979_t st;
    ca_p256_pt_t      R;
} ca_p256_sign_work_t;

typedef struct {
    u32 r[ECDSA_P256_LIMBS], s[ECDSA_P256_LIMBS], z[ECDSA_P256_LIMBS];
    u32 w[ECDSA_P256_LIMBS], u1[ECDSA_P256_LIMBS], u2[ECDSA_P256_LIMBS];
    u32 x[ECDSA_P256_LIMBS], lhs[ECDSA_P256_LIMBS], rhs[ECDSA_P256_LIMBS];
    u32 t[ECDSA_P256_LIMBS];
    ca_p256_pt_t Q, R;
} ca_p256_verify_work_t;

typedef struct {
    u32 d[ECDSA_P256_LIMBS], x[ECDSA_P256_LIMBS], y[ECDSA_P256_LIMBS];
    ca_p256_pt_t Q;
} ca_p256_pub_work_t;

/* One operation at a time: the four share the memory */
typedef union {
    ca_sha256_ctx_t       hash;         /* message digest of sign / verify */
    ca_p256_sign_work_t   sign;
    ca_p256_verify_work_t verify;
    ca_p256_pub_work_t    pub;
} ca_p256_work_t;

typedef struct {
    ca_p256_work_t w;
    u8             h[ECDSA_P256_HASH_SIZE];     /* digest kept across the union */
} ca_ecdsa_p256_ctx_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Derives the public key Q = d*G.
 * @param public_key  Output, 64 bytes X || Y
 * @param private_key Private key, 32 bytes
 * @return false if d is 0 or >= n (not a valid private key)
 */
bool ca_ecdsa_p256_public_from_private(ca_ecdsa_p256_ctx_t *ctx, u8 *public_key,
                                       const u8 *private_key);

/**
 * @brief Generates a key pair with the RNG.
 * @param public_key  Output, 64 bytes
 * @param private_key Output, 32 bytes - to be kept secure (keys_store)
 * @return false if the RNG gave no entropy
 */
bool ca_ecdsa_p256_keygen(ca_ecdsa_p256_ctx_t *ctx, u8 *public_key, u8 *private_key);

/**
 * @brief Signs a SHA-256 digest (RFC 6979 deterministic nonce).
 * @param signature   Output, 64 bytes r || s
 * @param hash        Message digest, 32 bytes
 * @param private_key Private key, 32 bytes
 * @return false if the private key is not valid
 */
bool ca_ecdsa_p256_sign_hash(ca_ecdsa_p256_ctx_t *ctx, u8 *signature,
                             const u8 *hash, const u8 *private_key);

/**
 * @brief Hashes the message with SHA-256 and signs the digest.
 */
bool ca_ecdsa_p256_sign(ca_ecdsa_p256_ctx_t *ctx, u8 *signature,
                        const u8 *message, u32 message_len, const u8 *private_key);

/**
 * @brief Verifies a signature over a SHA-256 digest.
 *
 * Rejects r or s outside [1, n-1] and a public key that is not on the curve.
 *
 * @return true if the signature is valid
 */
bool ca_ecdsa_p256_verify_hash(ca_ecdsa_p256_ctx_t *ctx, const u8 *signature,
                               const u8 *hash, const u8 *public_key);

/**
 * @brief Hashes the message with SHA-256 and verifies the signature.
 */
bool ca_ecdsa_p256_verify(ca_ecdsa_p256_ctx_t *ctx, const u8 *signature,
                          const u8 *message, u32 message_len, const u8 *public_key);

#if EN_ECDSA_P256_TEST
/**
 * @brief Runs the RFC 6979 A.2.5 vectors plus negative cases, prints to stdout.
 * @return Number of failed tests (0 = all passed)
 */
u32 ca_ecdsa_p256_selftest(void);

/**
 * @brief Registers the CLI command "p256" (test | keygen).
 */
void ca_ecdsa_p256_test_init(void);
#endif /* EN_ECDSA_P256_TEST */

#endif /* CRYPTO_ECDSA_P256_H */
