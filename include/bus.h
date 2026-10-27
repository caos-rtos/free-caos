// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : bus.h
 * @brief          : BusFault initialization per caOS
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * BusFault management system for the caOS operating system.
 * Initializes the BusFault exception as a separate exception instead of letting it
 * escalate to HardFault. This allows for better debugging and memory access
 * error management.
 *
 * Typical sequence:
 * 1) BusFault initialization -> handler registration -> error management
 ******************************************************************************
 */

#ifndef BUS_H_
#define BUS_H_

#include "common.h"

/**
 * @brief Initializes BusFault exception
 * @details Enables BusFault as a separate exception (no escalation to HardFault)
 *          Without this init, BusFault escalates to HardFault
 */
void ca_bus_error_init(void);

#endif /* BUS_H_ */
