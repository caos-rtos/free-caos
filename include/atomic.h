// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : atomic.h
 * @brief          : Atomic Lock for Cortex-M4
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Implementation of spinlocks for atomic operations on Cortex-M4.
 * Three modes available:
 * - IRQ-safe: disables interrupts during short critical sections
 * - Cooperative: allows yield during long critical sections
 * - Adaptive: combines both strategies based on the situation
 *
 * Typical sequence:
 * 1) Spinlock initialization -> acquisition -> resource usage -> release
 ******************************************************************************
 */

#ifndef ATOMIC_LOCK_H
#define ATOMIC_LOCK_H

#include "common.h"
#include "safeguard.h"


/* ============================================================================
 * SPINLOCK TYPES
 * ========================================================================== */

#define SPINLOCK_FREE  0
#define SPINLOCK_TAKEN 0x08091961


/**
 * @brief Base spinlock - DO NOT use between threads (for reference only)
 */
typedef struct  ca_spinlock_s{
    volatile u32 key;
}  ca_spinlock_t;

/**
 * @brief IRQ-safe spinlock - for SHORT critical sections (<100µs)
 *        Disables interrupts during acquisition
 */
typedef struct ca_spinlock_irq_s{
    volatile u32 key;
    u32 irq_state;
}   ca_spinlock_irq_t;

/* ============================================================================
 * STATIC INITIALIZERS (for compile-time initialization)
 * ========================================================================== */

/**
 * @brief Static initializer for IRQ-safe spinlock
 * @note Use only for static declarations, not at runtime
 */
#define CA_SPINLOCK_IRQ_INIT  { .key = SPINLOCK_FREE, .irq_state = 0 }

/**
 * @brief Static initializer for cooperative spinlock
 * @note Use only for static declarations, not at runtime
 */
#define CA_SPINLOCK_COOP_INIT  { .key = SPINLOCK_FREE }

/* ============================================================================
 * SPINLOCK IRQ-SAFE API (short critical sections)
 * ========================================================================== */

/**
 * @brief Initializes IRQ-safe spinlock
 * @param lock Pointer to the spinlock
 */
void ca_spinlock_irq_init(ca_spinlock_irq_t *lock);

/**
 * @brief Acquires lock by disabling interrupts
 *        Use for SHORT critical sections (<100µs)
 * @param lock Pointer to the spinlock
 * @note Blocks ALL interrupts - guaranteed but limited latency
 */
void ca_spinlock_lock_irq(ca_spinlock_irq_t *lock);

/**
 * @brief Releases lock and restores interrupt state
 * @param lock Pointer to the spinlock
 */
void ca_spinlock_unlock_irq(ca_spinlock_irq_t *lock);

/**
 * @brief Attempts lock acquisition without blocking
 * @param lock Pointer to the spinlock
 * @return true if lock acquired, false otherwise
 */
bool ca_spinlock_trylock_irq(ca_spinlock_irq_t *lock);

/* ============================================================================
 * SPINLOCK COOPERATIVE API (long critical sections)
 * ========================================================================== */

/**
 * @brief Initializes cooperative spinlock
 * @param lock Pointer to the spinlock
 */
void ca_spinlock_cooperative_init(ca_spinlock_t *lock);

/**
 * @brief Acquires lock with cooperative yield
 *        Use for LONG critical sections (>100µs)
 * @param lock Pointer to the spinlock
 * @note Yields to the scheduler if lock is occupied - other threads continue
 */
void ca_spinlock_lock_cooperative(ca_spinlock_t *lock);

/**
 * @brief Releases cooperative lock
 * @param lock Pointer to the spinlock
 */
void ca_spinlock_unlock_cooperative(ca_spinlock_t *lock);

/**
 * @brief Attempts cooperative lock acquisition without blocking
 * @param lock Pointer to the spinlock
 * @return true if lock acquired, false otherwise
 */
bool ca_spinlock_trylock_cooperative(ca_spinlock_t *lock);

/* ============================================================================
 * SPINLOCK ADAPTIVE API (optimized)
 * ========================================================================== */

/**
 * @brief Acquires lock with adaptive strategy
 *        - First attempts fast spin (avoids context switch)
 *        - Then cooperative yield if necessary
 * @param lock Pointer to the spinlock
 */
void ca_spinlock_lock_adaptive(ca_spinlock_t *lock);

/* ============================================================================
 * UTILITY
 * ========================================================================== */

/**
 * @brief Reads lock state without modifying it
 * @param key Pointer to the lock key
 * @return Current value (SPINLOCK_FREE or SPINLOCK_TAKEN)
 */
u32 ca_spinlock_get(volatile u32 *key);

#endif /* ATOMIC_LOCK_H */