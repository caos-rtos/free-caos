// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    bkp.h
 * @brief   Backup registers: a few words that survive a system reset
 ******************************************************************************
 * They are in the backup domain: a system reset (software, watchdog, pin) and
 * standby keep them; a power loss keeps them only with VBAT supplied (on the
 * Nucleo boards VBAT is tied to VDD, so a power cycle clears them).
 * F1: 16 bits per register (the upper half reads 0).
 * No EN_ flag: a handful of bytes, needed by the bootloader handover.
 */
#ifndef BKP_H
#define BKP_H

#include "caos.h"

u32  ca_bkp_count(void);                 /* registers available on this part */
u32  ca_bkp_read(u32 idx);               /* 0 when idx is out of range */
bool ca_bkp_write(u32 idx, u32 value);   /* false when idx is out of range */

/* Backup SRAM: 4 KB (F4, F7, H7, H563), 2 KB (U575), none on F1. Same backup
 * domain as the registers, so the same survival rules; Standby and VBAT keep
 * it only after ca_bkp_sram_retain(). Opened (clock, write access) on the
 * first call. U5/H5 have ECC on it when the BKPRAM_ECC option is on: after a
 * power-up write a region before reading it. */
u32  ca_bkp_sram_size(void);                                  /* bytes, 0 = none */
bool ca_bkp_sram_read(u32 off, void *dst, u32 len);           /* false when out of range */
bool ca_bkp_sram_write(u32 off, const void *src, u32 len);
bool ca_bkp_sram_retain(void);                                /* backup regulator on: kept in Standby/VBAT */

#endif /* BKP_H */
