// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : queue.h
 * @brief          : Message Queue (FIFO/LIFO thread-safe)
 * @author         : Angelo Coppi CAOS
 ******************************************************************************
 * @details
 * Queue implementation for inter-task communication.
 *
 * Two variants:
 * 1. Static Queue: structure + inline buffer (everything allocated by the user)
 * 2. Heap Queue: buffer dynamically allocated by ca_memory_allocate()
 *
 * Features:
 * - Send/Receive with blocking and timeout
 * - TrySend/TryReceive non-blocking
 * - SendFromISR/ReceiveFromISR for interrupt context
 * - SendToFront (LIFO mode)
 * - Peek (read without removing)
 * - Configurable item size (u8, u32, custom struct)
 ******************************************************************************
 */

#ifndef QUEUE_H
#define QUEUE_H

#include "common.h"
#include "atomic.h"
#include "scheduler.h"
#include "sync.h"

/* ============================================================================
 * QUEUE STRUCTURES
 * ========================================================================== */

/**
 * @brief Queue with dynamically allocated buffer (heap)
 * @note User allocates the structure, buffer managed by ca_memory_allocate()
 */
typedef struct {
    u8 *buffer;                  // Buffer items (allocated on heap)
    u32 item_size;               // Single item size (bytes)
    u32 max_items;               // Maximum capacity
    volatile u32 count;          // Number of items present
    volatile u32 head;           // Read index (consumer)
    volatile u32 tail;           // Write index (producer)
    ca_wait_queue_t wait_send;       // Task waiting for send (queue full)
    ca_wait_queue_t wait_recv;       // Task waiting for receive (queue empty)
    ca_spinlock_irq_t lock;          // Concurrent access protection
} ca_queue_t;

/**
 * @brief Queue with static inline buffer
 * @note User allocates the entire structure (global, stack, heap)
 */
typedef struct {
    u32 item_size;               // Single item size (bytes)
    u32 max_items;               // Maximum capacity
    volatile u32 count;          // Number of items present
    volatile u32 head;           // Read index (consumer)
    volatile u32 tail;           // Write index (producer)
    ca_wait_queue_t wait_send;       // Task waiting for send (queue full)
    ca_wait_queue_t wait_recv;       // Task waiting for receive (queue empty)
    ca_spinlock_irq_t lock;          // Concurrent access protection
    u8 buffer[];                 // Flexible array member (inline buffer)
} ca_queue_static_t;

/* ============================================================================
 * MACRO HELPERS
 * ========================================================================== */

/**
 * @brief Calculates total size for static queue
 * @param max_items Maximum number of items
 * @param item_size Single item size (bytes)
 */
#define CAQUEUE_STATIC_SIZE(max_items, item_size) \
    (sizeof(ca_queue_static_t) + ((max_items) * (item_size)))

/**
 * @brief Defines and initializes global static queue
 * @param name Queue variable name
 * @param max_items Maximum number of items
 * @param item_size Single item size (bytes)
 *
 * Example:
 *   CAQUEUE_STATIC_DEFINE(my_queue, 10, sizeof(u32));
 */
/* Union: ca_queue_static_t view (flexible array) + sized raw storage.
 * ALWAYS access via .q — never cast the union to ca_queue_static_t*
 * (strict-aliasing UB). Punning via union is legal (C11 6.5.2.3). */
#define CAQUEUE_STATIC_DEFINE(NAME, MAX_ITEM, ITEM_SIZE) \
    union { \
        ca_queue_static_t q; \
        u8 raw[sizeof(ca_queue_static_t) + (MAX_ITEM) * (ITEM_SIZE)]; \
    } NAME = { .q = { \
        .item_size = ITEM_SIZE, \
        .max_items = MAX_ITEM, \
        .count = 0, \
        .head = 0, \
        .tail = 0, \
        .wait_send = {NULL, NULL}, \
        .wait_recv = {NULL, NULL}, \
        .lock = {SPINLOCK_FREE, 0} \
    } }

