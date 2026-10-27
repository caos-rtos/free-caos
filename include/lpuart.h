// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    lpuart.h
 * @brief   LPUART driver — low-power console on LPUART1 (kernel clock HSI16)
 *
 * @note    Single instance: LPUART1. Kernel clock HSI16 with HSIKERON:
 *          the baudrate is independent of sysclk (no retune after
 *          ca_pwr_set_freq / wake from Stop) and reception remains functional
 *          in Stop. TX polling, RX byte-per-byte interrupt (FIFO off).
 *
 *          With wake=true the instance is a wake-up source for the power
 *          manager: receiving a character wakes the MCU from
 *          Sleep/Stop (UESM + RXNEIE; the byte is NOT lost — kernel clock
 *          alive, the ISR delivers it to on_rx after wake).
 *          ca_pwr_enter(CA_PWR_MODE_STOP, 0, 0) accepts entry into Stop
 *          if ca_lpuart_wake_active() is true.
 *
 *          ca_lpuart_put is sender-compatible: usable as STDOUT
 *          (ca_stdout_set_sender / ca_cli_set_sender). For the console:
 *          on_rx = ca_cli_rx_byte wrapper.
 *
 *          Families: U5xx (autonomous SMEN/AMEN), H5xx (LPEN), H7xx
 *          (LPEN + D3AMR + EXTI 34 managed by the driver).
 *          F1xx/F4xx/F7xx: LPUART not present → init returns false.
 *
 *          Pins per board (AF in family file, check with datasheet):
 *          U575 PA2/PA3 AF8 — H563 PB6/PB7 AF8 — H753 PA9/PA10 AF3
 ******************************************************************************
 */

#ifndef LPUART_H_
#define LPUART_H_

#include "common.h"
#include "os_config.h"

#if EN_LPUART

typedef void (*ca_lpuart_rx_cb_t)(u8 byte);

/* alternate_gpio: pick the secondary TX/RX pin mapping where the family has one
 * (H7: false=PA9/PA10 AF3, true=PB6/PB7 AF8). Ignored where only one set exists.
 * Default 0; a per-example os_config may override (e.g. ex62 on the H753 Nucleo). */
#ifndef LPUART_STD_ALT_GPIO
#define LPUART_STD_ALT_GPIO 0
#endif

/* --- Init / deinit --- */
bool ca_lpuart_configure_gpio(bool alternate_gpio);   /* TX/RX pins in AF (also called by init) */
bool ca_lpuart_init  (u32 baud, ca_lpuart_rx_cb_t on_rx, bool wake, bool alternate_gpio);
bool ca_lpuart_deinit(void);

/* --- System console (EVAL_LPUART_STD, called by ca_os_init) ---
 * init at LPUART_STD_BAUD with on_rx → ca_cli_rx_byte, wake = LPUART_STD_WAKE */
#if ENABLE_CLI
bool ca_lpuart_std_cli(bool alternate_gpio);
#endif

/* --- TX (sender-compatible for STDOUT/CLI) --- */
u16  ca_lpuart_put(p_str message);

/* --- Status and ISR dispatch --- */
bool ca_lpuart_is_busy    (void);
u32  ca_lpuart_wake_active(void);  /* 1 if init with wake=true (for pwr) */
void ca_lpuart_dispatch   (void);  /* call from LPUART1_IRQHandler */

#endif /* EN_LPUART */

#endif /* LPUART_H_ */
