// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : keys_store.h
 * @brief          : Secure Keys Store via MPU
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Advanced protection system for cryptographic keys using MPU.
 * Keys can be:
 * - Completely isolated (no access)
 * - Read-only (write protection)
 * - Temporarily accessible (during use)
 *
 * Typical sequence:
 * 1) Store keys -> full protection (no access)
 * 2) When a key is needed -> temporary read/write -> use -> full protection
 ******************************************************************************
 */

#ifndef CRYPTO_KEYS_H
#define CRYPTO_KEYS_H

#include "common.h"
#include "mpu.h"
#include "memaux.h"

/* ============================================================================
 * CONFIGURATION CONSTANTS
 * ========================================================================== */

#define MAX_TOTAL_KEY_STORAGE   2048    /* Total size of the key storage area */
#define MAX_SECURE_KEYS         20      /* Maximum number of manageable keys */
#define MIN_KEY_SIZE            16      /* Minimum size for a key (AES uses 16-32 bytes) */

/* ============================================================================
 * DATA TYPES
 * ========================================================================== */

typedef enum {
    CA_KEYS_STORE_NO_ACCESS = 0,       /* No access allowed (region disabled) */
    CA_KEYS_STORE_READ_ONLY = 1,       /* Read-only allowed */
    CA_KEYS_STORE_READ_WRITE = 2,      /* Read and write allowed */
    CA_KEYS_STORE_EXEC_ONLY = 3        /* Execute-only allowed (rarely used) */
} ca_keys_store_access_mode_t;

typedef struct {
    u32 base_address;               /* Base address of the key within the common area */
    u32 size;                       /* Key size */
    u32 validity_start;             /* Packed validity start date (0 = no limit) */
    u32 validity_stop;              /* Packed validity end date (0 = no limit) */
    u8 is_allocated;                /* Flag: key allocated */
    u8 access_mode;                 /* Current access mode */
    u16 key_id;                     /* Unique key ID */
} ca_secure_key_entry_t;

/* Packed date: bits [23:16]=year from 2000, [15:8]=month, [7:0]=day
 * Directly comparable as u32 (natural ordering).
 * Example: 2026-03-15 → KSTORE_DATE_PACK(26, 3, 15) */
#define KSTORE_DATE_PACK(y, m, d)  (((u32)(y) << 16) | ((u32)(m) << 8) | (u32)(d))
#define KSTORE_DATE_YEAR(v)        (((v) >> 16) & 0xFFU)
#define KSTORE_DATE_MONTH(v)       (((v) >>  8) & 0xFFU)
#define KSTORE_DATE_DAY(v)         ((v) & 0xFFU)

/* ============================================================================
 * PUBLIC API FUNCTIONS
 * ========================================================================== */

/**
 * @brief Initializes the secure key management system
 * @param requested_max_keys Maximum number of keys the system must manage
 * @return 0 if successful, -1 if failure
 * @note To be called only once after system initialization
 *       Allocates a single large memory area protected by MPU (1 region only)
 */
int ca_keys_store_init(u8 requested_max_keys);

/**
 * @brief Allocates a secure memory area for a key
 * @param size Requested size for the key
 * @return Base address of the allocated key, 0 in case of error
 * @note The key is initially fully protected (no access)
 *       All keys share a single area protected by 1 MPU region only
 */
void* ca_keys_store_allocate(u32 size);

/**
 * @brief Frees a previously allocated secure memory area
 * @param ptr Pointer to the area to free
 * @return 0 if successful, -1 if failure
 */
int ca_keys_store_free(void* ptr);

/**
 * @brief Sets the access mode for a key area
 * @param ptr Pointer to the key area
 * @param mode Desired access mode
 * @return 0 if successful, -1 if failure
 */
int ca_keys_store_set_access_mode(void* ptr, ca_keys_store_access_mode_t mode);

/**
 * @brief Gets the current access mode for a key area
 * @param ptr Pointer to the key area
 * @return Current access mode, -1 in case of error
 */
ca_keys_store_access_mode_t ca_keys_store_get_access_mode(void* ptr);

/**
 * @brief Copies keys into a secure area (only when accessible)
 * @param dest Secure destination area (must be in read/write mode)
 * @param src Source area of the keys
 * @param size Number of bytes to copy
 * @return 0 if successful, -1 if failure
 */
int ca_keys_store_copy(void* dest, const void* src, u32 size);

/**
 * @brief Completely locks a key area (no access)
 * @param ptr Pointer to the key area
 * @return 0 if successful, -1 if failure
 * @note This function sets maximum protection: no access allowed
 */
int ca_keys_store_lock_completely(void* ptr);

/**
 * @brief Temporarily enables access to a key area
 * @param ptr Pointer to the key area
 * @param mode Temporary access mode
 * @return 0 if successful, -1 if failure
 * @note After use, call ca_keys_store_lock_completely() to restore protection
 */
int ca_keys_store_temp_enable_access(void* ptr, ca_keys_store_access_mode_t mode);

/**
 * @brief Temporarily disables access to a key area
 * @param ptr Pointer to the key area
 * @return 0 if successful, -1 if failure
 * @note Alias for ca_keys_store_lock_completely()
 */
int ca_keys_store_temp_disable_access(void* ptr);

/**
 * @brief Gets information about allocated keys
 * @param[out] keys Array of ca_secure_key_entry_t structures (must be allocated by the caller)
 * @param[in,out] count Maximum number of elements in keys (input), actual number (output)
 * @return 0 if successful, -1 if failure
 */
int ca_keys_store_get_keys_info(ca_secure_key_entry_t* keys, u8* count);

/**
 * @brief Sets the validity period for a key (by index)
 * @param key_idx  Index in the key table (0..max_keys-1)
 * @param start    Packed validity start date via KSTORE_DATE_PACK(); 0 = no limit
 * @param stop     Packed validity end date via KSTORE_DATE_PACK(); 0 = no limit
 * @return 0 if successful, -1 if failure
 */
int ca_keys_store_set_expiry(u8 key_idx, u32 start, u32 stop);

/**
 * @brief Verifies if a key has expired; if so, locks it automatically
 * @param ptr Pointer to the key area
 * @return true if expired/invalid, false if still valid
 * @note If RTC is disabled and validity is set -> true (key not usable).
 *       If RTC is enabled but date is not yet valid -> true.
 *       Auto-lock on true.
 */
bool ca_keys_store_is_key_expired(void *ptr);

/**
 * TODO TODO TODO
 * @brief Locks the MPU configuration to prevent future modifications
 * @return 0 if successful, -1 if failure
 * @note Once called, the MPU configuration cannot be modified
 *       until the next reset. Useful to prevent malicious tampering.
 */
int ca_keys_store_lock_mpu_config(void);

#endif /* KEYS_STORE_H */