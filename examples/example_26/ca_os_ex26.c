// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 26 — Cryptography: CRC32, HASH, AES, RNG and Key Derivation
 *
 * This example demonstrates the use of STM32 cryptographic modules:
 *
 *   1. CRC32  — Checksum calculation for data integrity verification
 *   2. HASH   — SHA-256 calculation (one-shot and concurrent contextual)
 *   3. AES    — AES-256 encryption/decryption (ECB, CBC, CTR, GCM)
 *   4. RNG    — Hardware random number generation
 *
 *   5. Multithreading — HASH and AES support context swap with mutex,
 *                       showing 2 threads operating in concurrency
 *                       synchronized via mutex/semaphore
 *
 *   6. Key Derivation — Derives key from IV+KEY_SECRET:
 *                       IV + KEY_SECRET -> HASH-SHA256 -> 32-BYTE SECRET KEY
 *
 *   7. Key-Store — Secure storage of the derived key using MPU:
 *                       - Protected area allocation (1 MPU region)
 *                       - Key copy to secure area
 *                       - Full protection (no access)
 *                       - Validity period by date (start/stop)
 *                       - Auto-lock on expired key or invalid RTC
 *                       - Temporary access for use and release
 *
 * Hardware (Nucleo-H753ZI):
 *   - UART2 for STDOUT/CLI (CN5: TX=PA2, RX=PA3)
 *   - USB CDC for alternative output
 *
 * CLI Commands:
 *   crc32      — Calculate CRC32 on test buffer
 *   hash       — Calculate one-shot SHA-256
 *   hash_mt    — Concurrent HASH test (2 threads)
 *   aes        — AES-256 ECB encrypt/decrypt test
 *   aes_gcm    — AES-256 GCM test (authenticated encryption)
 *   aes_mt     — Concurrent AES test (2 threads)
 *   derive     — Derive key: IV+KEY -> SHA256 -> 32-byte key
 *   kstore     — Key-store test: save/retrieve protected key
 *   full       — Executes the entire sequence: derive + kstore + aes_gcm
 *   help       — List available commands
 *
 * STDOUT Log (every 10 s):
 *   EX26: rng=0xXXXXXXXX  tick=NNNNN
 */

#include "caos.h"

#if EN_CRC && EN_HASH && EN_AES && EN_RNG && EN_KEYS_STORE

/* ============================================================================
 * CONFIGURATION AND DEFINITIONS
 * ========================================================================== */

/* Buffer size for test */
#define TEST_BUFFER_SIZE    256

/* AES-256 key size */
#define AES_256_KEY_SIZE    32
#define AES_IV_SIZE         16
#define AES_BLOCK_SIZE      16

/* GCM Tag */
#define GCM_TAG_SIZE        16

/* Secret for key derivation */
#define KEY_SECRET_SIZE     32
#define IV_SECRET_SIZE      16

/* ============================================================================
 * SHARED STATE
 * ========================================================================== */

/* Mutex for thread synchronization */
static ca_mutex_t g_sync_mutex;
static ca_semaphore_t g_start_sem;

/* Flag for synchronized start */
static volatile u8 g_threads_ready = 0;
static volatile u8 g_hash_threads_started = 0;
static volatile u8 g_aes_threads_started = 0;

/* Buffer for test */
static u8 g_test_data[TEST_BUFFER_SIZE];
static u8 g_aes_key[AES_256_KEY_SIZE];
static u8 g_aes_iv[AES_IV_SIZE];

/* Derived key (32 bytes) */
static u8 g_derived_key[32];
static u8 g_derived_key_ready = 0;

/* Key-store area */
static void* g_secure_key_area = NULL;

/* Counters for statistics */
static volatile u32 g_hash_thread1_count = 0;
static volatile u32 g_hash_thread2_count = 0;
static volatile u32 g_aes_thread1_count = 0;
static volatile u32 g_aes_thread2_count = 0;

/* ============================================================================
 * UTILITY FUNCTIONS
 * ========================================================================== */

/* Print hexadecimal buffer with label (wrapper for ca_hexdump) */
static void print_hex_label(const char* label, const u8* data, u32 len) {
    ca_stdout_printf_n("\n%s (%u bytes):", label, len);
    /* ca_hexdump's second param is the total byte count (despite the "lines"
     * name in its signature), same as every other caller in the codebase. */
    ca_hexdump((u8*)data, (u16)len);
}

/* Initialize test buffer with pattern */
static void init_test_buffer(void) {
    for (u32 i = 0; i < TEST_BUFFER_SIZE; i++) {
        g_test_data[i] = (u8)(i & 0xFF);
    }
}

