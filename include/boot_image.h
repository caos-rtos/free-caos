// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    boot_image.h
 * @brief   Contract between the caOS bootloader (workspace/app_2) and the
 *          images it starts, plus the image side (EN_BOOT_IMAGE)
 ******************************************************************************
 * Guide: workspace/app_2/doc/app_2_user_guide.md. The bootloader and the image share no
 * memory: the only channel is two backup registers (bkp.h), which survive the
 * reset between them. The boot configuration in flash is written only by the
 * bootloader.
 *
 *   trial    an updated slot is "pending": the bootloader counts the tries in
 *            CA_BOOT_BKP_STATE, arms the IWDG and starts it
 *   confirm  the image, once healthy, marks CA_BOOT_BKP_STATE confirmed; at
 *            the next reset the bootloader makes the slot the active one
 *            (in the state register, not in MSG: an update request written
 *            after it must not erase it)
 *   rollback more than CA_BOOT_MAX_TRIES starts without a confirm: the
 *            pending slot is dropped, the active one starts again
 *   update   an image receives a new .cimg into the other slot and writes
 *            UPDATE: the bootloader checks it and makes it pending
 */
#ifndef BOOT_IMAGE_H
#define BOOT_IMAGE_H

#include "caos.h"

/* Slot map: H563 and H753, 2 MB in two 1 MB banks
 *   0x08000000  bootloader   CA_BOOT_LOADER_LEN   bank 1
 *   0x08080000  slot A       CA_BOOT_SLOT_LEN     bank 1, after the bootloader
 *   0x08100000  slot B       CA_BOOT_SLOT_LEN     bank 2
 * THE ONE PLACE OF THE MAP: the workspace CMake files (slot link address and
 * the bootloader size guard) and tools/caos_image.py read these three values
 * from this file, so they stay plain hex. To change the map, change them and
 * rebuild the library (boot_image.c uses them). Constraints: slot A must stay
 * inside bank 1 (A_BASE + SLOT_LEN <= 0x08100000), slot B inside bank 2
 * below the boot configuration sector, every boundary on a sector boundary
 * (8 KB H563, 128 KB H753); the bootloader write protection follows
 * CA_BOOT_LOADER_LEN (32 KB groups on H563, 128 KB sectors on H753). */
#if MCU_ID == 563 || MCU_ID == 753
#define CA_BOOT_SUPPORTED       1
#else
#define CA_BOOT_SUPPORTED       0
#endif
#define CA_BOOT_LOADER_LEN      0x00080000U  /* 512 KB */
#define CA_BOOT_SLOT_LEN        0x00080000U  /* 512 KB */
#define CA_BOOT_SLOT_B_BASE     0x08100000U
#define CA_BOOT_SLOT_A_BASE     (0x08000000U + CA_BOOT_LOADER_LEN)
#define CA_BOOT_HDR_SIZE        0x400U       /* the vector table follows the header */

/* Backup registers (indices present on every family but F1) */
#define CA_BOOT_BKP_STATE       16U          /* bootloader: slot on trial + tries */
#define CA_BOOT_BKP_MSG         17U          /* image -> bootloader: update request */
#define CA_BOOT_MAGIC           0xCAB70000U
#define CA_BOOT_MAGIC_MASK      0xFFFF0000U
#define CA_BOOT_MSG_UPDATE      'U'
#define CA_BOOT_CONFIRMED       0xFFU        /* tries field of a confirmed trial */
#define CA_BOOT_MSG(cmd, slot)      (CA_BOOT_MAGIC | ((u32)(u8)(cmd) << 8) | (u32)(u8)(slot))
#define CA_BOOT_STATE(slot, tries)  (CA_BOOT_MAGIC | ((u32)(u8)(slot) << 8) | ((u32)(tries) & 0xFFU))

#define CA_BOOT_MAX_TRIES       3U           /* starts of a pending slot before rollback */
#define CA_BOOT_IWDG_MS         8000U        /* IWDG armed by the bootloader for a trial */
#define CA_BOOT_TRIAL_MS        60000U       /* unconfirmed by then: the feeder stops */

#if EN_BOOT_IMAGE
/* Slot this image runs from ('A', 'B'), 0 when not started from a slot */
char ca_boot_image_slot(void);
/* Started by the bootloader as a trial (a pending slot, not yet confirmed) */
bool ca_boot_is_trial(void);
/* Call in ca_os_main before ca_os_run. On a trial: takes over the IWDG the
 * bootloader armed and feeds it from a job, for CA_BOOT_TRIAL_MS if never
 * confirmed, for ever after ca_boot_confirm(). Otherwise does nothing. */
void ca_boot_image_start(void);
/* The image is healthy: the bootloader keeps this slot at the next reset */
void ca_boot_confirm(void);
/* Ask the bootloader to check slot ('A'/'B') and start it as a trial: resets.
 * Refused on an unconfirmed trial: the other slot is the way back. */
bool ca_boot_request_update(char slot);
#if EN_FLASH && EN_TFTP_SERVICE && EN_NET
/* Receive one .cimg pushed over TFTP (tools/caos_tftp) into the OTHER slot,
 * then ca_boot_request_update(): on success it does not return */
bool ca_boot_receive_update(ca_mac_t local_mac, u32 wait_ms);
#endif
#endif /* EN_BOOT_IMAGE */

#endif /* BOOT_IMAGE_H */
