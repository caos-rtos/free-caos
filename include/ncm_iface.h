// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    ncm_iface.h
 * @brief   Adapter USB-NCM -> ca_net_iface_t (push, copy-based)
 *
 * @note    Wraps the ca_usb_ncm class service into a ca_net_iface_t with
 *          ops.recv = NULL (PUSH model): the NCM service delivers Ethernet
 *          datagrams via callback, the adapter copies them into a net_buf and calls
 *          ca_net_rx_push(). The TX copies the net_buf into an NTB via
 *          ca_usb_ncm_send_frame() (NOT zero-copy: the buf is freed here).
 *
 *          Usage (USB Ethernet + caOS IP stack):
 *            static ca_usb_ncm_cfg_t s_ncm = {
 *                .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
 *                .mps = 64, .mac = {...},
 *                // .on_rx set by the adapter
 *            };
 *            ca_usb_dev_mng_init(USB_NUM);
 *            ca_usb_dev_mng_add(&s_ncm.header);
 *            ca_usb_dev_mng_start();
 *            ca_net_init(ca_ncm_iface_get(&s_ncm));   // registers as push iface
 *            ca_net_proto_init(&net_cfg);             // ARP/ICMP/UDP
 *
 *          The NCM cfg must remain valid for the entire lifetime of the stack.
 ******************************************************************************
 */

#ifndef NCM_IFACE_H_
#define NCM_IFACE_H_

#include "common.h"
#include "os_config.h"

#if EN_NET && EN_USB && EN_USB_NCM

#include "usb_ncm.h"
#include "net_iface.h"

/* Returns the iface (static singleton) over the NCM service. Sets
 * cfg->on_rx to the internal handler (push towards the net core). */
ca_net_iface_t *ca_ncm_iface_get(ca_usb_ncm_cfg_t *cfg);

#endif /* EN_NET && EN_USB && EN_USB_NCM */
#endif /* NCM_IFACE_H_ */
