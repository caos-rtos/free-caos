// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : kdf.h
 * @brief          : HMAC-SHA256, HKDF-SHA256, PBKDF2-HMAC-SHA256
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Key derivation on top of the software SHA-256 (sha256.c), so it gives the
 * same result on every family, with or without a HASH accelerator.
 *
 *   HMAC-SHA256   RFC 2104 / FIPS 198-1   keyed MAC, the building block
 *   HKDF-SHA256   RFC 5869                key material -> independent keys
 *   PBKDF2        RFC 8018                password -> key, deliberately slow
 *
 * Typical chain (secure link of app_3):
 *   master = PBKDF2(password, salt, iterations)
 *   k12    = HKDF(master, salt=NULL, info="... 1->2")
 *   k21    = HKDF(master, salt=NULL, info="... 2->1")
 *
 * Every intermediate (inner/outer pads, PRK, U blocks) is wiped before
 * returning. Enabled by EN_SHA256; the known-answer tests by EN_SHA256_TEST.
 ******************************************************************************
 */

#ifndef CRYPTO_KDF_H
#define CRYPTO_KDF_H

#include "sha256.h"

#define HMAC_SHA256_SIZE     SHA256_DIGEST_SIZE   /* 32 */
#define HKDF_SHA256_MAX_OUT  (255U * SHA256_DIGEST_SIZE)

typedef struct {
    ca_sha256_ctx_t inner;
    ca_sha256_ctx_t outer;      /* already keyed with K ^ opad */
} ca_hmac_sha256_ctx_t;

/* Incremental HMAC-SHA256. A key longer than 64 bytes is hashed first. */
void ca_hmac_sha256_init(ca_hmac_sha256_ctx_t *ctx, const u8 *key, u32 key_len);
void ca_hmac_sha256_update(ca_hmac_sha256_ctx_t *ctx, const u8 *data, u32 len);
/* Writes the 32-byte MAC and wipes the context. */
void ca_hmac_sha256_final(ca_hmac_sha256_ctx_t *ctx, u8 mac[HMAC_SHA256_SIZE]);

/* One-shot HMAC-SHA256. */
void ca_hmac_sha256(const u8 *key, u32 key_len, const u8 *data, u32 len,
                    u8 mac[HMAC_SHA256_SIZE]);

/*
 * HKDF-SHA256 (RFC 5869): extract (salt, ikm) then expand (prk, info) into
 * out_len bytes. salt may be NULL (= 32 zero bytes). out_len 1..8160.
 * Returns false on bad arguments only.
 */
bool ca_hkdf_sha256(const u8 *salt, u32 salt_len,
                    const u8 *ikm, u32 ikm_len,
                    const u8 *info, u32 info_len,
                    u8 *out, u32 out_len);

/*
 * PBKDF2-HMAC-SHA256 (RFC 8018): password + salt, `iterations` rounds,
 * out_len bytes. Cost grows linearly with iterations: on the MCU it takes
 * seconds on purpose (each guess costs an attacker the same). Not from ISR.
 * Returns false on bad arguments (iterations == 0, out_len == 0).
 */
bool ca_pbkdf2_sha256(const u8 *password, u32 password_len,
                      const u8 *salt, u32 salt_len,
                      u32 iterations, u8 *out, u32 out_len);

#if EN_SHA256_TEST
/* Known-answer tests: RFC 4231 (HMAC), RFC 5869 (HKDF), RFC 7914 §11
 * (PBKDF2-SHA256). Returns the number of failures (0 = all passed). */
u32 ca_kdf_selftest(void);
#endif

#endif /* CRYPTO_KDF_H */
