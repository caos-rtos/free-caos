// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : stream.h
 * @brief          : Stream Buffer - FIFO byte stream for inter-task communication
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Buffer system for FIFO byte stream for inter-task communication.
 * Implements a circular stream with send/receive functionality
 * with timeout and support for interrupt contexts. Unlike the message
 * buffer, the stream works at the byte level and not with structured messages.
 *
 * Typical sequence:
 * 1) Stream buffer initialization -> send/receive data -> communication management
 ******************************************************************************
 */

#ifndef CAOS_STREAM_H
#define CAOS_STREAM_H

#include "common.h"
#include "atomic.h"

/**
 * @brief Stream Buffer structure (circular byte buffer)
 */
typedef struct {
    u8 *buffer;         /**< Data buffer */
    u32 size;           /**< Buffer size */
    volatile u32 head;  /**< Write index */
    volatile u32 tail;  /**< Read index */
    volatile u32 count; /**< Bytes in buffer */
    ca_spinlock_irq_t lock; /**< IRQ-safe lock */
    u32 waiting_task;   /**< Waiter HANDLE (ca_task_handle_t), CA_TASK_HANDLE_NONE = none */
    u32 trigger_level;  /**< Minimum bytes to wake receiver */
} ca_stream_buffer_t;

/**
 * @brief Macro to define static stream buffer
 */
#define CA_STREAM_STATIC_DEFINE(name, buf_size) \
    static u8 name##_buf[buf_size]; \
    static ca_stream_buffer_t name = { \
        .buffer = name##_buf, \
        .size = buf_size, \
        .head = 0, \
        .tail = 0, \
        .count = 0, \
        .lock = CA_SPINLOCK_IRQ_INIT, \
        .waiting_task = 0xFFFFFFFF, \
        .trigger_level = 1 \
    }

/**
 * @brief Initialize stream buffer (heap)
 * @param sb Stream buffer
 * @param size Size in bytes
 * @return true if successful
 */
bool ca_stream_init(ca_stream_buffer_t *sb, u32 size);

/**
 * @brief Destroy stream buffer (free memory)
 */
void ca_stream_destroy(ca_stream_buffer_t *sb);

/**
 * @brief Initialize static stream buffer
 * @param sb Stream buffer
 * @param buffer External buffer
 * @param size Buffer size
 */
void ca_stream_init_static(ca_stream_buffer_t *sb, u8 *buffer, u32 size);

/**
 * @brief Reset stream buffer (clear)
 */
void ca_stream_reset(ca_stream_buffer_t *sb);

/**
 * @brief Set trigger level to wake receiver
 * @param sb Stream buffer
 * @param level Minimum bytes to wake (default 1)
 */
void ca_stream_set_trigger(ca_stream_buffer_t *sb, u32 level);

/**
 * @brief Send bytes to stream (blocking with timeout)
 * @param sb Stream buffer
 * @param data Data to send
 * @param len Number of bytes
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Bytes actually written
 */
u32 ca_stream_send(ca_stream_buffer_t *sb, const u8 *data, u32 len, u32 timeout_ms);

/**
 * @brief Receive bytes from stream (blocking with timeout)
 * @param sb Stream buffer
 * @param data Destination buffer
 * @param max_len Max bytes to read
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Bytes actually read
 */
u32 ca_stream_receive(ca_stream_buffer_t *sb, u8 *data, u32 max_len, u32 timeout_ms);

/**
 * @brief Send bytes from ISR (non-blocking)
 * @return Bytes written
 */
u32 ca_stream_send_isr(ca_stream_buffer_t *sb, const u8 *data, u32 len);

/**
 * @brief Receive bytes from ISR (non-blocking)
 * @return Bytes read
 */
u32 ca_stream_receive_isr(ca_stream_buffer_t *sb, u8 *data, u32 max_len);

/**
 * @brief Bytes available for reading
 */
u32 ca_stream_bytes_available(ca_stream_buffer_t *sb);

/**
 * @brief Free space for writing
 */
u32 ca_stream_space_available(ca_stream_buffer_t *sb);

/**
 * @brief Stream empty?
 */
bool ca_stream_is_empty(ca_stream_buffer_t *sb);

/**
 * @brief Stream full?
 */
bool ca_stream_is_full(ca_stream_buffer_t *sb);

#endif /* CAOS_STREAM_H */
