// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : stdout.h
 * @brief          : Stdout output channel for caOS
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Asynchronous stdout channel for output to terminal/UART/USB-CDC/ITM.
 *
 * Architecture:
 * - Dedicated "stdout" thread
 * - Static queue STDOUT_QUEUE_DEPTH x DEFAULT_MSG_SIZE
 * - Sender configurable at runtime
 *
 * Enabled by ENABLE_STDOUT in os_config.h.
 * If ENABLE_STDOUT=0 all functions become empty macros.
 ******************************************************************************
 */

#ifndef STDOUT_H
#define STDOUT_H

#include "common.h"
#include "utils.h"

#if ENABLE_STDOUT

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

/* Queue slot number: lives in hw/<family>/<mcu>/objmap.h, and NOWHERE ELSE */
#ifndef STDOUT_QUEUE_DEPTH
#error "STDOUT_QUEUE_DEPTH must be defined in hw/<family>/<mcu>/objmap.h"
#endif

/* ============================================================================
 * API
 * ========================================================================== */

/**
 * @brief Initializes the stdout system
 * @note Called by the application in ca_os_configure() (EVAL_STDOUT), before
 *       ca_stdout_set_sender() and ca_stdout_create_thread()
 */
void ca_stdout_init(void);

/**
 * @brief Sets the sender function for stdout
 * @param out Sender function (e.g. ca_itm_put, ca_uart_dbg_put, ca_usb_cdc_write)
 */
void ca_stdout_set_sender(sender out);

/**
 * @brief Reads the current sender function
 * @return Current sender (NULL if not set)
 */
sender ca_stdout_get_sender(void);

/**
 * @brief Sends formatted message to stdout (from task)
 * @param fmt Format string (printf style)
 * @return Number of characters
 */
u16 ca_stdout_printf(const char *fmt, ...);

/**
 * @brief Sends formatted message + \r\n to stdout (from task)
 * @param fmt Format string (printf style)
 * @return Number of characters
 */
u16 ca_stdout_printf_n(const char *fmt, ...);

/**
 * @brief Sends raw string to stdout (from task)
 * @param msg C string
 * @return Number of characters
 */
u16 ca_stdout_put(const char *msg);

/**
 * @brief Sends raw string + \r\n to stdout (from task)
 * @param msg C string
 * @return Number of characters
 */
u16 ca_stdout_put_n(const char *msg);

/**
 * @brief Sends p_str to stdout (from task)
 * @param str p_str
 * @return Number of characters
 */
u16 ca_stdout_put_str(p_str str);

/**
 * @brief Sends p_str + \r\n to stdout (from task)
 * @param str p_str
 * @return Number of characters
 */
u16 ca_stdout_put_str_n(p_str str);

/**
 * @brief Gets stdout statistics
 * @param queued  Messages in queue (can be NULL)
 * @param dropped Dropped messages   (can be NULL)
 */
void ca_stdout_stats(u32 *queued, u32 *dropped);

/**
 * @brief Flushes the stdout queue (discards pending messages). ISR-safe.
 * @details Useful when a USB CDC console opens: discards output queued
 *          while the terminal was closed, avoiding interlaced residues.
 */
void ca_stdout_flush(void);

/**
 * @brief Queues console echo bytes for the drain thread. ISR-safe.
 * @param buf Bytes to echo
 * @param len Number of bytes
 * @return true if queued (the drain writes them between two lines); false if
 *         the drain is not running yet or there is no sender - the caller then
 *         writes directly, as it did before
 * @details The drain thread is the ONLY writer of the console once it runs.
 *          An echo written straight from the RX interrupt could land in the
 *          middle of a line the drain was shifting out, byte by byte. Bytes
 *          that do not fit are dropped and counted: the command itself is not
 *          affected, it is already in the CLI buffer.
 */
bool ca_stdout_echo(const char *buf, u32 len);

/**
 * @brief Echo bytes dropped because the echo queue was full
 */
u32 ca_stdout_echo_dropped(void);

/**
 * @brief Installs a repeater called with every drained line, after the sender
 * @param hook Repeater, or NULL to remove it
 * @details The OS provides the mechanism, the application decides the policy:
 *          the hook is where a user writer forwards the line to syslog, flash,
 *          a file or anywhere else. It runs in the stdout drain thread - a
 *          normal task context, scheduler alive - so it may block, and the
 *          natural backpressure of the queue applies to it as it does to the
 *          sender. It is NEVER called from a fault handler: panic.c writes
 *          straight to the sender with PUT() and does not go through the
 *          queue, on purpose (a flash write from handler mode could stall on
 *          an ISR that will never be served).
 *          The string is owned by the drain thread and is only valid for the
 *          duration of the call: copy what must outlive it.
 */
void ca_stdout_set_repeater(sender hook);

/**
 * @brief Returns the installed repeater, or NULL
 */
sender ca_stdout_get_repeater(void);

/**
 * @brief Creates the stdout drain thread (called by the application in
 *        ca_os_configure(), after ca_stdout_init() and ca_stdout_set_sender())
 * @return Handle of the task (ca_task_handle_t), 0xFFFFFFFF if error
 */
u32 ca_stdout_create_thread(void);

#else /* ENABLE_STDOUT == 0 */

#define ca_stdout_init()
#define ca_stdout_set_sender(out)
#define ca_stdout_printf(...)       0
#define ca_stdout_printf_n(...)     0
#define ca_stdout_put(...)          0
#define ca_stdout_put_n(...)        0
#define ca_stdout_put_str(...)      0
#define ca_stdout_put_str_n(...)    0
#define ca_stdout_stats(q, d)
#define ca_stdout_set_repeater(hook)
#define ca_stdout_get_repeater()    NULL
#define ca_stdout_flush()
#define ca_stdout_echo(buf, len)    false
#define ca_stdout_echo_dropped()    0
#define ca_stdout_create_thread()   0xFFFFFFFF

#endif /* ENABLE_STDOUT */

#endif /* STDOUT_H */