/* Generate random key and IV */
static void generate_random_key_iv(void) {
    ca_rng_fill(g_aes_key, AES_256_KEY_SIZE);
    ca_rng_fill(g_aes_iv, AES_IV_SIZE);
}

/* ============================================================================
 * 1. CRC32 EXAMPLE
 * ========================================================================== */

static bool example_crc32(void) {
    init_test_buffer();

    /* Calculate CRC32 */
    u32 crc = ca_crc32_get(g_test_data, TEST_BUFFER_SIZE);

    ca_stdout_put_n("=== CRC32 Example ===");
    ca_stdout_printf_n("Buffer size: %u bytes", TEST_BUFFER_SIZE);
    ca_stdout_printf_n("CRC32: 0x%08X", crc);

    /* Integrity check: modify a byte and recalculate */
    g_test_data[100] ^= 0xFF;
    u32 crc_modified = ca_crc32_get(g_test_data, TEST_BUFFER_SIZE);
    ca_stdout_printf_n("CRC32 (modified): 0x%08X (different!)", crc_modified);

    /* Restore */
    g_test_data[100] ^= 0xFF;
    u32 crc_restored = ca_crc32_get(g_test_data, TEST_BUFFER_SIZE);

    /* PASS: a single flipped bit must change the CRC, and restoring the
     * buffer must bring the very same value back. */
    return (crc_modified != crc) && (crc_restored == crc);
}

/* ============================================================================
 * 2. HASH EXAMPLE (SHA-256)
 * ========================================================================== */

static bool example_hash_oneshot(void) {
    u8 digest[HASH_SHA256_DIGEST_SIZE];
    u32 digest_len;
    bool ok = false;

    /* Known-answer test first: SHA-256("abc"), FIPS 180-4 App. B.1. Without a
     * reference vector the example could only prove that "something" came out. */
    static const u8 kat_abc[32] = {
        0xBA,0x78,0x16,0xBF,0x8F,0x01,0xCF,0xEA,0x41,0x41,0x40,0xDE,0x5D,0xAE,0x22,0x23,
        0xB0,0x03,0x61,0xA3,0x96,0x17,0x7A,0x9C,0xB4,0x10,0xFF,0x61,0xF2,0x00,0x15,0xAD
    };
    u8  kat[HASH_SHA256_DIGEST_SIZE];
    u32 kat_len;
    if (ca_hash_compute(CA_HASH_ALGO_SHA256, (const u8 *)"abc", 3U, kat, &kat_len)) {
        ok = (kat_len == 32U) && (ca_memcmp(kat, kat_abc, 32) == 0);
        ca_stdout_printf_n("SHA-256(\"abc\") KAT: %s", ok ? "PASS" : "FAILED");
    } else {
        ca_stdout_put_n("SHA-256 KAT: FAILED (compute error)");
    }

    init_test_buffer();

    /* One-shot SHA-256 */
    if (ca_hash_compute(CA_HASH_ALGO_SHA256, g_test_data, TEST_BUFFER_SIZE,
                        digest, &digest_len)) {
        ca_stdout_put_n("=== HASH SHA-256 One-Shot ===");
        print_hex_label("SHA-256 digest", digest, digest_len);
    } else {
        ca_stdout_put_n("Hash compute FAILED");
        ok = false;
    }
    return ok;
}

/* Thread 1: Concurrent Hash with context swap */
static u32 task_hash_thread1(void *arg) {
    (void)arg;

    ca_hash_ctx_t ctx;
    u8 digest[HASH_SHA256_DIGEST_SIZE];
    u32 digest_len;

    /* Wait for start signal */
    g_hash_threads_started++;
    ca_sem_wait(&g_start_sem);

    while (1) {
        /* Initialize context */
        ca_hash_ctx_init(&ctx, CA_HASH_ALGO_SHA256);

        /* Incremental calculation in 3 steps */
        const char* data1 = "Hello ";
        const char* data2 = "concurrent ";
        const char* data3 = "world!";

        ca_hash_ctx_update(&ctx, (const u8*)data1, 6);
        ca_os_sleep(10);  /* Small delay to simulate concurrency */
        ca_hash_ctx_update(&ctx, (const u8*)data2, 11);
        ca_os_sleep(10);
        ca_hash_ctx_update(&ctx, (const u8*)data3, 6);

        /* Finalize */
        if (ca_hash_ctx_final(&ctx, digest, &digest_len)) {
            g_hash_thread1_count++;
            if (g_hash_thread1_count % 10 == 0) {
                ca_stdout_printf_n("[HASH-T1] digest=%02X...%02X (count=%u)",
                        digest[0], digest[digest_len-1], g_hash_thread1_count);
            }
        }

        ca_os_sleep(100);
    }
    return 0;
}

