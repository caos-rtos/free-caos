// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : pwr.h
 * @brief          : Power manager driver interface for CAOS
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Centralized power manager driver: the power state of all peripherals
 * is read directly from the RCC registers (clock gating) and the PWR block
 * (voltage scaling, wakeup flags). No hooks in peripheral drivers.
 ******************************************************************************
 */

#ifndef PWR_H
#define PWR_H

#include "caos.h"

#if EN_PWR

/* =============================================================================
 * TYPES
 * ============================================================================= */

typedef enum {
    CA_PWR_MODE_SLEEP = 0,    /*!< Simple WFI, clocks active; wake from any IRQ */
    CA_PWR_MODE_STOP,         /*!< deepsleep, LP regulator; RAM/peripherals stopped, wake → resume */
    PWR_MODE_STANDBY,      /*!< RAM lost; wake → reset (see standby_resume in ca_pwr_info_t) */
} ca_pwr_mode_t;

/* wakeup sources for ca_pwr_enter */
#define CA_PWR_WAKE_RTC   (1U << 0)   /*!< RTC alarm after `seconds` (requires ca_rtc_init) */
#define CA_PWR_WAKE_PIN   (1U << 1)   /*!< WKUP1 pin (PA0 on most families) */
#define CA_PWR_WAKE_LPTIM (1U << 2)   /*!< LPTIM1 one-shot on LSI after `seconds` (U5xx only; max 262 s) */
#define CA_PWR_WAKE_LPUART (1U << 3)  /*!< internal use: LPUART wake-armed (set by ca_pwr_enter, do not pass it) */

typedef struct {
    const char   *name;    /*!< peripheral name, e.g. "USART1" */
    const char   *bus;     /*!< RCC bus, e.g. "APB2" */
    volatile u32 *enr;     /*!< RCC xxxENR register */
    u32           mask;    /*!< clock enable bit */
} ca_pwr_periph_t;

typedef struct {
    u32  sysclk_hz;        /*!< current SYSCLK */
    u32  sysclk_max_hz;    /*!< nominal board SYSCLK */
    u8   vos;              /*!< current voltage scaling; 0xFF = not available (F1) */
    bool standby_resume;   /*!< true if boot comes from Standby */
    u16  periph_on;        /*!< peripherals with active RCC clock */
    u16  periph_total;     /*!< peripherals registered for the family */
} ca_pwr_info_t;

/* =============================================================================
 * PUBLIC API
 * ============================================================================= */

/**
 * @brief Initializes the power manager (PWR clock, wakeup flags cache, CLI)
 * @return 0 if OK
 */
int ca_pwr_init(void);

/**
 * @brief Snapshot of the overall power state
 */
void ca_pwr_get_info(ca_pwr_info_t *info);

/**
 * @brief RCC peripheral table of the current family
 * @param count number of entries
 */
const ca_pwr_periph_t *ca_pwr_periph_table(u32 *count);

/**
 * @brief Reduces CPU frequency and recalibrates the time base (SysTick + UART baud)
 * @param scale 1..99 %
 * @return current SYSCLK in Hz
 */
u32 ca_pwr_set_freq(u16 scale);

/**
 * @brief Restores nominal CPU frequency and recalibrates the time base
 * @return current SYSCLK in Hz
 */
u32 ca_pwr_restore_freq(void);

/**
 * @brief Enters a low-power mode
 * @param mode       CA_PWR_MODE_STOP (returns on wake) or PWR_MODE_STANDBY (reset on wake)
 * @param wake_flags CA_PWR_WAKE_RTC | CA_PWR_WAKE_PIN | CA_PWR_WAKE_LPTIM (at least one mandatory)
 * @param duration   wake timer delay, > 0: SECONDS with CA_PWR_WAKE_RTC,
 *                   MILLISECONDS with CA_PWR_WAKE_LPTIM (resolution up to 31 µs, max 262144 ms)
 * @return 0 on wake from STOP; -1 on error (STANDBY does not return if successful)
 * @note  On wake from STOP, the PLL is reinitialized and SysTick/UART/CAN are recalibrated.
 *        OS time (tick) remains stopped during STOP: ongoing sleeps will drift.
 *        With IWDG active, the timeout must exceed the stop duration (LSI active in stop).
 */
int ca_pwr_enter(ca_pwr_mode_t mode, u32 wake_flags, u32 duration);

#endif /* EN_PWR */
#endif /* PWR_H */
