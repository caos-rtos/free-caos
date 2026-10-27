// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    exti.h
 * @brief   EXTI driver public API
 * @note    HW implementation: hw/<family>/drivers/exti_<family>.c
 *
 *  Typical usage:
 *    1. ca_gpio_configure(port, pin, CA_GPIO_MODE_INPUT, ...) — pin as input
 *    2. ca_exti_init(port, pin, edge, callback) — configure EXTI
 *    3. The callback is invoked by the ISR on trigger
 *
 *  ISR names:
 *    F1/F4/F7/H7: EXTI0..4 individual, EXTI9_5 and EXTI15_10 shared
 *    H5xx: EXTI0..EXTI15 individual
 ******************************************************************************
 */

#ifndef EXTI_H
#define EXTI_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_EXTI

typedef enum {
    CA_EXTI_EDGE_RISING  = 0,
    CA_EXTI_EDGE_FALLING = 1,
    CA_EXTI_EDGE_BOTH    = 2,
} ca_exti_edge_t;

/* Callback invoked by the ISR — executed in interrupt context */
typedef void (*ca_exti_cb_t)(void);

/* Configures EXTI for the indicated pin (0..15).
 * port: CA_GPIO_PORT_A..CA_GPIO_PORT_K (as for ca_gpio_configure)
 * Automatically enables NVIC and IMR. */
bool ca_exti_init(u8 port, u8 pin, ca_exti_edge_t edge, ca_exti_cb_t cb);

/* Enable/disable interrupt for the pin (IMR bit) */
void ca_exti_enable(u8 pin);
void ca_exti_disable(u8 pin);


#endif /* EN_EXTI */
#endif /* EXTI_H */
