// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : safeguard.h
 * @brief          : SAFEGUARD via SVC for caOS
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Intelligent SAFEGUARD system using SVC interrupt.
 * Unlike standard assert, the system handles the fault with recovery.
 *
 * Behavior:
 * - Scheduler active + not in IRQ: log + remove task + context switch
 * - Scheduler not active or in IRQ: log + while(1) + WFI
 *
 * Usage:
 *   CA_SAFEGUARD(ptr != NULL);              // Condition only
 *   CA_SAFEGUARD(idx < MAX, "overflow!");   // Condition + message
 ******************************************************************************
 */

#ifndef SAFEGUARD_H
#define SAFEGUARD_H

#include "common.h"

/* ============================================================================
 * SVC NUMBER
 * ========================================================================== */

#define SVC_SAFEGUARD  0xFE    /* SVC number for SAFEGUARD */
#define SVC_OS_END     0xFD    /* SVC number for ca_os_end() */

/* ============================================================================
 * SAFEGUARD MACRO (supports optional message)
 * ========================================================================== */

/* Helper to count arguments */
#define _SAFEGUARD_NARG(...) _SAFEGUARD_NARG_(__VA_ARGS__, _SAFEGUARD_RSEQ())
#define _SAFEGUARD_NARG_(...) _SAFEGUARD_ARG_N(__VA_ARGS__)
#define _SAFEGUARD_ARG_N(_1, _2, N, ...) N
#define _SAFEGUARD_RSEQ() 2, 1, 0

/* Dispatch based on number of arguments */
#define _SAFEGUARD_DISPATCH(N) _SAFEGUARD_##N
#define _SAFEGUARD_DISPATCH_(N) _SAFEGUARD_DISPATCH(N)

/* Version with 1 argument (condition only) */
#define _SAFEGUARD_1(cond) \
    do { \
        if (!(cond)) { \
            ca_safeguard_failed(#cond, NULL, __FILE__, __LINE__); \
        } \
    } while(0)

/* Version with 2 arguments (condition + message) */
#define _SAFEGUARD_2(cond, msg) \
    do { \
        if (!(cond)) { \
            ca_safeguard_failed(#cond, msg, __FILE__, __LINE__); \
        } \
    } while(0)

/**
 * @brief SAFEGUARD with task recovery (if possible)
 * @param cond Condition to verify
 * @param msg (optional) Custom error message
 * @details If cond is false:
 *          - Log: condition, message, file, line, task
 *          - If scheduler active and not in IRQ: remove task + continue
 *          - Otherwise: halt (WFI loop)
 */
#define CA_SAFEGUARD(...) _SAFEGUARD_DISPATCH_(_SAFEGUARD_NARG(__VA_ARGS__))(__VA_ARGS__)

/* CA_SAFE_FAIL: always-fail safeguard with message */
#define CA_SAFE_FAIL(msg) CA_SAFEGUARD(0, msg)


/* ============================================================================
 * INTERNAL API (do not call directly)
 * ========================================================================== */

/**
 * @brief Function called when SAFEGUARD fails
 * @param cond_str String of the failed condition
 * @param msg Optional message (can be NULL)
 * @param file Source file name
 * @param line Line number
 * @note This function does NOT return if scheduler is not active or in IRQ
 */
void ca_safeguard_failed(const char  *cond_str, const char  *msg, const char  *file, u32 line);

/**
 * @brief Reset safeguard flag (called after task restart)
 * @note Call from SVC handler after restart to allow new safeguards
 */
void ca_safeguard_reset(void);

#endif /* SAFEGUARD_H */