/* ============================================================================
 * STATIC QUEUE API (inline buffer)
 * ========================================================================== */

/**
 * @brief Initializes static queue
 * @param q Pointer to the queue
 * @param item_size Single item size (bytes)
 * @param max_items Maximum capacity
 * @note Call only if NOT using the CAQUEUE_STATIC_DEFINE macro
 */
void ca_queue_static_init(ca_queue_static_t *q, u32 item_size, u32 max_items);

/**
 * @brief Sends item (blocking if full)
 * @param q Pointer to the queue
 * @param item Pointer to the data to copy
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if sent, false if timeout
 */
bool ca_queue_static_send(ca_queue_static_t *q, const void *item, u32 timeout_ms);
/**
 * @brief Receives item (blocking if empty)
 * @param q Pointer to the queue
 * @param item Pointer to the destination buffer
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if received, false if timeout
 */
bool ca_queue_static_receive(ca_queue_static_t *q, void *item, u32 timeout_ms);

/**
 * @brief Attempts send without blocking
 * @param q Pointer to the queue
 * @param item Pointer to the data to copy
 * @return true if sent, false if full
 */
bool ca_queue_static_trysend(ca_queue_static_t *q, const void *item);
/**
 * @brief Attempts receive without blocking
 * @param q Pointer to the queue
 * @param item Pointer to the destination buffer
 * @return true if received, false if empty
 */
bool ca_queue_static_tryreceive(ca_queue_static_t *q, void *item);

/**
 * @brief Sends item from ISR (non-blocking)
 * @param q Pointer to the queue
 * @param item Pointer to the data to copy
 * @return true if sent, false if full
 * @note CANNOT block, call only from interrupt handler
 */
bool ca_queue_static_send_isr(ca_queue_static_t *q, const void *item);

/**
 * @brief Receives item from ISR (non-blocking)
 * @param q Pointer to the queue
 * @param item Pointer to the destination buffer
 * @return true if received, false if empty
 * @note CANNOT block, call only from interrupt handler
 */
bool ca_queue_static_receive_isr(ca_queue_static_t *q, void *item);

/**
 * @brief Sends to front (LIFO mode)
 * @param q Pointer to the queue
 * @param item Pointer to the data to copy
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if sent, false if timeout
 */
bool ca_queue_static_send_front(ca_queue_static_t *q, const void *item, u32 timeout_ms);

/**
 * @brief Mailbox write: replaces the item of a ONE-item queue
 * @details The slot is written whether or not it held an unread item; never
 *          waits, callable from an ISR. The equivalent of FreeRTOS
 *          xQueueOverwrite.
 * @param q Pointer to the queue (max_items must be 1)
 * @param item Pointer to the data to copy
 * @return true if written, false if q/item is NULL or the queue is not one item long
 */
bool ca_queue_static_overwrite(ca_queue_static_t *q, const void *item);

/**
 * @brief Reads item without removing it
 * @param q Pointer to the queue
 * @param item Pointer to the destination buffer
 * @return true if read, false if empty
 */
bool ca_queue_static_peek(ca_queue_static_t *q, void *item);

/**
 * @brief Resets queue (clears it)
 * @param q Pointer to the queue
 */
void ca_queue_static_reset(ca_queue_static_t *q);

/**
 * @brief Checks if empty
 * @param q Pointer to the queue
 * @return true if empty
 */
bool ca_queue_static_is_empty(ca_queue_static_t *q);

/**
 * @brief Checks if full
 * @param q Pointer to the queue
 * @return true if full
 */
bool ca_queue_static_is_full(ca_queue_static_t *q);

/**
 * @brief Get number of items present
 * @param q Pointer to the queue
 * @return Number of items
 */
u32 ca_queue_static_count(ca_queue_static_t *q);

/**
 * @brief Get free space
 * @param q Pointer to the queue
 * @return Number of free slots
 */
u32 ca_queue_static_available(ca_queue_static_t *q);