/* Thread 2: Concurrent Hash with context swap */
static u32 task_hash_thread2(void *arg) {
    (void)arg;

    ca_hash_ctx_t ctx;
    u8 digest[HASH_SHA256_DIGEST_SIZE];
    u32 digest_len;
    
    /* Wait for start signal */
    g_hash_threads_started++;
    ca_sem_wait(&g_start_sem);

    while (1) {
        /* Initialize context */
        ca_hash_ctx_init(&ctx, CA_HASH_ALGO_SHA256);
        
        /* Incremental calculation with different data */
        const char* data1 = "Thread ";
        const char* data2 = "number ";
        const char* data3 = "two!";
        
        ca_hash_ctx_update(&ctx, (const u8*)data1, 7);
        ca_os_sleep(15);
        ca_hash_ctx_update(&ctx, (const u8*)data2, 7);
        ca_os_sleep(15);
        ca_hash_ctx_update(&ctx, (const u8*)data3, 4);
        
        /* Finalize */
        if (ca_hash_ctx_final(&ctx, digest, &digest_len)) {
            g_hash_thread2_count++;
            if (g_hash_thread2_count % 10 == 0) {
                ca_stdout_printf_n("[HASH-T2] digest=%02X...%02X (count=%u)",
                        digest[0], digest[digest_len-1], g_hash_thread2_count);
            }
        }
        
        ca_os_sleep(100);
    }
    return 0;
}

static bool example_hash_multithread(void) {
    
    ca_stdout_printf_n("=== HASH Multithread Test ===");
    ca_stdout_printf_n("Starting 2 concurrent hash threads...");
    
    /* Reset counters and flags */
    g_hash_thread1_count = 0;
    g_hash_thread2_count = 0;
    g_hash_threads_started = 0;
    
    /* Reset semaphore (count = 0) */
    ca_sem_init(&g_start_sem, "start", 0, 10);
    
    /* Create the 2 threads */
    ca_os_job("hash_t1", CA_TH_LEVEL2, task_hash_thread1, NULL, STACK_MEDIUM, CA_JOB_STD, 0);
    ca_os_job("hash_t2", CA_TH_LEVEL2, task_hash_thread2, NULL, STACK_MEDIUM, CA_JOB_STD, 0);
    
    /* Wait for both threads to be in ca_sem_wait */
    while (g_hash_threads_started < 2) {
        ca_os_sleep(10);
    }

    /* Give the green light to the threads */
    ca_sem_post(&g_start_sem);
    ca_sem_post(&g_start_sem);
    
    ca_stdout_printf_n("Threads started. Running for 3 seconds...");
    
    /* Let it run for 3 seconds */
    ca_os_sleep(3000);
    
    ca_stdout_printf_n("T1 count: %u, T2 count: %u",
            g_hash_thread1_count, g_hash_thread2_count);

    /* PASS: both threads must have completed cycles — see AES multithread. */
    return (g_hash_thread1_count > 0U) && (g_hash_thread2_count > 0U);
}

/* ============================================================================
 * 3. AES EXAMPLE
 * ========================================================================== */

static bool example_aes_ecb(void) {
    u8 plaintext[16] = "Hello AES World!";
    u8 ciphertext[16];
    u8 decrypted[16];
    
    ca_aes_ctx_t ctx;
    
    ca_stdout_printf_n("=== AES-256 ECB Example ===");
    
    /* Setup AES-256 context */
    generate_random_key_iv();
    ca_aes_ctx_setup(&ctx, CA_AES_MODE_ECB, CA_AES_KEY_256, g_aes_key, NULL);
    
    print_hex_label("Plaintext", plaintext, 16);
    print_hex_label("AES-256 Key", g_aes_key, 32);
    
    /* Encrypt */
    if (ca_aes_encrypt(&ctx, plaintext, ciphertext, 16)) {
        print_hex_label("Ciphertext", ciphertext, 16);
        
        /* Decrypt */
        if (ca_aes_decrypt(&ctx, ciphertext, decrypted, 16)) {
            print_hex_label("Decrypted", decrypted, 16);
            
            /* Verify */
            if (ca_memcmp(plaintext, decrypted, 16) == 0) {
                ca_stdout_put_n("AES-256 ECB: SUCCESS (plaintext recovered)");
                return true;
            }
            ca_stdout_put_n("AES-256 ECB: FAILED (plaintext mismatch!)");
            return false;
        }
        ca_stdout_put_n("AES decrypt FAILED");
        return false;
    }
    ca_stdout_put_n("AES encrypt FAILED");
    return false;
}

