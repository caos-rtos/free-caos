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
 *   ca_ecdsa_p256_keygen(pub, priv);
 *   ca_ecdsa_p256_sign(sig, msg, len, priv);
 *   bool ok = ca_ecdsa_p256_verify(sig, msg, len, pub);
 ******************************************************************************
 */

#ifndef CRYPTO_ECDSA_P256_H
#define CRYPTO_ECDSA_P256_H

#include "common.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define ECDSA_P256_PRIVATE_SIZE    32
#define ECDSA_P256_PUBLIC_SIZE     64
#define ECDSA_P256_SIGNATURE_SIZE  64
#define ECDSA_P256_HASH_SIZE       32   /* SHA-256 digest */

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Derives the public key Q = d*G.
 * @param public_key  Output, 64 bytes X || Y
 * @param private_key Private key, 32 bytes
 * @return false if d is 0 or >= n (not a valid private key)
 */
bool ca_ecdsa_p256_public_from_private(u8 *public_key, const u8 *private_key);

/**
 * @brief Generates a key pair with the RNG.
 * @param public_key  Output, 64 bytes
 * @param private_key Output, 32 bytes - to be kept secure (keys_store)
 * @return false if the RNG gave no entropy
 */
bool ca_ecdsa_p256_keygen(u8 *public_key, u8 *private_key);

/**
 * @brief Signs a SHA-256 digest (RFC 6979 deterministic nonce).
 * @param signature   Output, 64 bytes r || s
 * @param hash        Message digest, 32 bytes
 * @param private_key Private key, 32 bytes
 * @return false if the private key is not valid
 */
bool ca_ecdsa_p256_sign_hash(u8 *signature, const u8 *hash,
                             const u8 *private_key);

/**
 * @brief Hashes the message with SHA-256 and signs the digest.
 */
bool ca_ecdsa_p256_sign(u8 *signature, const u8 *message, u32 message_len,
                        const u8 *private_key);

/**
 * @brief Verifies a signature over a SHA-256 digest.
 *
 * Rejects r or s outside [1, n-1] and a public key that is not on the curve.
 *
 * @return true if the signature is valid
 */
bool ca_ecdsa_p256_verify_hash(const u8 *signature, const u8 *hash,
                               const u8 *public_key);

/**
 * @brief Hashes the message with SHA-256 and verifies the signature.
 */
bool ca_ecdsa_p256_verify(const u8 *signature, const u8 *message,
                          u32 message_len, const u8 *public_key);

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
