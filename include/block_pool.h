// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : block_pool.h
 * @brief          : O(1) fixed-block allocator, ISR-safe
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Fixed-size memory pool. Alloc and free are O(1) and ISR-safe.
 * The free list is linked in-place within the blocks (no external structure).
 * There is no fragmentation: all blocks have the same size.
 *
 * Typical usage:
 *   static u8 buf[16 * 32];
 *   static ca_block_pool_t bp;
 *   ca_block_pool_init(&bp, buf, sizeof(buf), 32);
 *
 *   void *p = ca_block_alloc(&bp);   // NULL if exhausted
 *   ca_block_free(&bp, p);
 ******************************************************************************
 */

#ifndef BLOCK_POOL_H
#define BLOCK_POOL_H

#include "common.h"
#include "atomic.h"
#include "os_config.h"

#if EN_BLOCK_POOL == 1

/* ============================================================================
 * STRUCTURE
 * ========================================================================== */

typedef struct {
    void         *free_list;   /* first free block (in-place linked list) */
    u32           block_size;  /* size of each block (>= 4, multiple of 4) */
    u32           total;       /* total number of blocks */
    volatile u32  free_count;  /* currently free blocks */
    ca_spinlock_irq_t lock;        /* ISR-safe spinlock */
} ca_block_pool_t;

/* ============================================================================
 * API
 * ========================================================================== */

/**
 * @brief Initialize the pool
 * @param bp         Pool to initialize
 * @param buf        Backing buffer (must remain allocated for the entire life of the pool)
 * @param buf_size   Total buffer size in bytes
 * @param block_size Size of each block (rounded to 4 bytes, minimum 4)
 * @note  buf must be 4-byte aligned (use __attribute__((aligned(4))))
 */
void  ca_block_pool_init(ca_block_pool_t *bp, void *buf, u32 buf_size, u32 block_size);

/**
 * @brief Allocate a block (O(1), ISR-safe)
 * @return Pointer to the block, NULL if pool is exhausted
 */
void *ca_block_alloc(ca_block_pool_t *bp);

/**
 * @brief Release a block (O(1), ISR-safe)
 * @param ptr Pointer returned by ca_block_alloc(); ignored if NULL
 */
void  ca_block_free(ca_block_pool_t *bp, void *ptr);

/**
 * @brief Number of free blocks in the pool
 */
u32   ca_block_pool_free_count(ca_block_pool_t *bp);

/**
 * @brief Total number of blocks in the pool
 */
u32   ca_block_pool_total(ca_block_pool_t *bp);

#endif /* EN_BLOCK_POOL */
#endif /* BLOCK_POOL_H */
