// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : led.h
 * @brief          : Onboard LED driver interface for CAOS
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Names the LEDs a board actually carries, so applications stop restating the
 * pin map. Before this driver every example that blinked carried its own
 * "#if MCU_ID" block, and a wrong pin there is invisible: the board simply
 * stays dark while the console keeps saying everything is fine.
 *
 * LEDs are numbered from 1 (1..ca_led_count()); 0 is never valid. The count
 * and the pins come from the board's objmap.h - see hw/common/led.c.
 ******************************************************************************
 */

#ifndef LED_H
#define LED_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_LED

/* =============================================================================
 * PUBLIC API
 * ============================================================================= */

/**
 * @brief Configure every LED declared by this board as a push-pull output.
 *
 * Leaves them all off. Call once at startup, before any other ca_led_* call.
 */
void ca_led_init(void);

/**
 * @brief Number of LEDs physically present on this board.
 *
 * @return Count of usable LEDs; valid numbers are 1..count. Boards with no
 *         LED at all report 0.
 */
u8 ca_led_count(void);

/**
 * @brief Turn a LED on, whatever polarity the board wires it with.
 *
 * @param led_num LED number, 1-based.
 * @return true if the LED exists on this board, false otherwise.
 */
bool ca_led_on(u8 led_num);

/**
 * @brief Turn a LED off.
 *
 * @param led_num LED number, 1-based.
 * @return true if the LED exists on this board, false otherwise.
 */
bool ca_led_off(u8 led_num);

/**
 * @brief Invert the state of a LED.
 *
 * @param led_num LED number, 1-based.
 * @return true if the LED exists on this board, false otherwise.
 */
bool ca_led_toggle(u8 led_num);

/**
 * @brief Read back what the driver last drove onto a LED.
 *
 * Reports the LOGICAL state (on/off), not the pin level: on an active-low
 * board the two differ.
 *
 * @param led_num LED number, 1-based.
 * @return true if the LED is on, false if off or if it does not exist.
 */
bool ca_led_get(u8 led_num);

#endif /* EN_LED */

#endif /* LED_H */
