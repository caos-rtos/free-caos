// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    hash.h
 * @brief   HASH driver public API — hardware SHA-1/SHA-256
 * @note    Available on: H5xx (H563), H7xx (H753)
 *          F1xx/F4xx/F7xx and H747: stub (return false)
 *
 *          Context swap: the HASH processor supports saving/restoring
 *          CSR[] registers allowing concurrent use between different tasks.
 *          ca_hash_ctx_t encapsulates the state; _update/_final functions
 *          perform automatic save/restore via mutex.
 ******************************************************************************
 */

#ifndef HASH_H
#define HASH_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_HASH

typedef enum {
    CA_HASH_ALGO_SHA1   = 0,
    CA_HASH_ALGO_SHA256 = 1,
} ca_hash_algo_t;

/* max digest size (SHA-256 = 32 bytes, SHA-1 = 20 bytes) */
#define HASH_SHA1_DIGEST_SIZE    20
#define HASH_SHA256_DIGEST_SIZE  32
#define HASH_MAX_DIGEST_SIZE     32

/* The STM32 HASH context swap only checkpoints on 64-byte block boundaries and
 * cannot accumulate sub-block feeds reliably; instead ca_hash_ctx_update()
 * buffers the whole message (heap, grown on demand) and ca_hash_ctx_final()
 * hashes it in one HW pass. Initial buffer covers small hashes (key derivation)
 * without reallocation; the SSH exchange hash H (I_C ~1.5KB) grows it once. */
#ifndef HASH_CTX_INIT_CAP
#define HASH_CTX_INIT_CAP  256U
#endif

typedef struct {
    ca_hash_algo_t algo;
    bool started;              /* an update has begun buffering              */
    bool error;                /* allocation failed -> final() fails         */
    u32  len;                  /* bytes buffered so far                      */
    u32  cap;                  /* current buf capacity                       */
    u8  *buf;                  /* heap buffer (ca_memory_allocate)           */
} ca_hash_ctx_t;

/* init hardware HASH (call once) */
bool ca_hash_init(void);

/* one-shot: calculate digest on full buffer */
bool ca_hash_compute(ca_hash_algo_t algo, const u8 *data, u32 len,
                     u8 *digest, u32 *digest_len);

/* API with context for incremental / concurrent calculations */
bool ca_hash_ctx_init(ca_hash_ctx_t *ctx, ca_hash_algo_t algo);
bool ca_hash_ctx_update(ca_hash_ctx_t *ctx, const u8 *data, u32 len);
bool ca_hash_ctx_final(ca_hash_ctx_t *ctx, u8 *digest, u32 *digest_len);
/* Release the heap buffer; only needed if final() is not called (abort path). */
void ca_hash_ctx_free(ca_hash_ctx_t *ctx);

#endif /* EN_HASH */
#endif /* HASH_H */
