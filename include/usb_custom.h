// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    usb_custom.h
 * @brief   USB custom service — interface vendor 0xFF + 1 EP bulk IN+OUT loopback
 *
 *  Minimal service (step 6 of the usb_stack_workplan.md sequence) to exercise
 *  init / add / start / dispatch of ca_usb_dev_mng without standard protocols.
 *
 *  Application usage:
 *      static const ca_usb_custom_cfg_t s_custom = {
 *          .header   = { CA_USB_TYPE_CUSTOM, &ca_usb_custom_ops },
 *          .ep_data  = 1,             // EP1 IN + EP1 OUT
 *          .mps      = 64,
 *          .on_rx    = my_rx_cb,
 *      };
 *      ca_usb_dev_mng_init(USB_NUM);
 *      ca_usb_dev_mng_add(&s_custom.header);
 *      ca_usb_dev_mng_start();
 ******************************************************************************
 */

#ifndef USB_CUSTOM_H
#define USB_CUSTOM_H

#include "def_mcu.h"
#include "os_config.h"
#include "usb_mgmt.h"

#if EN_USB && EN_USB_CUSTOM

/* USB_PID_BASE_CUSTOM: os_config.h (usb group) */

typedef struct {
    ca_usb_device_type_t header;       /* tag + ops (D19) */
    u8   ep_data;                       /* data EP number (1..N), IN+OUT bulk */
    u16  mps;                           /* bulk MPS (64 FS, 512 HS) */
    void (*on_rx)     (u8 ep_addr, u16 len);
    void (*on_tx_done)(u8 ep_addr);
    void (*on_mounted)(void);           /* opt.: host has enumerated the device */
    void (*on_unmounted)(void);         /* opt.: cable disconnected / host suspended */
} __attribute__((aligned(4))) ca_usb_custom_cfg_t;

extern const ca_usb_class_ops_t ca_usb_custom_ops;

#endif /* EN_USB && EN_USB_CUSTOM */
#endif /* USB_CUSTOM_H */