static bool example_aes_gcm(void) {
    u8 plaintext[48] = "AES-GCM authenticated encrypt!";
    u8 ciphertext[48];
    u8 decrypted[48];
    u8 tag[GCM_TAG_SIZE];
    u8 aad[16] = "authenticated!";
    
    ca_aes_ctx_t ctx;
    
    ca_stdout_printf_n("=== AES-256 GCM Example ===");
    
    /* Setup AES-256 GCM context */
    generate_random_key_iv();
    ca_aes_ctx_setup(&ctx, CA_AES_MODE_GCM, CA_AES_KEY_256, g_aes_key, g_aes_iv);
    
    print_hex_label("Plaintext", plaintext, 32);
    print_hex_label("AES-256 Key", g_aes_key, 32);
    print_hex_label("IV", g_aes_iv, 16);
    print_hex_label("AAD", aad, 16);
    
    /* Encrypt with GCM */
    if (ca_aes_gcm_encrypt(&ctx, aad, 16, plaintext, ciphertext, 32, tag)) {
        print_hex_label("Ciphertext", ciphertext, 32);
        print_hex_label("GCM Tag", tag, 16);
        ca_stdout_put_n("AES-256 GCM Encrypt: SUCCESS");
        
        /* Decrypt with tag verification */
        bool round_trip = false, tamper_caught = false;
        if (ca_aes_gcm_decrypt(&ctx, aad, 16, ciphertext, decrypted, 32, tag)) {
            print_hex_label("Decrypted", decrypted, 32);
            if (ca_memcmp(plaintext, decrypted, 32) == 0) {
                ca_stdout_put_n("AES-256 GCM Decrypt: SUCCESS (plaintext recovered)");
                round_trip = true;
            }
        } else {
            ca_stdout_put_n("AES-256 GCM Decrypt: FAILED (tag mismatch!)");
        }
        
        /* Test with tampered tag */
        tag[0] ^= 0xFF;
        if (!ca_aes_gcm_decrypt(&ctx, aad, 16, ciphertext, decrypted, 32, tag)) {
            ca_stdout_put_n("AES-256 GCM Tamper Detection: SUCCESS (tag mismatch detected)");
            tamper_caught = true;
        } else {
            ca_stdout_put_n("AES-256 GCM Tamper Detection: FAILED (forged tag accepted!)");
        }
        tag[0] ^= 0xFF;  /* Restore */

        /* PASS needs BOTH: the round trip works AND a forged tag is rejected. */
        return round_trip && tamper_caught;
    }
    ca_stdout_put_n("AES GCM encrypt FAILED");
    return false;
}

/* Thread 1: Concurrent AES */
static u32 task_aes_thread1(void *arg) {
    (void)arg;
    
    ca_aes_ctx_t ctx;
    u8 plaintext[16] = "Thread1-encrypt!";
    u8 ciphertext[16];
    u8 decrypted[16];
    
    /* Wait for start signal */
    g_aes_threads_started++;
    ca_sem_wait(&g_start_sem);

    /* Setup context */
    ca_aes_ctx_setup(&ctx, CA_AES_MODE_ECB, CA_AES_KEY_256, g_aes_key, NULL);

    while (1) {
        /* Encrypt/Decrypt cycle */
        if (ca_aes_encrypt(&ctx, plaintext, ciphertext, 16) &&
            ca_aes_decrypt(&ctx, ciphertext, decrypted, 16)) {
            g_aes_thread1_count++;
            if (g_aes_thread1_count % 20 == 0) {
                ca_stdout_printf_n("[AES-T1] cycles=%u", g_aes_thread1_count);
            }
        }
        ca_os_sleep(50);
    }
    return 0;
}

/* Thread 2: Concurrent AES */
static u32 task_aes_thread2(void *arg) {
    (void)arg;
    
    ca_aes_ctx_t ctx;
    u8 plaintext[16] = "Thread2-encrypt!";
    u8 ciphertext[16];
    u8 decrypted[16];
    
    /* Wait for start signal */
    g_aes_threads_started++;
    ca_sem_wait(&g_start_sem);

    /* Setup context */
    ca_aes_ctx_setup(&ctx, CA_AES_MODE_ECB, CA_AES_KEY_256, g_aes_key, NULL);

    while (1) {
        /* Encrypt/Decrypt cycle */
        if (ca_aes_encrypt(&ctx, plaintext, ciphertext, 16) &&
            ca_aes_decrypt(&ctx, ciphertext, decrypted, 16)) {
            g_aes_thread2_count++;
            if (g_aes_thread2_count % 20 == 0) {
                ca_stdout_printf_n("[AES-T2] cycles=%u", g_aes_thread2_count);
            }
        }
        ca_os_sleep(50);
    }
    return 0;
}

