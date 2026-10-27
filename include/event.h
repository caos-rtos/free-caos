// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : event.h
 * @brief          : Event Groups (32-bit flags for multi-task sync)
 * @author         : Angelo Coppi CAOS
 ******************************************************************************
 * @details
 * Event Groups for multi-task synchronization based on bit flag patterns.
 *
 * Features:
 * - 32 bit flags (u32)
 * - Wait ANY: wake up when at least ONE of the requested bits is set
 * - Wait ALL: wake up when ALL requested bits are set
 * - Auto-clear: clear bits after wait (optional)
 * - Set/Clear thread-safe and ISR-safe
 * - Timeout on wait
 *
 * Use cases:
 * - State machine: wait for "calibration_done OR timeout_flag"
 * - Sync barrier: all tasks wait for a common bit
 * - Multi-condition: "data_ready AND buffer_free"
 ******************************************************************************
 */

#ifndef EVENT_H
#define EVENT_H

#include "common.h"
#include "atomic.h"
#include "mpu.h"
#include "scheduler.h"
#include "sync.h"

/* ============================================================================
 * EVENT GROUP CONSTANTS
 * ========================================================================== */

/**
 * @brief Wait mode
 */
#define CA_EVENT_WAIT_ANY  0  /**< Wake up when at least ONE bit is set */
#define CA_EVENT_WAIT_ALL  1  /**< Wake up when ALL bits are set */

/**
 * @brief Bit helpers
 */
#define CA_EVENT_BIT(n)  (1U << (n))  /**< Mask for single bit */

/* ============================================================================
 * EVENT GROUP STRUCTURES
 * ========================================================================== */

/**
 * @brief Metadata for tasks waiting on event group
 * @note Static array indexed by task index
 */
typedef struct {
    u32 wait_bits;       /**< Bit mask to wait for */
    u8  wait_mode;       /**< CA_EVENT_WAIT_ANY or CA_EVENT_WAIT_ALL */
    u8  auto_clear;      /**< If true, clear bits after match */
    volatile u32 result; /**< Matched bits, set by wakeup_waiters; 0 = not signaled */
} ca_event_waiter_meta_t;

/**
 * @brief Event Group structure
 * @note User allocates the structure (global, stack, heap)
 */
typedef struct {
    volatile u32 bits;                    /**< 32 bit flags */
    ca_wait_queue_t wait_queue;               /**< Tasks waiting */
    ca_spinlock_irq_t lock;                   /**< Concurrent access protection */
    ca_event_waiter_meta_t waiters[MAX_TASK];    /**< Metadata for task index */
} ca_event_group_t;

/* ============================================================================
 * EVENT GROUP API
 * ========================================================================== */

/**
 * @brief Initialize event group
 * @param eg Pointer to the event group
 */
void ca_event_init(ca_event_group_t *eg);

/**
 * @brief Set bit flags (OR operation)
 * @param eg Pointer to the event group
 * @param bits_to_set Bit mask to set
 * @return Bit value after operation
 */
u32 ca_event_set(ca_event_group_t *eg, u32 bits_to_set);

/**
 * @brief Clear bit flags (AND NOT operation)
 * @param eg Pointer to the event group
 * @param bits_to_clear Bit mask to clear
 * @return Bit value after operation
 */
u32 ca_event_clear(ca_event_group_t *eg, u32 bits_to_clear);

/**
 * @brief Set bits from ISR (non-blocking)
 * @param eg Pointer to the event group
 * @param bits_to_set Bit mask to set
 * @return Bit value after operation
 */
u32 ca_event_set_isr(ca_event_group_t *eg, u32 bits_to_set);

/**
 * @brief Clear bits from ISR (non-blocking)
 * @param eg Pointer to the event group
 * @param bits_to_clear Bit mask to clear
 * @return Bit value after operation
 */
u32 ca_event_clear_isr(ca_event_group_t *eg, u32 bits_to_clear);

/**
 * @brief Wait on bit flags (blocking with timeout)
 * @param eg Pointer to the event group
 * @param bits_to_wait Bit mask to wait for
 * @param wait_mode CA_EVENT_WAIT_ANY or CA_EVENT_WAIT_ALL
 * @param auto_clear If true, clear matched bits after wake
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Bit value at the moment of match, or 0 if timeout
 *
 * @details
 * - WAIT_ANY: returns when at least ONE bit among bits_to_wait is set
 * - WAIT_ALL: returns when ALL bits in bits_to_wait are set
 * - auto_clear: if true, matched bits are cleared atomically
 */
u32 ca_event_wait(ca_event_group_t *eg, u32 bits_to_wait, u8 wait_mode,
                  bool auto_clear, u32 timeout_ms);

/**
 * @brief Wait for ANY bit (at least one)
 * @param eg Pointer to the event group
 * @param bits_to_wait Bit mask to wait for
 * @param auto_clear If true, clear matched bits
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Bit value at the moment of match, or 0 if timeout
 */
static inline u32 ca_event_wait_any(ca_event_group_t *eg, u32 bits_to_wait,
                                     bool auto_clear, u32 timeout_ms) {
    return ca_event_wait(eg, bits_to_wait, CA_EVENT_WAIT_ANY, auto_clear, timeout_ms);
}

/**
 * @brief Wait for ALL bits (all)
 * @param eg Pointer to the event group
 * @param bits_to_wait Bit mask to wait for
 * @param auto_clear If true, clear matched bits
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Bit value at the moment of match, or 0 if timeout
 */
static inline u32 ca_event_wait_all(ca_event_group_t *eg, u32 bits_to_wait,
                                     bool auto_clear, u32 timeout_ms) {
    return ca_event_wait(eg, bits_to_wait, CA_EVENT_WAIT_ALL, auto_clear, timeout_ms);
}

/**
 * @brief Read current bit value (non-blocking)
 * @param eg Pointer to the event group
 * @return Current value of bit flags
 */
u32 ca_event_get(ca_event_group_t *eg);

/**
 * @brief Reset event group (clear all bits)
 * @param eg Pointer to the event group
 */
void ca_event_reset(ca_event_group_t *eg);

#endif /* EVENT_H */
