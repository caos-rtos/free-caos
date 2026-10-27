// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com


/**
 ******************************************************************************
 * @file           : gpio_driver.h
 * @brief          : GPIO driver interface for CAOS
 * @author         : Angelo Coppi
 ******************************************************************************
 */

#ifndef GPIO_DRIVER_H
#define GPIO_DRIVER_H

#include "caos.h"

/* =============================================================================
 * GPIO PIN DEFINITIONS
 * ============================================================================= */

/**
 * @brief GPIO port enumeration
 */
typedef enum {
    CA_GPIO_PORT_A = 0,
    CA_GPIO_PORT_B,
    CA_GPIO_PORT_C,
    CA_GPIO_PORT_D,
    CA_GPIO_PORT_E,
    CA_GPIO_PORT_F,
    CA_GPIO_PORT_G,
    CA_GPIO_PORT_H,
    CA_GPIO_PORT_I,
    CA_GPIO_PORT_J,
    CA_GPIO_PORT_K,
} ca_gpio_port_t;

/**
 * @brief GPIO pin enumeration
 */
typedef enum {
    CA_GPIO_PIN_0 = 0,
    CA_GPIO_PIN_1,
    CA_GPIO_PIN_2,
    CA_GPIO_PIN_3,
    CA_GPIO_PIN_4,
    CA_GPIO_PIN_5,
    CA_GPIO_PIN_6,
    CA_GPIO_PIN_7,
    CA_GPIO_PIN_8,
    CA_GPIO_PIN_9,
    CA_GPIO_PIN_10,
    CA_GPIO_PIN_11,
    CA_GPIO_PIN_12,
    CA_GPIO_PIN_13,
    CA_GPIO_PIN_14,
    CA_GPIO_PIN_15,
} ca_gpio_pin_t;

/**
 * @brief GPIO mode enumeration
 */
typedef enum {
    CA_GPIO_MODE_INPUT = 0,
    CA_GPIO_MODE_OUTPUT,
    CA_GPIO_MODE_AF,
    CA_GPIO_MODE_ANALOG,
} ca_gpio_mode_t;

/**
 * @brief GPIO output type enumeration
 */
typedef enum {
    CA_GPIO_OTYPE_PP = 0,  /*!< Push-pull */
    CA_GPIO_OTYPE_OD,      /*!< Open-drain */
} ca_gpio_otype_t;

/**
 * @brief GPIO speed enumeration
 */
typedef enum {
    CA_GPIO_SPEED_LOW = 0,
    CA_GPIO_SPEED_MED,
    CA_GPIO_SPEED_HIGH,
    CA_GPIO_SPEED_VERY_HIGH,
} ca_gpio_speed_t;

/**
 * @brief GPIO pull-up/pull-down enumeration
 */
typedef enum {
    CA_GPIO_NOPULL = 0,
    CA_GPIO_PULLUP,
    CA_GPIO_PULLDOWN,
} ca_gpio_pull_t;

/* =============================================================================
 * GPIO DRIVER PROTOTYPES
 * ============================================================================= */

/**
 * @brief Configure a GPIO pin
 * @param port GPIO port (A-K)
 * @param pin GPIO pin (0-15)
 * @param mode GPIO mode (input, output, alternate function, analog)
 * @param otype Output type (push-pull or open-drain)
 * @param speed Output speed
 * @param pull Pull-up/pull-down configuration
 * @param af Alternate function number (0-15, used when mode is CA_GPIO_MODE_AF)
 */
bool ca_gpio_configure(ca_gpio_port_t port, ca_gpio_pin_t pin, ca_gpio_mode_t mode, 
                      ca_gpio_otype_t otype, ca_gpio_speed_t speed, ca_gpio_pull_t pull, u8 af);

/**
 * @brief Set GPIO clock for a specific port    
 * @param port GPIO port
 */
void ca_gpio_init_port(ca_gpio_port_t port);



/**
 * @brief Set GPIO pin output level
 * @param port GPIO port
 * @param pin GPIO pin
 * @param state Output state (0 = low, 1 = high)
 */
void ca_gpio_write(ca_gpio_port_t port, ca_gpio_pin_t pin, u8 state);

/**
 * @brief Read GPIO pin input level
 * @param port GPIO port
 * @param pin GPIO pin
 * @return Input state (0 = low, 1 = high)
 */
u8 ca_gpio_read(ca_gpio_port_t port, ca_gpio_pin_t pin);

/**
 * @brief Toggle GPIO pin output level
 * @param port GPIO port
 * @param pin GPIO pin
 */
void ca_gpio_toggle(ca_gpio_port_t port, ca_gpio_pin_t pin);


#endif /* GPIO_DRIVER_H */