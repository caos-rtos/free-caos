// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    timer.h
 * @brief   Timer driver — periodic, one-shot, counter, encoder
 *
 * @note    1-based numbering: num=1 → TIM1, num=2 → TIM2, ...
 *          Bitmask in_use: bit (num-1) = TIM(num) occupied.
 *
 *          Modes:
 *          PERIODIC — periodic callback via TIMx_IRQHandler
 *          ONESHOT  — callback once, timer stops automatically
 *          COUNTER  — counts external pulses; incremental read with
 *                     ca_timer_counter_read() → accumulates in u64 *count
 *          ENCODER  — quadrature encoder; read with
 *                     ca_timer_encoder_read() → accumulates in i64 *pos
 *          PWM      — PWM output on channel 1-4 (same frequency for timer,
 *                     independent duty per channel, permille 0-1000).
 *                     Pins are the caller's responsibility: ca_gpio_configure(
 *                     CA_GPIO_MODE_AF, ..., af) before init. Then
 *                     ca_timer_start(num); duty runtime glitch-free (preload).
 *
 *          ca_timer_dispatch(num) must be called from TIMx_IRQHandler
 *          (family file) for PERIODIC and ONESHOT.
 *          COUNTER and ENCODER do not use update ISR.
 ******************************************************************************
 */

#ifndef TIMER_H_
#define TIMER_H_

#include "common.h"
#include "os_config.h"

#if EN_TIMER

typedef void (*ca_timer_cb_t)(void);

typedef enum {
    CA_TIMER_MODE_PERIODIC = 0,
    CA_TIMER_MODE_ONESHOT,
    TIMER_MODE_COUNTER,
    CA_TIMER_MODE_ENCODER,
    TIMER_MODE_COUNTER_IRQ,  /* down counter on ETR clock, IRQ every pulse preset */
    CA_TIMER_MODE_PWM,          /* PWM output, up to 4 channels per timer */
    CA_TIMER_MODE_CAPTURE,      /* input capture: ticks between rising edges */
} ca_timer_mode_t;

/* Input capture source. LSE/LSI are internal routes meant for calibration and
 * exist on one timer channel per family: U5 TIM16/17 CH1, H5/H7 TIM16 CH1,
 * F4/F7 TIM5 CH4 (init fails elsewhere). With CA_TIMER_IC_PIN the channel pin
 * must already be in its timer alternate function. TIM1/TIM8 not supported
 * (their capture interrupt is a separate vector). */
typedef enum {
    CA_TIMER_IC_PIN = 0,
    CA_TIMER_IC_LSE,
    CA_TIMER_IC_LSI
} ca_timer_ic_src_t;

/* Called from the timer interrupt with the ticks (ca_timer_clock_hz) between
 * two captures; with div N one capture every N rising edges. */
typedef void (*ca_timer_ic_cb_t)(u32 ticks);

/* --- Init for mode --- */
bool ca_timer_init_periodic    (u8 num, u32 period_us, ca_timer_cb_t cb);
bool ca_timer_init_oneshot     (u8 num, u32 delay_us,  ca_timer_cb_t cb);
bool ca_timer_init_counter     (u8 num, u64 *count);
bool ca_timer_init_encoder     (u8 num, i64 *pos);
bool ca_timer_init_counter_irq (u8 num, u32 preset,    ca_timer_cb_t cb); /* down, ECE, UIE every pulse preset */
bool ca_timer_init_pwm         (u8 num, u8 ch, u32 freq_hz); /* ch 1-4; can be called multiple times (channels) same freq */
bool ca_timer_init_capture     (u8 num, u8 ch, ca_timer_ic_src_t src, u8 div, ca_timer_ic_cb_t cb); /* div 1/2/4/8 */
u32  ca_timer_clock_hz         (u8 num);                     /* counting rate, ticks per second */

/* --- PWM runtime --- */
bool ca_timer_set_pwm          (u8 num, u8 ch, u16 duty_pm); /* duty 0-1000 permille, glitch-free */

/* --- Control --- */
bool ca_timer_start (u8 num);
bool ca_timer_stop  (u8 num);
bool ca_timer_deinit(u8 num);

/* --- Incremental read (counter / encoder) --- */
void ca_timer_counter_read(u8 num);   /* updates *count += delta */
void ca_timer_encoder_read(u8 num);   /* updates *pos   += delta signed */

/* --- Status and ISR dispatch --- */
bool ca_timer_is_busy  (u8 num);
void ca_timer_dispatch (u8 num);      /* call from TIMx_IRQHandler */
void ca_timer_capture_event(u8 num, u32 ccr, u32 arr); /* hw/: one capture, arr = counter wrap mask */

/* --- Init (registers CLI if EN_CLI_TIMER) --- */
void ca_timer_init(void);

#endif /* EN_TIMER */

#endif /* TIMER_H_ */
