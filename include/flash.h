// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    flash.h
 * @brief   Internal FLASH driver — public API
 * @note    HW implementation: hw/<family>/drivers/flash_<family>.c
 *
 *  Minimal API for reading/writing/erasing internal flash.
 *
 *  ALIGNMENT CONSTRAINTS (depend on the family):
 *    F1xx:   addr 2-byte aligned, write unit 16 bit (2 bytes)
 *    F4xx:   addr 4-byte aligned, write unit 32 bit (4 bytes)
 *    F7xx:   addr 4-byte aligned, write unit 32 bit (4 bytes)
 *    H7xx:   addr 32-byte aligned, write unit 256 bit (32 bytes)
 *    H5xx:   addr 16-byte aligned, write unit 128 bit (16 bytes)
 *
 *  For len not a multiple of the unit, the last block is padded with 0xFF.
 *  Call ca_flash_erase_sector() before ca_flash_write() on the same area.
 ******************************************************************************
 */

#ifndef FLASH_H
#define FLASH_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_FLASH

/* Unlocks the FLASH controller for write/erase operations.
 * Returns false if unlock fails. */
bool ca_flash_unlock(void);

/* Locks the controller (protection against accidental writes). */
void ca_flash_lock(void);

/* Erases the sector/page containing addr.
 * Returns false in case of HW error. */
bool ca_flash_erase_sector(u32 addr);

/* Writes len bytes from buf to address addr.
 * addr must be aligned to the family's write unit.
 * Returns false in case of HW error. */
bool ca_flash_write(u32 addr, const u8 *buf, u32 len);

/* ==========================================================================
 * IRQ-driven API (uses EOPIE/ERRIE + semaphore instead of BSY polling)
 * Requires ca_flash_irq_init() once at boot.
 * ========================================================================== */

/* Initializes completion semaphore and enables NVIC FLASH_IRQn. */
void ca_flash_irq_init(void);

/* Erase of the sector containing addr — completion via IRQ. */
bool ca_flash_erase_sector_irq(u32 addr);

/* Write len bytes from buf to addr — completion via IRQ per unit. */
bool ca_flash_write_irq(u32 addr, const u8 *buf, u32 len);

/* SR error flag of the last IRQ operation (0 = no error). */
u32  ca_flash_get_error(void);

/* ==========================================================================
 * Sector info API
 * ========================================================================== */

/* Total number of internal flash sectors/pages. */
u16  ca_flash_get_num_sectors(void);

/* Start address of sector N (returns 0 if N >= num_sectors). */
u32  ca_flash_get_sector_addr(u8 sector);

/* Size in bytes of sector N (returns 0 if N >= num_sectors). */
u32  ca_flash_get_sector_size(u8 sector);

/* ==========================================================================
 * High-level erase
 * ========================================================================== */

/* Erase of sector number N (automatic unlock/lock). */
bool ca_flash_erase_sector_num(u8 sector);

/* Erase of all sectors overlapping [addr, addr+len). */
bool ca_flash_erase_range(u32 addr, u32 len);

/* ==========================================================================
 * Init
 * ========================================================================== */

/* Registers the CLI handler if EN_CLI_FLASH. Call once at boot. */
void ca_flash_init(void);

#endif /* EN_FLASH */
#endif /* FLASH_H */
