// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : timer.h
 * @brief          : Software Timer for caOS
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Software timer system for the caOS operating system.
 * Implements configurable software timers with callbacks that can
 * be one-shot (single execution) or periodic (repeated).
 * Manages a maximum of 16 timers simultaneously through
 * a dedicated daemon task.
 *
 * Typical sequence:
 * 1) Timer system initialization -> timer creation -> timer start -> expiration management
 ******************************************************************************
 */

#ifndef _SW_TIMER_H_
#define _SW_TIMER_H_

#include "common.h"

#if EN_SW_TIMER

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */

#ifndef CA_TIMER_MAX
#define CA_TIMER_MAX        16      /* Maximum number of timers */
#endif

#ifndef CA_TIMER_TASK_STACK
#define CA_TIMER_TASK_STACK 512     /* Timer daemon task stack */
#endif

#ifndef CA_TIMER_TASK_PRIO
#define CA_TIMER_TASK_PRIO  2       /* High priority for timer daemon */
#endif

/* ============================================================================
 * TYPES
 * ========================================================================== */

/** Timer callback - called when timer expires */
typedef void (*ca_timer_callback_t)(void *arg);

/** Timer mode */
typedef enum {
    CA_TIMER_ONE_SHOT = 0,  /* Single execution */
    CA_TIMER_PERIODIC = 1   /* Automatic repetition */
} ca_sw_timer_mode_t;

/** Timer state */
typedef enum {
    CA_TIMER_STOPPED = 0,
    CA_TIMER_RUNNING = 1,
    CA_TIMER_EXPIRED = 2
} ca_sw_timer_state_t;

/** Timer structure */
typedef struct {
    u32 period_ms;              /* Period in ms */
    u32 remaining_ms;           /* Remaining time */
    ca_timer_callback_t cb;     /* Callback */
    void *cb_arg;               /* Callback argument */
    ca_sw_timer_mode_t mode;           /* One-shot or periodic */
    ca_sw_timer_state_t state;         /* Current state */
    u8 active;                  /* 1 = slot used, 0 = free */
} ca_sw_timer_t;

/** Timer handle (index + 1, 0 = invalid) */
typedef u8 ca_sw_timer_handle_t;

#define CA_TIMER_INVALID    0

/* ============================================================================
 * API
 * ========================================================================== */

/**
 * @brief Initializes the timer system
 * @return true if ok, false if error
 * @note Creates the timer daemon task
 */
bool ca_sw_timer_init(void);

/**
 * @brief Creates a new timer
 * @param period_ms Period in milliseconds
 * @param mode CA_TIMER_ONE_SHOT or CA_TIMER_PERIODIC
 * @param callback Function to call upon expiration
 * @param arg Argument passed to the callback
 * @return Timer handle (CA_TIMER_INVALID if error)
 */
ca_sw_timer_handle_t ca_sw_timer_create(u32 period_ms, ca_sw_timer_mode_t mode,
                               ca_timer_callback_t callback, void *arg);

/**
 * @brief Starts a timer
 * @param handle Timer handle
 * @return true if ok
 */
bool ca_sw_timer_start(ca_sw_timer_handle_t handle);

/**
 * @brief Stops a timer
 * @param handle Timer handle
 * @return true if ok
 */
bool ca_sw_timer_stop(ca_sw_timer_handle_t handle);

/**
 * @brief Resets timer (restarts counting)
 * @param handle Timer handle
 * @return true if ok
 */
bool ca_sw_timer_reset(ca_sw_timer_handle_t handle);

/**
 * @brief Modifies timer period
 * @param handle Timer handle
 * @param period_ms New period
 * @return true if ok
 */
bool ca_sw_timer_set_period(ca_sw_timer_handle_t handle, u32 period_ms);

/**
 * @brief Deletes a timer
 * @param handle Timer handle
 * @return true if ok
 */
bool ca_sw_timer_delete(ca_sw_timer_handle_t handle);

/**
 * @brief Gets remaining time
 * @param handle Timer handle
 * @return Remaining time in ms, 0 if expired or error
 */
u32 ca_sw_timer_get_remaining(ca_sw_timer_handle_t handle);

/**
 * @brief Gets timer state
 * @param handle Timer handle
 * @return Timer state
 */
ca_sw_timer_state_t ca_sw_timer_get_state(ca_sw_timer_handle_t handle);

/**
 * @brief Counts active timers
 * @return Number of running timers
 */
u8 ca_sw_timer_count_active(void);

#endif

#endif /* _SW_TIMER_H_ */
