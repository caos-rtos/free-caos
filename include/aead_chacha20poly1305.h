// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : aead_chacha20poly1305.h
 * @brief          : ChaCha20-Poly1305 and XChaCha20-Poly1305 AEAD
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 *   ChaCha20-Poly1305    RFC 8439 2.8       96-bit nonce (IETF)
 *                        WireGuard          64-bit counter as nonce
 *   XChaCha20-Poly1305   draft-irtf-cfrg-xchacha   192-bit nonce
 *                        (WireGuard cookie reply)
 *
 * Output of encrypt: ciphertext (src_len bytes) || tag (16 bytes), so dst
 * must hold src_len + CHACHA20POLY1305_TAG_SIZE. decrypt takes the same
 * layout (src_len includes the tag), checks the tag in constant time BEFORE
 * decrypting, and on a wrong tag writes zeros and returns false. dst may be
 * src (in place).
 *
 * The working state (ChaCha20, Poly1305, one-time key, subkey) is the
 * caller's ca_chacha20poly1305_ctx_t (crypto rule 19/11): in heap it is
 * wiped with ca_memory_free(wk, NULL, true); on the stack it is faster and
 * costs ~250 B. Every function zeroes it before returning.
 *
 * Enabled by EN_CHACHA20_POLY1305; known answers by
 * EN_CHACHA20_POLY1305_TEST, CLI "chacha" by EN_CLI_CHACHA20_POLY1305.
 ******************************************************************************
 */

#ifndef CRYPTO_AEAD_CHACHA20POLY1305_H
#define CRYPTO_AEAD_CHACHA20POLY1305_H

#include "common.h"
#include "chacha20.h"
#include "poly1305.h"

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define CHACHA20POLY1305_KEY_SIZE       32      /* Key in bytes */
#define CHACHA20POLY1305_TAG_SIZE       16      /* Tag appended to the ciphertext */
#define CHACHA20POLY1305_NONCE_SIZE     12      /* IETF nonce in bytes */
#define XCHACHA20POLY1305_NONCE_SIZE    24      /* XChaCha nonce in bytes */

/* ============================================================================
 * DATA TYPES
 * ========================================================================== */

typedef struct {
    ca_chacha20_ctx_t  chacha;                          /* keystream */
    ca_poly1305_ctx_t  poly;                            /* authenticator */
    u8 poly_key[POLY1305_KEY_SIZE];                     /* one-time key, block 0 */
    u8 tag[CHACHA20POLY1305_TAG_SIZE];                  /* computed tag */
    u8 lens[16];                                        /* LE64(ad_len) || LE64(ct_len) */
    u8 nonce[CHACHA20POLY1305_NONCE_SIZE];              /* derived nonce (u64, XChaCha) */
    u8 subkey[HCHACHA20_OUT_SIZE];                      /* XChaCha subkey */
} ca_chacha20poly1305_ctx_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief AEAD encryption with a 96-bit nonce (RFC 8439 2.8).
 * @param wk      Working context from the caller; zeroed on return
 * @param dst     Output: src_len bytes of ciphertext + 16 of tag
 * @param src     Plaintext (can be NULL if src_len == 0)
 * @param src_len Plaintext length
 * @param ad      Additional data, authenticated not encrypted (NULL if ad_len == 0)
 * @param ad_len  Additional data length
 * @param nonce   CHACHA20POLY1305_NONCE_SIZE bytes, never repeated with the same key
 * @param key     CHACHA20POLY1305_KEY_SIZE bytes
 */
void ca_chacha20poly1305_encrypt_ietf(ca_chacha20poly1305_ctx_t *wk, u8 *dst,
                                      const u8 *src, u32 src_len,
                                      const u8 *ad, u32 ad_len,
                                      const u8 *nonce, const u8 *key);

/**
 * @brief AEAD decryption with a 96-bit nonce (RFC 8439 2.8).
 * @param src_len Ciphertext length INCLUDING the 16-byte tag
 * @return true = authentic, dst holds src_len - 16 bytes of plaintext;
 *         false = tag wrong or src_len < 16, dst zeroed
 */
bool ca_chacha20poly1305_decrypt_ietf(ca_chacha20poly1305_ctx_t *wk, u8 *dst,
                                      const u8 *src, u32 src_len,
                                      const u8 *ad, u32 ad_len,
                                      const u8 *nonce, const u8 *key);

/**
 * @brief AEAD encryption with a 64-bit nonce (WireGuard: 4 zero bytes ||
 *        LE64(nonce)). Parameters as ca_chacha20poly1305_encrypt_ietf().
 */
void ca_chacha20poly1305_encrypt(ca_chacha20poly1305_ctx_t *wk, u8 *dst,
                                 const u8 *src, u32 src_len,
                                 const u8 *ad, u32 ad_len,
                                 u64 nonce, const u8 *key);

/**
 * @brief AEAD decryption with a 64-bit nonce. As ca_chacha20poly1305_decrypt_ietf().
 */
bool ca_chacha20poly1305_decrypt(ca_chacha20poly1305_ctx_t *wk, u8 *dst,
                                 const u8 *src, u32 src_len,
                                 const u8 *ad, u32 ad_len,
                                 u64 nonce, const u8 *key);

/**
 * @brief XChaCha20-Poly1305 encryption: subkey = HChaCha20(key, nonce[0..15]),
 *        then ChaCha20-Poly1305 with nonce 4 zero bytes || nonce[16..23].
 * @param nonce XCHACHA20POLY1305_NONCE_SIZE bytes (random is safe)
 */
void ca_xchacha20poly1305_encrypt(ca_chacha20poly1305_ctx_t *wk, u8 *dst,
                                  const u8 *src, u32 src_len,
                                  const u8 *ad, u32 ad_len,
                                  const u8 *nonce, const u8 *key);

/**
 * @brief XChaCha20-Poly1305 decryption. As ca_chacha20poly1305_decrypt_ietf().
 */
bool ca_xchacha20poly1305_decrypt(ca_chacha20poly1305_ctx_t *wk, u8 *dst,
                                  const u8 *src, u32 src_len,
                                  const u8 *ad, u32 ad_len,
                                  const u8 *nonce, const u8 *key);

#if EN_CHACHA20_POLY1305_TEST
/**
 * @brief Known-answer tests of ChaCha20, HChaCha20, Poly1305 and both AEADs
 *        (RFC 8439, draft-irtf-cfrg-xchacha), printed to stdout.
 * @return Number of failed tests (0 = all passed)
 */
u32 ca_chacha20poly1305_selftest(void);

/**
 * @brief Registers the CLI command "chacha" (test | speed).
 *        Can be called multiple times: registration occurs only once.
 */
void ca_chacha20poly1305_test_init(void);
#endif /* EN_CHACHA20_POLY1305_TEST */

#endif /* CRYPTO_AEAD_CHACHA20POLY1305_H */