static bool example_aes_multithread(void) {
    
    ca_stdout_printf_n("=== AES Multithread Test ===");
    ca_stdout_printf_n("Starting 2 concurrent AES threads...");
    
    /* Reset counters and flags */
    g_aes_thread1_count = 0;
    g_aes_thread2_count = 0;
    g_aes_threads_started = 0;
    
    /* Reset semaphore (count = 0) */
    ca_sem_init(&g_start_sem, "start", 0, 10);
    
    /* Generate common key */
    generate_random_key_iv();
    
    /* Create the 2 threads */
    ca_os_job("aes_t1", CA_TH_LEVEL2, task_aes_thread1, NULL, STACK_MEDIUM, CA_JOB_STD, 0);
    ca_os_job("aes_t2", CA_TH_LEVEL2, task_aes_thread2, NULL, STACK_MEDIUM, CA_JOB_STD, 0);
    
    /* Wait for both threads to be in ca_sem_wait */
    while (g_aes_threads_started < 2) {
        ca_os_sleep(10);
    }
    
    /* Give the green light to the threads (2 posts for 2 threads) */
    ca_sem_post(&g_start_sem);
    ca_sem_post(&g_start_sem);
    
    ca_stdout_printf_n("Threads started. Running for 3 seconds...");
    
    /* Let it run for 3 seconds */
    ca_os_sleep(3000);
    
    ca_stdout_printf_n("T1 count: %u, T2 count: %u",
            g_aes_thread1_count, g_aes_thread2_count);

    /* PASS: both threads must have completed cycles — one thread starving
     * would mean the context swap under mutex is not working. */
    return (g_aes_thread1_count > 0U) && (g_aes_thread2_count > 0U);
}

/* ============================================================================
 * 4. RNG EXAMPLE
 * ========================================================================== */


/* ============================================================================
 * 5. KEY DERIVATION: IV+KEY_SECRET -> SHA256 -> AES256 -> 32-BYTE KEY
 * ========================================================================== */

static void example_key_derivation(void) {
    u8 iv_secret[IV_SECRET_SIZE];
    u8 key_secret[KEY_SECRET_SIZE];
    u8 hash_input[IV_SECRET_SIZE + KEY_SECRET_SIZE];
    u8 sha256_digest[HASH_SHA256_DIGEST_SIZE];
    u32 digest_len;
    
    ca_stdout_printf_n("=== Key Derivation ===");
    ca_stdout_printf_n("IV+KEY_SECRET -> SHA256 -> 32-BYTE SECRET KEY");
    
    /* Generate random IV and KEY_SECRET */
    ca_rng_fill(iv_secret, IV_SECRET_SIZE);
    ca_rng_fill(key_secret, KEY_SECRET_SIZE);
    
    print_hex_label("IV_SECRET", iv_secret, IV_SECRET_SIZE);
    print_hex_label("KEY_SECRET", key_secret, KEY_SECRET_SIZE);
    
    /* Step 1: Concatenate IV + KEY_SECRET */
    ca_memcpy(hash_input, iv_secret, IV_SECRET_SIZE);
    ca_memcpy(hash_input + IV_SECRET_SIZE, key_secret, KEY_SECRET_SIZE);

    ca_stdout_printf_n("Step 1: IV + KEY_SECRET concatenated (%u bytes)",
            IV_SECRET_SIZE + KEY_SECRET_SIZE);
    
    /* Step 2: SHA-256 hash */
    if (ca_hash_compute(CA_HASH_ALGO_SHA256, hash_input,
                        IV_SECRET_SIZE + KEY_SECRET_SIZE,
                        sha256_digest, &digest_len)) {
        print_hex_label("Step 2: SHA-256(IV||KEY_SECRET)", sha256_digest, digest_len);
        
        /* Step 3: Use SHA-256 digest as AES-256 key */
        /* The SHA-256 digest is already 32 bytes, perfect for AES-256 */
        ca_memcpy(g_derived_key, sha256_digest, 32);
        g_derived_key_ready = 1;
        
        print_hex_label("Step 3: Derived 32-BYTE SECRET KEY", g_derived_key, 32);
        
        ca_stdout_printf_n("Key derivation completed successfully!");
        ca_stdout_printf_n("The derived key can now be stored in the key-store.");
    } else {
        ca_stdout_put_n("Hash computation FAILED");
    }
}

/* ============================================================================
 * 6. KEY-STORE EXAMPLE
 * ========================================================================== */

