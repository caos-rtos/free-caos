// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : common.h
 * @brief          : Common header for the entire caOS project
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Common header that must be included by all files in the caOS project.
 * Contains fundamental definitions such as:
 * - Standard data types (i8, u8, i16, u16, etc.)
 * - Macros for interrupt control (CPSID I, CPSIE I)
 * - Utility macros (ARRAY_SIZE, UNUSED, MIN, MAX)
 * - Includes of the project-specific configuration
 *
 * Typical sequence:
 * 1) Inclusion in every .c and .h file -> access to common definitions
 ******************************************************************************
 */

#ifndef COMMON_H_
#define COMMON_H_

/* =============================================================================
 * PROJECT CONFIGURATION (must be the first include)
 * ============================================================================= */
#include "config.h"
#include "endian.h"



/* =============================================================================
 * MACROS FOR INTERRUPT CONTROL (ARM CORTEX-M4)
 * ============================================================================= */

/**
 * @brief  Disable global interrupts (PRIMASK = 1)
 * @note   Uses assembly instruction CPSID I
 */
#if defined(__GNUC__)
#define __disable_irq() __asm volatile ("cpsid i" : : : "memory")
#else
static inline void __disable_irq(void) {
    __asm volatile ("cpsid i" : : : "memory");
}
#endif

/**
 * @brief  Enable global interrupts (PRIMASK = 0)
 * @note   Uses assembly instruction CPSIE I
 */
#if defined(__GNUC__)
#define __enable_irq()  __asm volatile ("cpsie i" : : : "memory")
#else
static inline void __enable_irq(void) {
    __asm volatile ("cpsie i" : : : "memory");
}
#endif


/* =============================================================================
* COMMON UTILITY MACROS
* ============================================================================= */

/**
 * @brief  Returns the number of elements in a static array
 */
#define ARRAY_SIZE(arr)  (sizeof(arr) / sizeof((arr)[0]))

/**
 * @brief  Marks a variable as unused (avoids compiler warnings)
 */
#define UNUSED(x)  ((void)(x))

/**
 * @brief  Min/Max macros
 */
#ifndef MIN
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#endif

/**
 * @brief  Timeout values shared by every blocking call (sem, mutex, queue,
 *         notify, event, stream, message, condvar): 0 never waits,
 *         CA_WAIT_FOREVER waits with no deadline, anything else is ms.
 */
#define CA_NO_WAIT_TIME   0U
#define CA_WAIT_FOREVER   0xFFFFFFFFU


#endif /* COMMON_H_ */
