// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : notify.h
 * @brief          : Task Notification (Direct-to-Task signaling)
 * @author         : Angelo Coppi CAOS
 ******************************************************************************
 * @details
 * Task Notification for lightweight task-to-task signaling.
 *
 * Lightweight alternative to Queue/Semaphore for simple cases:
 * - Zero memory overhead (uses notify_value in ca_thread_context_t)
 * - Faster than queue for simple signaling
 * - Only one u32 value per task
 *
 * Use cases:
 * - "Data ready" flag
 * - "Work done" counter
 * - Simple event signaling
 ******************************************************************************
 */

#ifndef NOTIFY_H
#define NOTIFY_H

#include "common.h"
#include "scheduler.h"

/* ============================================================================
 * NOTIFICATION ACTIONS
 * ========================================================================== */

/**
 * @brief Actions for notification value modification
 */
#define CA_NOTIFY_ACTION_NONE       0  /**< No modification */
#define CA_NOTIFY_ACTION_SET_BITS   1  /**< OR with value (set bit) */
#define CA_NOTIFY_ACTION_CLEAR_BITS 2  /**< AND NOT (clear bit) */
#define CA_NOTIFY_ACTION_OVERWRITE  3  /**< Overwrite value */
#define CA_NOTIFY_ACTION_INCREMENT  4  /**< Increment (saturation at 0xFFFFFFFF) */
#define CA_NOTIFY_ACTION_SET_IF_EMPTY 5 /**< Write value only if none is pending (value 0),
                                            otherwise leave it and fail */

/* ============================================================================
 * TASK NOTIFICATION API
 * ========================================================================== */

/**
 * @brief Send notification to task
 * @param task_idx Destination task index
 * @param value Value to write/modify
 * @param action CA_NOTIFY_ACTION_*
 * @return true if sent, false if task invalid or SET_IF_EMPTY found a value pending
 *
 * @details
 * - SET_BITS: notify_value |= value
 * - CLEAR_BITS: notify_value &= ~value
 * - OVERWRITE: notify_value = value
 * - INCREMENT: notify_value++ (saturation)
 * - SET_IF_EMPTY: notify_value = value only if it is 0 (nothing pending);
 *   otherwise nothing changes, nobody is woken, false is returned
 * - Wake up task if waiting
 */
CA_THID_API bool ca_notify_send(u32 task_idx, u32 value, u8 action);

/**
 * @brief Send notification from ISR (non-blocking)
 * @param task_idx Destination task index
 * @param value Value to write/modify
 * @param action CA_NOTIFY_ACTION_*
 * @return true if sent, false if task invalid or SET_IF_EMPTY found a value pending
 */
CA_THID_API bool ca_notify_send_isr(u32 task_idx, u32 value, u8 action);

/**
 * @brief Send notification to a task named by HANDLE
 * @param h Destination handle (ca_os_task_handle / ca_os_handle_from_thid)
 * @return true if sent, false if that task has ended or SET_IF_EMPTY
 *         found a value pending
 * @details Prefer this to ca_notify_send() whenever the destination is kept
 *          across time: a thid is a position in the task list and changes when
 *          a task below it ends, so an index saved at creation can end up
 *          naming another task. The handle is resolved at the moment of the
 *          send, under the same IRQ mask as the send itself.
 */
bool ca_notify_send_handle(ca_task_handle_t h, u32 value, u8 action);

/** @brief ca_notify_send_handle() from ISR */
bool ca_notify_send_handle_isr(ca_task_handle_t h, u32 value, u8 action);

/**
 * @brief Wait for notification (blocking with timeout)
 * @param clear_on_exit If true, clear notify_value after wait
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Received notify_value, 0 if timeout
 *
 * @details
 * - Blocks task until notify_value != 0 or timeout
 * - clear_on_exit: if true, clears notify_value atomically
 */
u32 ca_notify_wait(bool clear_on_exit, u32 timeout_ms);

/**
 * @brief Read notification value without blocking
 * @param task_idx Task index (0xFFFFFFFF = current task)
 * @return Current notify_value
 */
u32 ca_notify_get(u32 task_idx);

/**
 * @brief Clear notification value
 * @param task_idx Task index (0xFFFFFFFF = current task)
 * @return Previous notify_value
 */
u32 ca_notify_clear(u32 task_idx);

/* ============================================================================
 * INLINE HELPERS
 * ========================================================================== */

/**
 * @brief Send notification with overwrite (most common)
 * @param h Destination task handle
 * @param value Value to write
 * @return true if sent
 */
static inline bool ca_notify_give(ca_task_handle_t h, u32 value) {
    return ca_notify_send_handle(h, value, CA_NOTIFY_ACTION_OVERWRITE);
}

/**
 * @brief Send notification by incrementing counter
 * @param h Destination task handle
 * @return true if sent
 */
static inline bool ca_notify_give_inc(ca_task_handle_t h) {
    return ca_notify_send_handle(h, 0, CA_NOTIFY_ACTION_INCREMENT);
}

/**
 * @brief Send notification from ISR with overwrite
 * @param h Destination task handle
 * @param value Value to write
 * @return true if sent
 */
static inline bool ca_notify_give_isr(ca_task_handle_t h, u32 value) {
    return ca_notify_send_handle_isr(h, value, CA_NOTIFY_ACTION_OVERWRITE);
}

/**
 * @brief Wait for notification and clear
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Received value, 0 if timeout
 */
static inline u32 ca_notify_take(u32 timeout_ms) {
    return ca_notify_wait(true, timeout_ms);
}

/**
 * @brief Wait for notification without clearing
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Received value, 0 if timeout
 */
static inline u32 ca_notify_peek(u32 timeout_ms) {
    return ca_notify_wait(false, timeout_ms);
}

#endif /* NOTIFY_H */