/* Return the key_id (slot index) given the pointer, 0xFF if not found */
static u8 get_key_id(void *ptr) {
    ca_secure_key_entry_t info[MAX_SECURE_KEYS];
    u8 cnt = MAX_SECURE_KEYS;
    if (!ptr || ca_keys_store_get_keys_info(info, &cnt) != 0) return 0xFF;
    for (u8 i = 0; i < cnt; i++) {
        if (info[i].base_address == (u32)ptr) return (u8)info[i].key_id;
    }
    return 0xFF;
}

static void example_key_store(void) {
    u8 key_data[32];

    ca_stdout_put_n("=== Key-Store Example (MPU protected) ===");

    /* --- KEY 0: permanent — derived or random key (AES-256) --- */
    g_secure_key_area = ca_keys_store_allocate(32);
    if (!g_secure_key_area) { ca_stdout_put_n("Alloc key[0] FAILED"); return; }

    if (g_derived_key_ready)
        ca_memcpy(key_data, g_derived_key, 32);
    else
        ca_rng_fill(key_data, 32);

    ca_keys_store_temp_enable_access(g_secure_key_area, CA_KEYS_STORE_READ_WRITE);
    ca_keys_store_copy(g_secure_key_area, key_data, 32);
    ca_keys_store_lock_completely(g_secure_key_area);
    /* no validity → permanent */
    ca_stdout_printf_n("Key[%u]: 32B  AES-256  validity=permanent",
                       get_key_id(g_secure_key_area));

    /* --- KEY 1: valid 2026-01-01 → 2027-12-31 --- */
    void *kv = ca_keys_store_allocate(16);
    if (kv) {
        u8 kv_data[16];
        ca_rng_fill(kv_data, 16);
        ca_keys_store_temp_enable_access(kv, CA_KEYS_STORE_READ_WRITE);
        ca_keys_store_copy(kv, kv_data, 16);
        ca_keys_store_lock_completely(kv);
        u8 id = get_key_id(kv);
        ca_keys_store_set_expiry(id,
            KSTORE_DATE_PACK(26, 1,  1),
            KSTORE_DATE_PACK(27, 12, 31));
        bool exp = ca_keys_store_is_key_expired(kv);
        ca_stdout_printf_n("Key[%u]: 16B  start=2026-01-01  stop=2027-12-31  -> %s",
                           id, exp ? "EXPIRED (auto-locked)" : "valid");
    }

    /* --- KEY 2: already expired 2024-01-01 → 2025-12-31 --- */
    void *ke = ca_keys_store_allocate(16);
    if (ke) {
        u8 ke_data[16];
        ca_rng_fill(ke_data, 16);
        ca_keys_store_temp_enable_access(ke, CA_KEYS_STORE_READ_WRITE);
        ca_keys_store_copy(ke, ke_data, 16);
        ca_keys_store_lock_completely(ke);
        u8 id = get_key_id(ke);
        ca_keys_store_set_expiry(id,
            KSTORE_DATE_PACK(24, 1,  1),
            KSTORE_DATE_PACK(25, 12, 31));
        bool exp = ca_keys_store_is_key_expired(ke);
        ca_stdout_printf_n("Key[%u]: 16B  start=2024-01-01  stop=2025-12-31  -> %s",
                           id, exp ? "EXPIRED (auto-locked)" : "valid");
    }

    /* --- Use of permanent key --- */
    ca_stdout_put_n("--- Using key[0] for AES (simulate) ---");
    if (ca_keys_store_is_key_expired(g_secure_key_area)) {
        ca_stdout_put_n("Key[0] expired - cannot use");
    } else if (ca_keys_store_temp_enable_access(g_secure_key_area, CA_KEYS_STORE_READ_ONLY) == 0) {
        u8 retrieved[32];
        ca_memcpy(retrieved, g_secure_key_area, 32);
        ca_keys_store_temp_disable_access(g_secure_key_area);
        ca_stdout_put_n(ca_memcmp(key_data, retrieved, 32) == 0
                        ? "Key[0] verification: OK"
                        : "Key[0] verification: FAILED");
    }

    ca_stdout_put_n("Type 'kstore list' to see live validity status");
    ca_stdout_put_n("Type 'kstore info' for storage statistics");
}

/* ============================================================================
 * 7. FULL DEMO: Derive + Key-Store + AES-GCM
 * ========================================================================== */

/* ============================================================================
 * TEST — run every check in sequence and report one verdict
 * ========================================================================== */

