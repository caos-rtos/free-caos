// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    usb_msc.h
 * @brief   USB Mass Storage Class (BOT / SCSI transparent) — block device.
 *
 *  Step 8 of the usb_stack_workplan.md sequence. Implements MSC as
 *  ca_usb_class_ops_t over ca_usb_dev_mng:
 *    - 1 interface (class 0x08, subclass SCSI 0x06, protocol BOT 0x50)
 *    - 1 EP bulk IN + 1 EP bulk OUT (same EP number, D17)
 *    - Bulk-Only Transport: CBW (31B) → data → CSW (13B)
 *    - SCSI subset: INQUIRY, TEST_UNIT_READY, READ_CAPACITY10, READ10,
 *      WRITE10, REQUEST_SENSE, MODE_SENSE6, PREVENT_ALLOW_MEDIUM_REMOVAL
 *
 *  The storage back-end is provided by the app via callback (block device):
 *  the host (PC) provides the filesystem (mkfs.fat). caOS has no file management,
 *  it only exposes 512B blocks.
 *
 *  Application usage (ramdisk on heap):
 *      static ca_usb_msc_cfg_t s_msc = {
 *          .header     = { CA_USB_TYPE_MSC, &ca_usb_msc_ops },
 *          .mps        = 64,
 *          .block_size = 512,
 *          .block_count= 256,            // 128 KB
 *          .read       = my_read_block,
 *          .write      = my_write_block,
 *      };
 *      ca_usb_dev_mng_init(USB_NUM);
 *      ca_usb_dev_mng_add(&s_msc.header);
 *      ca_usb_dev_mng_start();
 ******************************************************************************
 */

#ifndef USB_MSC_H
#define USB_MSC_H

#include "def_mcu.h"
#include "os_config.h"
#include "usb_mgmt.h"

#if EN_USB && EN_USB_MSC

#ifndef USB_MSC_EP_DATA
#define USB_MSC_EP_DATA   4U
#endif
/* USB_PID_BASE_MSC: os_config.h (usb group) */

#define USB_MSC_FIFO_WORDS_DATA  64U

/* Block device back-end. read/write transfer `count` blocks of
   `block_size` bytes starting from `lba`. Return true if ok.
   Called in task/ISR context by the service: must be non-blocking
   and fast (for ramdisk: memcpy). */
typedef struct {
    ca_usb_device_type_t header;        /* tag + ops (D19) — first field */
    u16  mps;                            /* Bulk MPS (64 FS, 512 HS); 0 = 64 */
    u32  block_size;                     /* typically 512 */
    u32  block_count;                    /* total number of blocks (capacity) */
    bool (*read) (u32 lba, u8 *buf, u32 count);
    bool (*write)(u32 lba, const u8 *buf, u32 count);
} __attribute__((aligned(4))) ca_usb_msc_cfg_t;

extern const ca_usb_class_ops_t ca_usb_msc_ops;

#endif /* EN_USB && EN_USB_MSC */
#endif /* USB_MSC_H */
