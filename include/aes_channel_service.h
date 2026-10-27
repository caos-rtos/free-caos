// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

#ifndef AES_CHANNEL_SERVICE_H
#define AES_CHANNEL_SERVICE_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_AES_CHANNEL_SERVICE

#include "aes.h"
#include "sync.h"

/* wire format: [nonce:12][orig_len_be16:2][ciphertext_padded:ceil(N,16)][tag:16]
 * Plaintext is always padded to 16-byte boundary before encryption to avoid
 * partial-block handling in CRYP hardware (NPBLB unreliable on H7).
 * The 14-byte header [nonce|orig_len] is the GCM AAD: the tag covers the
 * length too, so it cannot be lowered to truncate the plaintext.
 * overhead = 12 + 2 + 16 = 30 bytes */
#define AES_CH_IV_SIZE   12U
#define AES_CH_TAG_SIZE  AES_GCM_TAG_SIZE
#define AES_CH_OVERHEAD  (AES_CH_IV_SIZE + 2U + AES_CH_TAG_SIZE)
#define AES_CH_HDR_SIZE  (AES_CH_IV_SIZE + 2U)   /* authenticated as AAD */

/*
 * IV generation callback — receives 12-byte nonce buffer to fill.
 * NULL → default: ca_rng_fill()
 */
typedef void (*ca_aes_iv_gen_cb_t)(u8 iv[AES_CH_IV_SIZE], void *user);

/*
 * Key ratchet callback — receives current key in-place, must overwrite
 * with next key before returning. Called by ca_aes_channel_rotate().
 * NULL → rotate returns -1 (no-op).
 */
typedef bool (*ca_aes_key_ratchet_cb_t)(u8 *key, u32 key_len, void *user);

typedef struct {
    ca_aes_mode_t            mode;
    ca_aes_keysize_t         keysize;
    void                    *ks_handle;     /* keys_store pointer (ca_keys_store_allocate) */
    ca_aes_iv_gen_cb_t       iv_gen_cb;     /* NULL → ca_rng_fill() */
    ca_aes_key_ratchet_cb_t  ratchet_cb;    /* NULL → rotate disabled */
    void                    *cb_user;       /* opaque for both callbacks */
    ca_aes_ctx_t            *red_ctx;       /* encrypt context (heap) */
    ca_aes_ctx_t            *black_ctx;     /* decrypt context (heap) */
    ca_mutex_t                  mutex;
} ca_aes_channel_t;

/*
 * Init: allocate red_ctx and black_ctx on heap.
 * mode/keysize configurable (typical: CA_AES_MODE_GCM + CA_AES_KEY_256).
 * ks_handle: pointer returned by ca_keys_store_allocate().
 */
i32  ca_aes_channel_init(ca_aes_channel_t *ch,
                         ca_aes_mode_t mode, ca_aes_keysize_t keysize,
                         void *ks_handle,
                         ca_aes_iv_gen_cb_t iv_cb,
                         ca_aes_key_ratchet_cb_t ratchet_cb,
                         void *cb_user);

/*
 * Red→Black (encrypt): allocate *out on heap with ca_memory_allocate_shared:
 * a wire buffer, it may be handed to another task. A task that keeps it
 * calls ca_memory_adopt(*out) so it is freed if the task dies.
 * Wire: [nonce:12][orig_len_be16:2][ciphertext_padded][tag:16] (see top)
 * Caller: ca_aes_channel_free(*out) after use — mandatory wipe.
 */
i32  ca_aes_channel_encrypt(ca_aes_channel_t *ch,
                            const u8 *plain, u32 plain_len,
                            u8 **out, u32 *out_len);

/*
 * Black→Red (decrypt): allocate *out on heap with ca_memory_allocate.
 * Verify GCM tag — returns -1 if authentication fails.
 * Caller: ca_aes_channel_free(*out) after use — mandatory wipe.
 */
i32  ca_aes_channel_decrypt(ca_aes_channel_t *ch,
                            const u8 *cipher, u32 cipher_len,
                            u8 **out, u32 *out_len);

/*
 * Free output buffer with mandatory wipe (ca_memory_free wiped=true).
 * Always use instead of direct ca_memory_free.
 */
void ca_aes_channel_free(void *p);

/*
 * Manual ratchet: calls ratchet_cb to update the key in ks_handle.
 * Returns -1 if ratchet_cb == NULL or keys_store access error.
 */
i32  ca_aes_channel_rotate(ca_aes_channel_t *ch);

/*
 * Deinit: zero and free red_ctx and black_ctx from the heap.
 */
void ca_aes_channel_deinit(ca_aes_channel_t *ch);

#endif /* EN_AES_CHANNEL_SERVICE */
#endif /* AES_CHANNEL_SERVICE_H */