/* ============================================================================
 * HEAP QUEUE API (dynamically allocated buffer)
 * ========================================================================== */

/**
 * @brief Initializes heap queue (allocates buffer)
 * @param q Pointer to the queue
 * @param item_size Size of a single item (bytes)
 * @param max_items Maximum capacity
 * @return true if successful, false if heap is full
 */
bool ca_queue_heap_init(ca_queue_t *q, u32 item_size, u32 max_items);

/**
 * @brief Destroys heap queue (frees buffer)
 * @param q Pointer to the queue
 */
void ca_queue_heap_destroy(ca_queue_t *q);

/**
 * @brief Sends item (blocking if full)
 * @param q Pointer to the queue
 * @param item Pointer to the data to copy
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if sent, false if timeout
 */
bool ca_queue_heap_send(ca_queue_t *q, const void *item, u32 timeout_ms);

/**
 * @brief Receives item (blocking if empty)
 * @param q Pointer to the queue
 * @param item Pointer to the destination buffer
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if received, false if timeout
 */
bool ca_queue_heap_receive(ca_queue_t *q, void *item, u32 timeout_ms);

/**
 * @brief Attempts send without blocking
 * @param q Pointer to the queue
 * @param item Pointer to the data to copy
 * @return true if sent, false if full
 */
bool ca_queue_heap_trysend(ca_queue_t *q, const void *item);

/**
 * @brief Attempts receive without blocking
 * @param q Pointer to the queue
 * @param item Pointer to the destination buffer
 * @return true if received, false if empty
 */
bool ca_queue_heap_tryreceive(ca_queue_t *q, void *item);

/**
 * @brief Sends item from ISR (non-blocking)
 * @param q Pointer to the queue
 * @param item Pointer to the data to copy
 * @return true if sent, false if full
 */
bool ca_queue_heap_send_isr(ca_queue_t *q, const void *item);

/**
 * @brief Receives item from ISR (non-blocking)
 * @param q Pointer to the queue
 * @param item Pointer to the destination buffer
 * @return true if received, false if empty
 */
bool ca_queue_heap_receive_isr(ca_queue_t *q, void *item);

/**
 * @brief Sends to head (LIFO mode)
 * @param q Pointer to the queue
 * @param item Pointer to the data to copy
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if sent, false if timeout
 */
bool ca_queue_heap_send_front(ca_queue_t *q, const void *item, u32 timeout_ms);

/**
 * @brief Mailbox write: replaces the item of a ONE-item queue
 * @details The slot is written whether or not it held an unread item; never
 *          waits, callable from an ISR. The equivalent of FreeRTOS
 *          xQueueOverwrite.
 * @param q Pointer to the queue (max_items must be 1)
 * @param item Pointer to the data to copy
 * @return true if written, false if q/item is NULL or the queue is not one item long
 */
bool ca_queue_heap_overwrite(ca_queue_t *q, const void *item);

/**
 * @brief Reads item without removing it
 * @param q Pointer to the queue
 * @param item Pointer to the destination buffer
 * @return true if read, false if empty
 */
bool ca_queue_heap_peek(ca_queue_t *q, void *item);

/**
 * @brief Resets queue (empties it)
 * @param q Pointer to the queue
 */
void ca_queue_heap_reset(ca_queue_t *q);

/**
 * @brief Checks if empty
 * @param q Pointer to the queue
 * @return true if empty
 */
bool ca_queue_heap_is_empty(ca_queue_t *q);

/**
 * @brief Checks if full
 * @param q Pointer to the queue
 * @return true if full
 */
bool ca_queue_heap_is_full(ca_queue_t *q);

/**
 * @brief Get number of items present
 * @param q Pointer to the queue
 * @return Number of items
 */
u32 ca_queue_heap_count(ca_queue_t *q);

/**
 * @brief Get free space
 * @param q Pointer to the queue
 * @return Number of free slots
 */
u32 ca_queue_heap_available(ca_queue_t *q);

#endif /* QUEUE_H */