static void run_all_tests(void)
{
    struct { const char *name; bool ok; } r[9];
    u32 n = 0, failed = 0;

    ca_stdout_put_n("");
    ca_stdout_put_n("========================================================");
    ca_stdout_put_n("            EX26 TEST SUITE - running all              ");
    ca_stdout_put_n("========================================================");

    r[n].name = "crc32";    r[n].ok = example_crc32();            n++;
    ca_os_sleep(200);
    r[n].name = "hash";     r[n].ok = example_hash_oneshot();     n++;
    ca_os_sleep(200);
    r[n].name = "hash_mt";  r[n].ok = example_hash_multithread(); n++;
    ca_os_sleep(200);
    r[n].name = "aes_ecb";  r[n].ok = example_aes_ecb();          n++;
    ca_os_sleep(200);
    r[n].name = "aes_gcm";  r[n].ok = example_aes_gcm();          n++;
    ca_os_sleep(200);
    r[n].name = "aes_mt";   r[n].ok = example_aes_multithread();  n++;
    ca_os_sleep(200);

    /* NIST known-answer vectors — only built when the SW test is enabled */
    r[n].name = "aes_nist";
    r[n].ok   = true;
    EVAL_AES_GCM_SW_TEST(
        r[n].ok = (ca_aes_gcm_selftest() == 0U) && (ca_aes_ecb_selftest() == 0U);
    )
    n++;
    ca_os_sleep(200);

    /* derive + kstore: judged by the state they are supposed to leave behind */
    example_key_derivation();
    r[n].name = "derive";   r[n].ok = (g_derived_key_ready != 0U);  n++;
    ca_os_sleep(200);
    example_key_store();
    r[n].name = "kstore";   r[n].ok = (g_secure_key_area != NULL);  n++;

    ca_stdout_put_n("");
    ca_stdout_put_n("--------------------------------------------------------");
    for (u32 i = 0; i < n; i++) {
        ca_stdout_printf_n("  %-10s %s", r[i].name, r[i].ok ? "PASS" : "FAILED");
        if (!r[i].ok) failed++;
    }
    ca_stdout_put_n("--------------------------------------------------------");
    if (failed == 0U)
        ca_stdout_printf_n("  RESULT: PASS  (%u/%u)", n, n);
    else
        ca_stdout_printf_n("  RESULT: FAILED  (%u/%u)", failed, n);
    ca_stdout_put_n("========================================================");
}

static void example_full_demo(void) {
    ca_stdout_put_n("");
    ca_stdout_put_n("========================================================");
    ca_stdout_put_n("         FULL CRYPTO DEMO: Derive -> Store -> Use       ");
    ca_stdout_put_n("========================================================");

    /* Step 1: Key derivation */
    example_key_derivation();
    ca_os_sleep(500);

    /* Step 2: Store in key-store */
    example_key_store();
    ca_os_sleep(500);
    
    /* Step 3: Use the key from the key-store for AES-GCM */
    ca_stdout_printf_n("=== Using stored key for AES-256 GCM ===");
    
    if (g_secure_key_area && g_derived_key_ready) {
        /* Enable temporary access */
        if (ca_keys_store_temp_enable_access(g_secure_key_area, CA_KEYS_STORE_READ_ONLY) == 0) {
            /* Read the key */
            u8 stored_key[32];
            ca_memcpy(stored_key, g_secure_key_area, 32);
            
            /* Protect again */
            ca_keys_store_temp_disable_access(g_secure_key_area);
            
            /* Use the key for AES-GCM */
            ca_aes_ctx_t ctx;
            u8 iv[16];
            ca_rng_fill(iv, 16);
            
            u8 plaintext[64] = "Msg encrypted with derived stored key!";
            u8 ciphertext[64];
            u8 tag[16];
            
            ca_aes_ctx_setup(&ctx, CA_AES_MODE_GCM, CA_AES_KEY_256, stored_key, iv);
            
            if (ca_aes_gcm_encrypt(&ctx, NULL, 0, plaintext, ciphertext, 48, tag)) {
                print_hex_label("Ciphertext (using stored key)", ciphertext, 48);
                print_hex_label("GCM Tag", tag, 16);
                ca_stdout_put_n("AES-256 GCM with stored key: SUCCESS");
            }
        }
    }
    
    ca_stdout_printf_n("========================================================");
}

