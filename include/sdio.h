// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    sdio.h
 * @brief   SDIO/SDMMC public driver — SDHC only, polling
 * @note    buf passed to read/write must be 4-byte aligned
 ******************************************************************************
 */

#ifndef SDIO_H
#define SDIO_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_SDIO

/* Longest a transfer may take before the driver gives up, per block (SD
 * Physical Layer spec): read access 100 ms; write busy 250 ms for SDHC and
 * 500 ms for SDXC - the larger one is used. A card that does not answer in
 * time is reported as a failed transfer, never waited on forever. */
#define CA_SDIO_READ_TIMEOUT_MS    100U
#define CA_SDIO_WRITE_TIMEOUT_MS   500U

bool ca_sdio_init(void);
bool ca_sdio_read_block(u32 block, u8 *buf);
bool ca_sdio_write_block(u32 block, const u8 *buf);
bool ca_sdio_write_blocks(u32 block, const u8 *buf, u32 count);
u32  ca_sdio_get_block_count(void);

#endif /* EN_SDIO */

#endif /* SDIO_H */
