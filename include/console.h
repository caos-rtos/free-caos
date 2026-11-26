// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : console.h
 * @brief          : Console owner - who receives the console input bytes
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Every console input byte (UART, LPUART, USB CDC) enters through
 * ca_console_rx_byte(), in ISR context. The console has one owner at a time:
 *  - the CLI (default): the byte goes to ca_cli_rx_byte(), line editing as usual;
 *  - a task that called ca_console_take(): the byte goes to a ring buffer of
 *    CONSOLE_RX_BUF_SIZE bytes and the task reads it with ca_console_getc(),
 *    one character at a time, with no echo and no line editing (a REPL, an
 *    editor, any interactive application).
 *
 * The task gives the console back with ca_console_give(); the CLI restarts from
 * an empty line. A console left to a task that ended, faulted or restarted is
 * taken back by the next input byte: a dead owner never leaves the board deaf.
 *
 * The optional ISR filter sees each byte before the ring: it is where Ctrl-C
 * becomes an asynchronous event (MicroPython keyboard interrupt) instead of a
 * byte that waits behind a busy task.
 ******************************************************************************
 */

#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include "def_mcu.h"
#include "os_config.h"

/**
 * @brief ISR filter for a task-owned console.
 * @param byte The received byte.
 * @return true if the byte was consumed (not queued), false to queue it.
 * @note Runs in ISR context: no blocking calls.
 */
typedef bool (*ca_console_isr_filter_t)(u8 byte);

/** Console input counters. */
typedef struct {
    u32 to_task;   /**< bytes queued for the owner task */
    u32 dropped;   /**< bytes lost: ring full */
    u32 filtered;  /**< bytes consumed by the ISR filter */
    u32 reclaimed; /**< times the console was taken back from a dead owner */
} ca_console_stats_t;

/**
 * @brief Initializes the console owner (called by ca_os_init, before the RX ISRs).
 */
void ca_console_init(void);

/**
 * @brief Entry point of every console input byte (ISR context).
 * @param ch The received byte (u32 for ca_uart_rx_cb_t compatibility).
 */
void ca_console_rx_byte(u32 ch);

/**
 * @brief Takes the console for the calling task.
 * @param filter Optional ISR filter (NULL = queue every byte).
 * @return true if taken (also when the caller already owns it), false if a
 *         live task owns it or called outside a task.
 * @note Bytes received before the call stay with the CLI.
 */
bool ca_console_take(ca_console_isr_filter_t filter);

/**
 * @brief Gives the console back to the CLI, which restarts from an empty line.
 * @return true if given back, false if the caller is not the owner.
 */
bool ca_console_give(void);

/**
 * @brief Reads one byte of the console owned by the calling task.
 * @param timeout_ms CA_NO_WAIT_TIME, CA_WAIT_FOREVER or milliseconds.
 * @return the byte (0..255), or -1 on timeout or if the caller is not the owner.
 */
i32 ca_console_getc(u32 timeout_ms);

/**
 * @brief Tells whether the calling task owns the console.
 */
bool ca_console_is_mine(void);

/**
 * @brief Copies the console input counters.
 */
void ca_console_stats(ca_console_stats_t *out);

#endif /* _CONSOLE_H_ */
