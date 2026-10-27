// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : message.h
 * @brief          : Message Buffer - Framed messages for inter-task communication
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Message buffer system with framing for inter-task communication.
 * Unlike a stream, every send/receive operation is atomic:
 * - Send writes [u16 len][payload] as an indivisible unit
 * - Receive reads a complete message or nothing at all
 * This ensures message integrity during communication.
 *
 * Typical sequence:
 * 1) Message buffer initialization -> send/receive messages -> communication management
 ******************************************************************************
 */

#ifndef CAOS_MESSAGE_H
#define CAOS_MESSAGE_H

#include "common.h"
#include "atomic.h"

/* Overhead per message: 2 bytes length */
#define CA_MSG_HEADER_SIZE  2

/**
 * @brief Message Buffer structure
 */
typedef struct {
    u8 *buffer;         /**< Data buffer */
    u32 size;           /**< Total buffer size */
    volatile u32 head;  /**< Write index */
    volatile u32 tail;  /**< Read index */
    volatile u32 used;  /**< Used bytes (header + payload) */
    volatile u16 msg_count; /**< Number of messages in buffer */
    ca_spinlock_irq_t lock; /**< IRQ-safe lock */
    u32 waiting_task;   /**< Waiter HANDLE (ca_task_handle_t), CA_TASK_HANDLE_NONE = none */
} ca_message_buffer_t;

/**
 * @brief Macro to define static message buffer
 * @note size must include space for header (2B per msg)
 */
#define CA_MESSAGE_STATIC_DEFINE(name, buf_size) \
    static u8 name##_buf[buf_size]; \
    static ca_message_buffer_t name = { \
        .buffer = name##_buf, \
        .size = buf_size, \
        .head = 0, \
        .tail = 0, \
        .used = 0, \
        .msg_count = 0, \
        .lock = CA_SPINLOCK_IRQ_INIT, \
        .waiting_task = 0xFFFFFFFF \
    }

/**
 * @brief Initialize message buffer (heap)
 * @param mb Message buffer
 * @param size Total size in bytes
 * @return true if successful
 */
bool ca_message_init(ca_message_buffer_t *mb, u32 size);

/**
 * @brief Destroy message buffer
 */
void ca_message_destroy(ca_message_buffer_t *mb);

/**
 * @brief Initialize static message buffer
 */
void ca_message_init_static(ca_message_buffer_t *mb, u8 *buffer, u32 size);

/**
 * @brief Reset message buffer
 */
void ca_message_reset(ca_message_buffer_t *mb);

/**
 * @brief Send message (atomic, blocking)
 * @param mb Message buffer
 * @param data Message payload
 * @param len Payload length (max 65535)
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return true if sent, false if timeout/error
 */
bool ca_message_send(ca_message_buffer_t *mb, const void *data, u16 len, u32 timeout_ms);

/**
 * @brief Receive message (atomic, blocking)
 * @param mb Message buffer
 * @param data Destination buffer
 * @param max_len Buffer size
 * @param timeout_ms Timeout in ms (CA_NO_WAIT_TIME = no wait, CA_WAIT_FOREVER = no deadline)
 * @return Received message length, 0 if timeout/error
 */
u16 ca_message_receive(ca_message_buffer_t *mb, void *data, u16 max_len, u32 timeout_ms);

/**
 * @brief Send message from ISR (non-blocking)
 * @return true if sent
 */
bool ca_message_send_isr(ca_message_buffer_t *mb, const void *data, u16 len);

/**
 * @brief Receive message from ISR (non-blocking)
 * @return Message length, 0 if empty
 */
u16 ca_message_receive_isr(ca_message_buffer_t *mb, void *data, u16 max_len);

/**
 * @brief Peek next message length (without removing)
 * @return Payload length, 0 if empty
 */
u16 ca_message_peek_size(ca_message_buffer_t *mb);

/**
 * @brief Number of messages in buffer
 */
u16 ca_message_count(ca_message_buffer_t *mb);

/**
 * @brief Free space in buffer
 */
u32 ca_message_space_available(ca_message_buffer_t *mb);

/**
 * @brief Buffer empty?
 */
bool ca_message_is_empty(ca_message_buffer_t *mb);

#endif /* CAOS_MESSAGE_H */
