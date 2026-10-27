// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    aes.h
 * @brief   AES driver public API — hardware AES (ECB/CBC/CTR/GCM)
 * @note    Available on: H7xx (H753 via CRYP)
 *          F1xx/F4xx/F7xx/H5xx/H747: stub (return false)
 *
 *          Context swap: the CRYP processor supports saving/restoring
 *          Key, IV and GCM/CCM context registers. ca_aes_ctx_t encapsulates
 *          the state for concurrent use protected by mutex.
 ******************************************************************************
 */

#ifndef AES_H
#define AES_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_AES

typedef enum {
    CA_AES_MODE_ECB = 0,
    CA_AES_MODE_CBC = 1,
    CA_AES_MODE_CTR = 2,
    CA_AES_MODE_GCM = 3,
} ca_aes_mode_t;

typedef enum {
    CA_AES_KEY_128 = 0,   /* KEYSIZE = 00 */
    CA_AES_KEY_192 = 1,   /* KEYSIZE = 01 */
    CA_AES_KEY_256 = 2,   /* KEYSIZE = 10 */
} ca_aes_keysize_t;

/* context for CRYP state save/restore (concurrency) */
typedef struct {
    ca_aes_mode_t     mode;
    ca_aes_keysize_t  keysize;
    u32 key[8];      /* max 256-bit = 8 words */
    u32 iv[4];       /* IV = 4 words (CBC/CTR) */
    bool configured;
#if !defined(CRYP_BASE) && !defined(AES_BASE)
    /* Software-only build (no crypto IP, e.g. F407/U575): the AES-256 expanded
     * key schedule (15 round keys = 60 words) lives in the context so it is
     * computed once at setup and reused by every block/GCM call — no per-call
     * re-expansion (CPU) and no 240B scratch on the caller stack (matters under
     * SW crypto, e.g. SSH on F407). Compiled out where a HW IP is present. */
    u32  sw_rk[60];
    bool sw_ready;   /* sw_rk holds a valid schedule for the current key */
    u8   sw_nr;      /* rounds of sw_rk: 10 (AES-128) or 14 (AES-256) */
#endif
} ca_aes_ctx_t;

/* init hardware CRYP (call once) */
bool ca_aes_init(void);

/* configure context (mode, key, iv) */
bool ca_aes_ctx_setup(ca_aes_ctx_t *ctx, ca_aes_mode_t mode,
                      ca_aes_keysize_t keysize,
                      const u8 *key, const u8 *iv);

/* New IV on a context already set up: the key and the expanded schedule
 * stay (no re-expansion). For a session that keeps its context for many
 * messages and only changes the nonce. false if ctx was never set up. */
bool ca_aes_ctx_set_iv(ca_aes_ctx_t *ctx, const u8 *iv);

/* encrypt: in-place, len must be a multiple of 16 */
bool ca_aes_encrypt(ca_aes_ctx_t *ctx, const u8 *in, u8 *out, u32 len);

/* decrypt: in-place, len must be a multiple of 16 */
bool ca_aes_decrypt(ca_aes_ctx_t *ctx, const u8 *in, u8 *out, u32 len);

/* GCM (Galois/Counter Mode) — authenticated encryption
 * aad: additional authenticated data (not encrypted, authenticated)
 * tag: 16 byte authentication tag (out for encrypt, in for decrypt verify) */
#define AES_GCM_TAG_SIZE  16

bool ca_aes_gcm_encrypt(ca_aes_ctx_t *ctx,
                        const u8 *aad, u32 aad_len,
                        const u8 *in, u8 *out, u32 len,
                        u8 tag[AES_GCM_TAG_SIZE]);

bool ca_aes_gcm_decrypt(ca_aes_ctx_t *ctx,
                        const u8 *aad, u32 aad_len,
                        const u8 *in, u8 *out, u32 len,
                        const u8 tag[AES_GCM_TAG_SIZE]);

#if EN_AES_GCM_SW_TEST
/* NIST AES-256-GCM known-answer vectors through the public dispatch: exercise
 * the ACTIVE backend (HW CRYP where present, software otherwise).
 * Returns the number of failed vectors (0 = all passed). */
u32 ca_aes_gcm_selftest(void);
/* FIPS-197 AES-256-ECB known-answer through the public dispatch (same rationale:
 * KAT vs the active backend, not a symmetric roundtrip). 0 = all passed. */
u32 ca_aes_ecb_selftest(void);
#endif

#endif /* EN_AES */
#endif /* AES_H */
