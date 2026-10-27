// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : systick.h
 * @brief          : System timer based on SysTick for scheduling
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * System timer based on the SysTick module integrated into the ARM Cortex-M4 core.
 * Generates periodic interrupts to provide a reliable time reference
 * for OS scheduling and timing functions.
 * Supports different tick frequencies configurable via ca_tick_speed_t.
 *
 * Typical sequence:
 * 1) SysTick initialization -> tick counting -> use for timing/scheduling
 ******************************************************************************
 */

#ifndef SYSTICK_H
#define SYSTICK_H

#include "common.h"

/* ============================================================================
 * RETURN CODES
 * ============================================================================
 */
#define SYSTICK_OK              0
#define SYSTICK_ERROR           -1

typedef enum __attribute__((packed)) tag_speed {
    CA_TICK_SPEED_100US = 100,
    CA_TICK_SPEED_200US = 200,
    CA_TICK_SPEED_250US = 250,
    CA_TICK_SPEED_400US = 400,
    CA_TICK_SPEED_500US = 500,
    CA_TICK_SPEED_1MS = 1000,
    CA_TICK_SPEED_2MS = 2000,
    CA_TICK_SPEED_4MS = 4000,
    CA_TICK_SPEED_5MS = 5000,
    CA_TICK_SPEED_10MS = 10000,
} ca_tick_speed_t;


/* ============================================================================
 * PUBLIC FUNCTIONS - INITIALIZATION
 * ============================================================================
 */

/**
 * @brief Initializes the SysTick timer for 1ms interrupts
 * @details Configures the SysTick to generate interrupts every 1ms using HCLK as source.
 *          With SYSCLK at 168 MHz:
 *          - Reload value = (168000000 / 1000) - 1 = 167999
 *          - Each tick = 1ms
 *
 *          Configured registers:
 *          - SYSTICK_LOAD: Reload Value Register
 *          - SYSTICK_VAL: Current Value Register (cleared to start immediately)
 *          - SYSTICK_CTRL: Control and Status Register (enable timer + interrupt)
 *
 * @param sysclk_hz SYSCLK frequency in Hz (must be 168000000 for this project)
 * @return SYSTICK_OK on success, SYSTICK_ERROR if parameters are invalid
 *
 * @note This function must be called AFTER SystemClock_Config()
 * @note The SysTick uses the SysTick_Handler() interrupt handler defined in systick.c
 */
int ca_os_tick_init(ca_tick_speed_t us);

/**
 * @brief Re-configures the SysTick after a CPU frequency change (does not reset ticks)
 * @return SYSTICK_OK on success
 */
int ca_os_tick_retune(void);

/**
 * @brief Restarts the SysTick period from now, keeping the elapsed fraction
 * @details For a switch out of the idle forced by an ISR: the incoming task
 *          gets a full quantum, and the fraction of tick already elapsed is
 *          accumulated so the tick count stays exact. IRQs must be masked.
 */
void ca_os_tick_restart_quantum(void);

/**
 * @brief Measures the cycles a quantum restart loses, against CYCCNT
 * @note Pre-scheduler, after the DWT cycle counter is running
 */
void ca_os_tick_calibrate(void);

/**
 * @brief Adds an interval the tick did not count (core in Stop), in us
 * @note IRQs masked. Only on the way back from Stop: cold boot and Standby
 *       go through reset.
 */
void ca_os_tick_compensate_us(u64 us);

/** @brief Cycles given back at each quantum restart (default or measured) */
u32 ca_os_tick_restart_gap(void);

/** @brief Tick-counted cycles minus CYCCNT cycles (diagnostic, see 'sched') */
i64 ca_os_tick_drift_cycles(void);

/**
 * @brief Suspends the SysTick and clears pending interrupts (STOP entry)
 */
void ca_os_tick_suspend(void);

/**
 * @brief Restarts the SysTick after wake from STOP
 */
void ca_os_tick_resume(void);

/**
 * @brief Disables the SysTick timer
 * @details Stops the timer and disables interrupts.
 *          The tick counter is preserved but not incremented.
 */
void ca_os_tick_disable(void);


/* ============================================================================
 * PUBLIC FUNCTIONS - TICK COUNTER
 * ============================================================================
 */

/**
 * @brief Gets the current value of the tick counter
 * @details The tick counter is incremented every 1ms by the SysTick interrupt.
 *          After 2^32-1 ticks (~49.7 days) the counter wraps around to 0.
 *
 * @return Number of ticks elapsed since initialization (in milliseconds)
 *
 * @note Thread-safe: u32 reading is atomic on Cortex-M4
 * @note Beware of wraparound: to measure time differences use
 *       subtraction (tick_end - tick_start) which correctly handles
 *       overflow thanks to modulo 2^32 arithmetic
 */
u32 ca_os_tick(void);
/* expressed with tick = 1ms */
u32 ca_os_tick_ms(void);

