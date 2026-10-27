// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    lptim.h
 * @brief   LPTIM driver — low-power timer on LSI (periodic, one-shot)
 *
 * @note    1-based numbering: num=1 → LPTIM1. Bitmask in_use: bit (num-1).
 *          Kernel clock LSI 32 kHz: times in MILLISECONDS (1..262144),
 *          automatic prescaler /1../128 for maximum resolution.
 *          Precision limited by LSI tolerance (±5%).
 *
 *          Modes:
 *          PERIODIC — callback on every ARR match (CNTSTRT, continuous)
 *          ONESHOT  — callback once (SNGSTRT), the timer stops by itself
 *
 *          The counter remains active in Stop (kernel clock LSI): with
 *          wake=true the instance wakes the MCU from Stop/Sleep of the power
 *          manager — ca_pwr_enter() re-enables NVIC (and EXTI on
 *          F7/H7, managed by the driver) by itself for wake-armed instances,
 *          and accepts entry into Stop even without other wake sources.
 *          LPTIM1 is shared with ca_pwr_enter() stopms: with an active
 *          instance, pwr rejects stopms (guard in ca_pwr_enter).
 *
 *          ca_lptim_stop() disables the counter (the HW has no pause:
 *          on restart CNT starts from 0). ca_lptim_dispatch(num) must be called
 *          from LPTIMx_IRQHandler (family file).
 *
 *          Families: F7/H7 = IP v1 (IER), H5/U5 = IP v2 (DIER/DIEROK).
 *          F1xx/F4xx: LPTIM not present → init returns false.
 ******************************************************************************
 */

#ifndef LPTIM_H_
#define LPTIM_H_

#include "common.h"
#include "os_config.h"

#if EN_LPTIM

typedef void (*ca_lptim_cb_t)(void);

typedef enum {
    CA_LPTIM_MODE_PERIODIC = 0,
    CA_LPTIM_MODE_ONESHOT,
} ca_lptim_mode_t;

/* --- Init for mode (ms 1..262144; wake = wake from Stop/Sleep pwr) --- */
bool ca_lptim_init_periodic(u8 num, u32 period_ms, ca_lptim_cb_t cb, bool wake);
bool ca_lptim_init_oneshot (u8 num, u32 delay_ms,  ca_lptim_cb_t cb, bool wake);

/* --- Control --- */
bool ca_lptim_start (u8 num);
bool ca_lptim_stop  (u8 num);
bool ca_lptim_deinit(u8 num);

/* --- Status and ISR dispatch --- */
bool ca_lptim_is_busy    (u8 num);
u32  ca_lptim_wake_active(void);  /* bitmask of instances in use with wake=true (for pwr) */
void ca_lptim_dispatch   (u8 num);   /* call from LPTIMx_IRQHandler */

/* --- Init (registers CLI if EN_CLI_LPTIM) --- */
void ca_lptim_init(void);

#endif /* EN_LPTIM */

#endif /* LPTIM_H_ */
