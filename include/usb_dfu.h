// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    usb_dfu.h
 * @brief   USB DFU 1.1 service (runtime mode) — DFU interface without data EPs.
 *
 *  Scope (D11 of usb_stack_workplan.md): DFU *runtime*. The device
 *  exposes a DFU interface (class 0xFE, subclass 0x01) alongside other
 *  classes; the host (dfu-util) can send DFU_DETACH to request the
 *  switch to DFU mode. caOS notifies the application via on_detach: typical
 *  use = set a flag in no-init RAM and jump to the system bootloader.
 *  DFU mode, download only (cfg.download = true): interface protocol 0x02,
 *  the host writes a file with 'dfu-util -D'. The blocks arrive in the USB
 *  interrupt and are handed to a task: the application calls
 *  ca_usb_dfu_poll() in a loop, which runs on_dnload / on_manifest in task
 *  context (flash erase and write may take seconds); meanwhile GETSTATUS
 *  answers dfuDNBUSY. Manifestation tolerant: back to dfuIDLE afterwards.
 *  Upload is not supported.
 *
 *  Only EP0 (control). No data EPs → no entry in the EP collision bitmap.
 *
 *  Application usage:
 *      static const ca_usb_dfu_cfg_t s_dfu = {
 *          .header        = { CA_USB_TYPE_DFU, &ca_usb_dfu_ops },
 *          .detach_timeout_ms = 1000,
 *          .transfer_size = 1024,
 *          .on_detach     = my_jump_to_bootloader,
 *      };
 *      ca_usb_dev_mng_init(USB_NUM);
 *      ca_usb_dev_mng_add(&s_dfu.header);
 *      ca_usb_dev_mng_start();
 ******************************************************************************
 */

#ifndef USB_DFU_H
#define USB_DFU_H

#include "def_mcu.h"
#include "os_config.h"
#include "usb_mgmt.h"

#if EN_USB && EN_USB_DFU

/* USB_PID_BASE_DFU: os_config.h (usb group) */

/* DFU functional descriptor bmAttributes (DFU 1.1 §4.1.3) */
#define USB_DFU_ATTR_CAN_DNLOAD       0x01U
#define USB_DFU_ATTR_CAN_UPLOAD       0x02U
#define USB_DFU_ATTR_MANIFEST_TOL     0x04U
#define USB_DFU_ATTR_WILL_DETACH      0x08U

/* String index of the DFU iInterface (idx > 3, routed by mgmt to get_string).
   In composite with NCM (which uses 4) change one of the two. */
#ifndef USB_DFU_STR_IFACE
#define USB_DFU_STR_IFACE  4U
#endif

typedef struct {
    ca_usb_device_type_t header;        /* tag + ops (D19) */
    u16  detach_timeout_ms;             /* wDetachTimeOut in the functional desc */
    u16  transfer_size;                 /* wTransferSize in the functional desc */
    u8   attributes;                    /* bmAttributes (USB_DFU_ATTR_*); 0 → WILL_DETACH */
    const char *iface_str;              /* iInterface (e.g. "caOS DFU"); NULL → none */
    /* Host requested DFU_DETACH: the app must switch to DFU mode (e.g. flag
       no-init + reset to bootloader). NULL → DETACH ignored (ACK only). */
    void (*on_detach)(void);
    /* DFU mode (download). false = runtime only, the fields below unused. */
    bool download;
    /* A block of the file, offset from its start (task context, from
       ca_usb_dfu_poll). false = write error: the host gets errWRITE. */
    bool (*on_dnload)(u32 offset, const u8 *data, u16 len);
    /* The host sent the end of the file: check it (task context). false =
       the host gets errVERIFY. */
    bool (*on_manifest)(u32 total);
} __attribute__((aligned(4))) ca_usb_dfu_cfg_t;

/* DFU mode: largest block (wTransferSize), one EP0 packet at full speed */
#define USB_DFU_XFER_MAX   64U

extern const ca_usb_class_ops_t ca_usb_dfu_ops;

/* DFU mode: waits up to wait_ms (CA_WAIT_FOREVER allowed) for a block or the
 * end of the file from the host and runs on_dnload / on_manifest. Call it in
 * a loop from a task. Returns true when it ran one. */
bool ca_usb_dfu_poll(u32 wait_ms);

#endif /* EN_USB && EN_USB_DFU */
#endif /* USB_DFU_H */