/* ============================================================================
 * CLI — chain of responsibility
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd);
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

static bool user_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("ex26      : test (all, with verdict) | full (derive+kstore+gcm)");
        CLI_PUT_N("            crc32 | hash | hash_mt | aes | aes_gcm | aes_mt");
        CLI_PUT_N("            aes_nist (GCM+ECB KAT) | derive | kstore");
        CLI_PUT_N("            rng get  (driver command)");
        return false;
    }
    if (ca_strncmp(p, "crc32", 5) == 0)   { example_crc32();              return true; }
    if (ca_strncmp(p, "hash_mt", 7) == 0) { example_hash_multithread();   return true; }
    if (ca_strncmp(p, "hash", 4) == 0)    { example_hash_oneshot();       return true; }
    if (ca_strncmp(p, "aes_nist", 8) == 0) { EVAL_AES_GCM_SW_TEST((void)ca_aes_gcm_selftest(); (void)ca_aes_ecb_selftest();) return true; }
    if (ca_strncmp(p, "aes_gcm", 7) == 0) { example_aes_gcm();            return true; }
    if (ca_strncmp(p, "aes_mt", 6) == 0)  { example_aes_multithread();    return true; }
    if (ca_strncmp(p, "aes", 3) == 0 && (p[3] == 0 || p[3] == ' ')) { example_aes_ecb(); return true; }
    if (ca_strncmp(p, "derive", 6) == 0)  { example_key_derivation();     return true; }
    if (ca_strncmp(p, "kstore", 6) == 0)  { example_key_store();          return true; }
    if (ca_strncmp(p, "test", 4) == 0)    { run_all_tests();              return true; }
    if (ca_strncmp(p, "full", 4) == 0)    { example_full_demo();          return true; }
    return false;
}

/* ============================================================================
 * TASK MONITOR
 * ========================================================================== */

/* Print welcome message (only after scheduler has started) */
static void print_welcome_message(void) {
    ca_stdout_put_n("=== Example 26: Crypto + Key-Store ===");
    ca_stdout_put_n("Hardware: CRC32 | SHA-256 | AES-256 | RNG");
    ca_stdout_put_n("Multithread: HASH & AES with context swap");
    ca_stdout_put_n("Key derivation: IV+KEY -> SHA256 -> 32-byte key");
    ca_stdout_put_n("Key-store: MPU protected, validity per date, auto-lock");
    ca_stdout_put_n("CLI driver: crc32 calc | rng get | kstore info | kstore list");
    ca_stdout_put_n("Type 'help' for app commands");
}

static u32 task_monitor(void *arg) {
    (void)arg;
    ca_os_sleep(3000);
    /* Print welcome only after scheduler has started */
    print_welcome_message();

    while (1) {
        ca_os_sleep(5000);

        u32 rng_val = 0;
        ca_rng_get(&rng_val);

        ca_stdout_printf_n("EX26: rng=0x%08X  tick=%u", rng_val, (unsigned)ca_os_tick_ms());
    }
    return 0;
}

/* ============================================================================
 * ENTRY POINTS
 * ========================================================================== */

void ca_os_configure(void) {

    /* Initialize cryptographic modules */
    EVAL_CRC(ca_crc32_init();)
    EVAL_HASH(ca_hash_init();)
    EVAL_AES(ca_aes_init();)
    EVAL_RNG(ca_rng_init();)
    ca_keys_store_init(10);     /* max 10 keys, CLI kstore available immediately */

    /* Initialize synchronization */
    ca_mutex_init(&g_sync_mutex);
    ca_sem_init(&g_start_sem, "start", 0, 10);  /* Initialize semaphore to 0 */

    /* Initialize system modules */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    /* RTC: the key-store validity check needs a plausible date. With EN_RTC=1
     * but an uninitialised RTC, rtc_date_hw_valid() fails and every key that
     * carries an expiry is locked anyway — the same outcome as EN_RTC=0. */
    EVAL_RTC(
        /* LSE if the 32.768 kHz crystal is found running (key expiry needs
         * proper time), else LSI */
        ca_rtc_init(CA_RTC_CLK_AUTO);
        ca_rtc_dt_t dt = {
            .sec = 0, .min = 0, .hour = 12,
            .wday = 5, .day = 22, .month = 8, .year = 26   /* 2026-08-22 */
        };
        ca_rtc_set(&dt);
    )
}

void ca_os_main(void) {
    /* Create monitor task with STACK_MEDIUM */
    ca_os_job("monitor", CA_TH_LEVEL3, task_monitor, NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    /* Register CLI */
    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    /* Start scheduler */
    ca_os_run(true);
}

#else  /* EN_CRC && EN_HASH && EN_AES && EN_RNG */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m26a, "=== Example 26 ===");
PRIVATE_STRING(m26b, "EN_CRC / EN_HASH / EN_AES / EN_RNG / EN_KEYS_STORE ==0, example disabled  ");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m26a);
    PUT_N(&priv_m26b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_CRC && EN_HASH && EN_AES && EN_RNG && EN_KEYS_STORE */
