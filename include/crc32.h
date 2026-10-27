// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : crc32.h
 * @brief          : CRC32 module for thread context integrity verification
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * CRC32 calculation system to verify the integrity of thread contexts.
 * Uses the microcontroller's hardware CRC32 module to calculate fast and
 * reliable checksums of thread contexts, useful for detecting corruption
 * or accidental modifications to context data.
 *
 * Typical sequence:
 * 1) CRC32 module initialization -> context digest calculation -> integrity check
 ******************************************************************************
 */

#ifndef CRC32_H_
#define CRC32_H_


#include "def_mcu.h"
#include "os_config.h"

#if EN_CRC

#include "common.h"
#include "scheduler.h"


/**
 * @brief Initializes the hardware CRC32 module
 * @details Configures the microcontroller's hardware CRC32 module for
 *          calculating checksums useful for verifying the integrity
 *          of thread contexts.
 * @return void
 * @note To be called during system initialization
 */
extern void ca_crc32_init(void) ;

/**
 * @brief Calculates CRC32 on a buffer (mutex protected)
 * @param buff Pointer to the data buffer
 * @param size Size in bytes of the buffer
 * @return CRC32 value calculated via hardware
 */
extern u32 ca_crc32_get(u8 *buff, u32 size);

#endif /* EN_CRC */
#endif /* CRC32_H_ */
