// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/**
 ******************************************************************************
 * @file           : config.h
 * @brief          : Global configuration for the STM32H7S3 project (NUCLEO-H7S3L8)
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

#include "stm32h7s3xx.h"

/* =============================================================================
 * MCU SPECIFIC CONFIGURATION
 * ============================================================================= */

#include "objmap.h"

/* =============================================================================
 * OS SPECIFIC CONFIGURATION
 * ============================================================================= */

#include "os_config.h"

#endif /* CONFIG_H_ */
