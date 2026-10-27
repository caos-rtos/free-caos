// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/**
 ******************************************************************************
 * @file           : config.h
 * @brief          : Global project configuration
 * @author         : Bare-metal STM32F767 Project
 ******************************************************************************
 * @attention
 *
 * This file contains all configurations and conditional compilation flags
 * to enable/disable project features.
 *
 * MUST be included by common.h as the first file.
 *
 ******************************************************************************
 */

#ifndef CONFIG_H_
#define CONFIG_H_


#include "def_mcu.h"

#include "stm32f767xx.h"

/* =============================================================================
 * OBJECT NUMBER CONFIGURATION
 * ============================================================================= */

#include "objmap.h"

/* =============================================================================
 * OS SPECIFIC CONFIGURATION
 * ============================================================================= */

#include "os_config.h"

#endif /* CONFIG_H_ */
