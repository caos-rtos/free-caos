// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    rtc.h
 * @brief   RTC driver — public types and APIs
 ******************************************************************************
 * @details
 * RTC driver for F4xx, F7xx, H7xx, H5xx (RTC v2/v3, HW calendar).
 * F1xx not supported (RTC v1 counter-based, use SysTick for timing).
 *
 * Alarm A and B with callback from IRQ.
 * Calibration in ppm (±487 ppm).
 ******************************************************************************
 */

#ifndef RTC_DRIVER_H
#define RTC_DRIVER_H

#include "os_config.h"

#if EN_RTC

/* ============================================================================
 * PUBLIC TYPES
 * ========================================================================== */

/** RTC clock source */
typedef enum {
    CA_RTC_CLK_AUTO = 0,  /* LSE if the crystal is found running, else LSI */
    CA_RTC_CLK_LSE = 1,   /* 32.768 kHz external crystal */
    CA_RTC_CLK_LSI = 2,   /* ~32 kHz internal RC (no crystal) */
    CA_RTC_CLK_HSE = 3,   /* HSE/32 */
} ca_rtc_clk_src_t;

/** Date/time */
typedef struct {
    u8  sec;    /* 0–59  */
    u8  min;    /* 0–59  */
    u8  hour;   /* 0–23  */
    u8  wday;   /* 1–7   (1=Monday) */
    u8  day;    /* 1–31  */
    u8  month;  /* 1–12  */
    u8  year;   /* 0–99  (relative to 2000) */
} ca_rtc_dt_t;

/** Alarm ID */
#define CA_RTC_ALARM_A  1u
#define CA_RTC_ALARM_B  2u

/** Alarm configuration */
typedef struct {
    u8   sec;         /* 0–59  */
    u8   min;         /* 0–59  */
    u8   hour;        /* 0–23  */
    u8   day;         /* 1–31  */
    bool match_sec;   /* true = compare seconds     */
    bool match_min;   /* true = compare minutes     */
    bool match_hour;  /* true = compare hours       */
    bool match_day;   /* true = compare day         */
} ca_rtc_alarm_t;

/** Alarm callback — called from IRQ */
typedef void (*ca_rtc_alarm_cb_t)(u8 alarm_id);

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief  Initializes RTC with the chosen clock source.
 * @param  src  CA_RTC_CLK_AUTO / CA_RTC_CLK_LSE / CA_RTC_CLK_LSI / CA_RTC_CLK_HSE
 * @details CA_RTC_CLK_AUTO keeps the source the kernel chose at boot
 *          (ca_rtc_clock_init): LSE if the crystal counts, else calibrated
 *          LSI. An explicit source re-initialises.
 * @retval true  OK
 * @retval false timeout or invalid parameter
 */
bool ca_rtc_init(ca_rtc_clk_src_t src);

/**
 * @brief  Clock source the RTC runs on (the one AUTO resolved to).
 */
ca_rtc_clk_src_t ca_rtc_get_clk_src(void);

/**
 * @brief  Boot-time clock choice, called by the kernel after the SysTick is
 *         set up and before ca_os_configure(): LSE if the crystal is found
 *         counting, else LSI with its frequency measured over 100 ms of
 *         SysTick (read by polling) and the prescaler set to it.
 *         After a reset the backup domain still holds the source (and the
 *         calibrated prescaler): nothing is waited for. Only a cold power-up
 *         waits for the LSE, at most RTC_LSE_WAIT_MS.
 * @retval false the RTC could not be started on either source
 */
bool ca_rtc_clock_init(void);

/**
 * @brief  LSI frequency measured at boot, 0 when the RTC is not on the LSI
 *         or the measurement could not run (nominal prescaler kept).
 */
u32 ca_rtc_lsi_hz(void);

/**
 * @brief  LSE startup time measured at a cold power-up (ms), -1 when the
 *         crystal was already running (reset) or was not started here.
 */
i32 ca_rtc_lse_start_ms(void);

/**
 * @brief  Sets date and time.
 */
bool ca_rtc_set(const ca_rtc_dt_t *dt);

/**
 * @brief  Reads current date and time.
 */
bool ca_rtc_get(ca_rtc_dt_t *dt);

/**
 * @brief  Milliseconds since 2000-01-01 00:00:00, with the sub-second part.
 * @details For measuring an interval across a low-power mode (Stop), where
 *          the OS tick does not run. Resolution 1/(PREDIV_S+1) s: ~4 ms on
 *          LSE/LSI. Valid 2000-2099.
 * @return false if the RTC cannot be read (not initialised, sync timeout)
 */
bool ca_rtc_get_stamp_ms(u64 *ms);

/**
 * @brief  Sets the RTC from a time taken from the network (NTP): unix seconds,
 *         UTC, epoch 1970-01-01.
 * @param  unix_time  seconds since 1970-01-01 00:00:00 UTC
 * @return false out of the RTC's range (2000-2099) or if the RTC cannot be set
 */
bool ca_rtc_from_net(u32 unix_time);

/**
 * @brief  Reads the RTC as a time for the network: unix seconds, UTC,
 *         epoch 1970-01-01 (the inverse of ca_rtc_from_net).
 * @return unix seconds, 0 if the RTC cannot be read or holds an invalid date
 */
u32 ca_rtc_to_net(void);

/**
 * @brief  Calibrates RTC in ppm.
 * @param  ppm  signed value: positive = speed up, negative = slow down.
 *              Range: approx –487 … +16 ppm (HW limit).
 */
bool ca_rtc_calibrate(i32 ppm);

/**
 * @brief  Configures an alarm with callback.
 * @param  alarm_id  CA_RTC_ALARM_A or CA_RTC_ALARM_B
 * @param  alarm     masks and comparison values
 * @param  cb        callback called from IRQ (can be NULL to disable)
 */
bool ca_rtc_set_alarm(u8 alarm_id, const ca_rtc_alarm_t *alarm, ca_rtc_alarm_cb_t cb);

/**
 * @brief  Disables an alarm and removes the callback.
 */
bool ca_rtc_clear_alarm(u8 alarm_id);


/**
 * @brief returns the date as an NTP timestamp: seconds since 1900-01-01 (NOT
 *        unix time - for unix seconds use ca_rtc_to_net)
 */
u32 ca_rtc_get_timestamp(void);

#endif /* EN_RTC */
#endif /* RTC_DRIVER_H */
