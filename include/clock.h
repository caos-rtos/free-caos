// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : clock.h
 * @brief          : System clock configuration
 * @author         : Angelo Coppi
 ******************************************************************************
 */

#ifndef CLOCK_H_
#define CLOCK_H_

#include "common.h"



/**
 * @brief Configure system clock to maximum frequency
 * @return 0 on success, -1 on failure
 */
int ca_os_clock_config(void);

/**
 * @brief Get current SYSCLK frequency in Hz
 */
u32 ca_os_clock_get_freq(void);

/**
 * @brief Reduce clock frequency
 * @param scale 1..100 %
 * @return current SYSCLK in Hz
 */
u32 ca_os_clock_reduce_freq(u16 scale);

/**
 * @brief Restore clock to maximum frequency
 * @return current SYSCLK in Hz
 */
u32 ca_os_clock_restore_freq(void);

/**
 * @brief Print all internal bus clocks
 */
void ca_os_clock_dump(void);

#endif /* CLOCK_H_ */
