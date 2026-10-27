// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    usb_ncm.h
 * @brief   USB CDC-NCM 1.0 — USB Ethernet (Network Control Model).
 *
 *  Step 9 of the usb_stack_workplan.md sequence. Implements NCM as
 *  ca_usb_class_ops_t over ca_usb_dev_mng:
 *    - IAD + comm-IF (CDC class 0x02, subclass NCM 0x0D) + data-IF
 *      (class 0x0A) with TWO alt settings: alt0=idle (no EP), alt1=active (bulk).
 *    - EP notif (interrupt IN) + EP data (bulk IN+OUT).
 *    - Wire: NTB (NCM Transfer Block) = NTH16 + NDP16 + datagrams.
 *      This step uses 1 datagram per NTB (bring-up; future aggregation).
 *    - Class SETUP: GET_NTB_PARAMETERS, SET/GET_NTB_INPUT_SIZE,
 *      SET_ETHERNET_PACKET_FILTER, GET/SET_NTB_FORMAT.
 *    - SET_INTERFACE alt0/alt1 opens/closes bulk EPs (via on_set_interface).
 *
 *  The host sees usb0 (Linux) / "USB Ethernet" (Windows). caOS does not impose a
 *  stack: Ethernet frames (complete L2, incl. 14B header) are passed
 *  to the app via callback; the app reinjects them with ca_usb_ncm_send_frame().
 *  Integration with caos/net (ca_net_iface_t push) is a subsequent step.
 *
 *  Application usage (L2 loopback with SA<->DA swap):
 *      static ca_usb_ncm_cfg_t s_ncm = {
 *          .header   = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
 *          .mps      = 64,
 *          .mac      = { 0x02,0xCA,0x05,0x00,0x00,0x01 },  // host-side MAC
 *          .on_rx    = my_rx_frame,   // Ethernet datagram received
 *      };
 *      // my_rx_frame: swaps the first 12 bytes (DA[6]<->SA[6]) and resends,
 *      // so the host receives DA=own_MAC, SA=device — consistent frame.
 *      ca_usb_dev_mng_init(USB_NUM);
 *      ca_usb_dev_mng_add(&s_ncm.header);
 *      ca_usb_dev_mng_start();
 ******************************************************************************
 */

#ifndef USB_NCM_H
#define USB_NCM_H

#include "def_mcu.h"
#include "os_config.h"
#include "usb_mgmt.h"

#if EN_USB && EN_USB_NCM

#ifndef USB_NCM_EP_NOTIF
#define USB_NCM_EP_NOTIF  2U
#endif
#ifndef USB_NCM_EP_DATA
#define USB_NCM_EP_DATA   3U
#endif
/* USB_PID_BASE_NCM: os_config.h (usb group) */

#define USB_NCM_FIFO_WORDS_NOTIF  8U
#define USB_NCM_FIFO_WORDS_DATA   64U

/* Supported L2 MTU (max Ethernet payload, header excluded not included here:
   1514 = 14 header + 1500). Datagram NTB limit for this step. */
#ifndef USB_NCM_MAX_FRAME
#define USB_NCM_MAX_FRAME  1514U
#endif

/* Callback: received a complete Ethernet datagram (L2, from DA to the
   payload). Called in task/ISR context by the service. eth/len are valid only
   during the call (copy if needed). */
typedef void (*ca_usb_ncm_rx_cb_t)(const u8 *eth, u16 len);

typedef struct {
    ca_usb_device_type_t header;    /* tag + ops (D19) — first field */
    u16  mps;                        /* Bulk MPS (64 FS); 0 = 64 */
    u8   mac[6];                     /* MAC exposed to the host (iMACAddress) */
    ca_usb_ncm_rx_cb_t on_rx;        /* received Ethernet datagram */
} __attribute__((aligned(4))) ca_usb_ncm_cfg_t;

extern const ca_usb_class_ops_t ca_usb_ncm_ops;

/* Sends an Ethernet frame (complete L2) to the host, encapsulated in an NTB
   with a single datagram. Returns false if link is not active (alt0) or error.
   eth: from DA (offset 0) for len bytes (max USB_NCM_MAX_FRAME). */
bool ca_usb_ncm_send_frame(const u8 *eth, u16 len);

/* true if the host has selected alt1 on the data interface (link active). */
bool ca_usb_ncm_is_up(void);

#endif /* EN_USB && EN_USB_NCM */
#endif /* USB_NCM_H */
