// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

#ifndef KEYSTORE_SERVICE_H
#define KEYSTORE_SERVICE_H

#include "common.h"
#include "keys_store.h"

#if EN_KEYSTORE_SERVICE

/* ============================================================================
 * FLASH BLOCK LAYOUT
 *
 * Header (cleartext, used as AAD for GCM):
 *   u32  size         total block size in bytes
 *   u8   IV[16]       KDF input: SHA256^n_evolve(IV||p1||p2) -> decrypt_key
 *   u32  n_evolve     SHA256 iteration count
 *   u8   p1[16]       piece1 of decryption key (cleartext)
 *   u16  n_key        number of keys in payload
 *   u8   gcm_nonce[12] AES-GCM nonce for payload
 *
 * Payload (AES-GCM encrypted, authenticated):
 *   ca_flash_key_entry_t key[n_key]   ceil(n_key*48, 16) bytes
 *
 * Trailer:
 *   u8   auth[16]     GCM authentication tag
 * ========================================================================== */

#define KSTORE_SVC_P1_SIZE      16
#define KSTORE_SVC_P2_SIZE      16
#define KSTORE_SVC_IV_SIZE      16
#define KSTORE_SVC_NONCE_SIZE   12
#define KSTORE_SVC_TAG_SIZE     16
#define KSTORE_SVC_KEY_SIZE     32
#define KSTORE_SVC_LABEL_SIZE    6

/* Single key entry inside encrypted payload — 48 byte */
typedef struct {
    u16 key_id;
    u8  label[KSTORE_SVC_LABEL_SIZE];
    u8  key[KSTORE_SVC_KEY_SIZE];
    u32 start;      /* KSTORE_DATE_PACK validity start (0 = no limit) */
    u32 stop;       /* KSTORE_DATE_PACK validity stop  (0 = no limit) */
} ca_flash_key_entry_t;    /* 2+6+32+4+4 = 48 byte */

/* Flash block header — cleartext, used as GCM AAD — 54 byte */
typedef struct {
    u32 size;
    u8  IV[KSTORE_SVC_IV_SIZE];
    u32 n_evolve;
    u8  p1[KSTORE_SVC_P1_SIZE];
    u16 n_key;
    u8  gcm_nonce[KSTORE_SVC_NONCE_SIZE];
} ca_flash_key_block_hdr_t;    /* 4+16+4+16+2+12 = 54 byte */

/* ============================================================================
 * API
 * ========================================================================== */

/**
 * @brief Reads and validates the flash block header.
 * @param base_addr  Base address in flash (aligned to sector size).
 * @return 0 if header is valid, -1 if magic/size are invalid.
 */
int ca_keystore_service_init(u32 base_addr);

/**
 * @brief Derives the key, decrypts the payload, loads the keys into keys_store.
 * @param p2  Piece2 (KSTORE_SVC_P2_SIZE byte).
 * @return 0 if successful, -1 if failure (keys wiped, state INACTIVE).
 * @note  decrypt_key = SHA256^n_evolve(IV || p1 || p2)
 *        The payload is verified with GCM tag before loading.
 *        Any error automatically calls ca_keystore_service_lock().
 */
int ca_keystore_service_unlock(const u8 *p2);

/**
 * @brief Wipes all loaded keys, state -> INACTIVE.
 */
void ca_keystore_service_lock(void);

/**
 * @brief Returns true if keys are loaded (state ACTIVE).
 */
bool ca_keystore_service_is_active(void);

#endif /* EN_KEYSTORE_SERVICE */
#endif /* KEYSTORE_SERVICE_H */