/**
 * @brief Gets the current 64-bit tick counter value
 * @details The 64-bit tick counter is incremented every 1ms by the SysTick interrupt.
 *          It practically never wraps around (585 million years @ 1ms tick).
 *
 * @return Number of ticks elapsed since initialization (in milliseconds)
 *
 * @note Thread-safe: u64 reading is NOT atomic, so interrupts
 *       are temporarily disabled during reading
 * @note Use this counter for long uptime measurements where the 32-bit
 *       counter wraparound (49 days) is problematic
 */
u64 ca_os_tick_64(void);
/* expressed with tick = 1ms */
u64 ca_os_tick_64_ms(void);

/* ============================================================================
 * PUBLIC FUNCTIONS - DELAY
 * ============================================================================
 */

/**
 * @brief Blocking delay in milliseconds
 * @details Waits for the specified number of milliseconds using the SysTick.
 *          This function is blocking: the CPU remains in a loop until the time expires.
 *
 * @param ms Number of milliseconds to wait (0 = immediate return)
 *
 * @note Correctly handles tick counter wraparound
 * @note Precision: ±1ms (depends on when it is called relative to the interrupt)
 * @note Does not use CPU busy-wait: calls __WFI() to put CPU in sleep
 *
 * @example
 * ca_os_delay_ms(500);  // Waits 500ms
 */
void ca_os_delay_ms(u32 ms);


/**
 * @brief Blocking delay in ticks
 * @details Waits for the specified number of ticks using the SysTick.
 *          This function is blocking: the CPU remains in a loop until the time expires.
 *
 * @param tick Number of ticks to wait (0 = immediate return)
 *
 * @note Correctly handles tick counter wraparound
 * @note Precision: ±1 tick (depends on when it is called relative to the interrupt)
 * @note Does not use CPU busy-wait: calls __WFI() to put CPU in sleep
 *
 * @example
 * ca_os_delay(50);  // Waits 50 ticks
 */
void ca_os_delay(u32 tick);

/**
 * @brief Checks if a certain time has elapsed since a reference tick
 * @details Checks if at least 'ms' milliseconds have passed since the 'start' tick.
 *          Correctly handles tick counter wraparound.
 *
 * @param start Initial reference tick (obtained with ca_os_tick())
 * @param ms Number of milliseconds to check
 * @return true if >= ms milliseconds have elapsed, false otherwise
 *
 * @note This function is non-blocking: returns immediately
 * @note Useful for implementing non-blocking timeouts and delays
 *
 * @example
 * u32 start = ca_os_tick();
 * while (!ca_os_timeout(start, 1000)) {
 *     // Do something for max 1 second
 *     if (condition_met) break;
 * }
 */
u16 ca_os_timeout(u32 start, u32 ms);


/* ============================================================================
 * PUBLIC FUNCTIONS - HARDWARE COUNTER
 * ============================================================================
 */

/**
 * @brief Gets the current value of the SysTick hardware counter (24-bit)
 * @details The SysTick hardware is a down-counter that starts from SYSTICK_LOAD and
 *          counts towards 0. When it reaches 0, it reloads and generates an interrupt.
 *          This value can be used for high-resolution timing.
 *
 * @return Current counter value (0 to SYSTICK_LOAD)
 *
 * @note Resolution: ~5.95ns @ 168MHz (1 HW tick = 1 CPU cycle)
 * @note The value counts BACKWARDS (from LOAD towards 0)
 * @note Useful for sub-millisecond time measurements
 */
u32 ca_os_tick_hw_counter(void);


/* ============================================================================
 * INLINE FUNCTIONS - TIME CONVERSIONS
 * ============================================================================
 */

/**
 * @brief Converts milliseconds to ticks (delegates to ca_os_tick_x_ms)
 */
#define ms_to_ticks(ms)        ca_os_tick_x_ms(ms)

/**
 * @brief Converts seconds to ticks
 */
#define seconds_to_ticks(s)    ca_os_tick_x_ms((s) * 1000)

/**
 * @brief returns the number of clock pulses per tick
 */
u32 ca_os_clocks_per_tick(void) ;

/**
 * @brief converts microseconds to 1ms scheduler ticks
 */
u32 ca_os_tick_x_ms(u32 ms);

/**
 * @brief Anti-drift periodic sleep (FreeRTOS vTaskDelayUntil style)
 * @details Calculates the next wake-up instant as (*last_wake_ms + period_ms)
 *          and sleeps only for the remaining time. Updates *last_wake_ms to the next
 *          expected instant (not the actual exit instant) eliminating cumulative
 *          drift in periodic loops.
 *
 * @param last_wake_ms  Pointer to the last wake-up timestamp (ms).
 *                      Initialize with ca_os_tick_ms() before the loop.
 *                      Automatically updated by the function.
 * @param period_ms     Desired period in milliseconds.
 *
 * @note If the loop body takes longer than period_ms, the function returns
 *       immediately without sleeping (no timer overflow).
 *
 * @example
 * u32 last = ca_os_tick_ms();
 * while (1) {
 *     ca_os_sleep_until(&last, 100);  // exact 100ms period, no drift
 *     do_periodic_work();
 * }
 */
void ca_os_sleep_until(u32 *last_wake_ms, u32 period_ms);

#endif /* SYSTICK_H */
