// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    usb_cdc.h
 * @brief   USB CDC-ACM service — virtual serial port (/dev/ttyACMx).
 *
 *  Step 7 of the usb_stack_workplan.md sequence. Implements the CDC ACM
 *  class as ca_usb_class_ops_t over ca_usb_dev_mng:
 *    - 1 comm interface (CDC control, class 0x02) + 1 data interface
 *      (CDC data, class 0x0A), grouped by a IAD (D17)
 *    - EP_NOTIF (interrupt IN) for line status notifications
 *    - EP_DATA (bulk IN+OUT) for serial data
 *    - class SETUP: GET/SET_LINE_CODING, SET_CONTROL_LINE_STATE
 *
 *  Application usage:
 *      static const ca_usb_cdc_cfg_t s_cdc = {
 *          .header  = { CA_USB_TYPE_CDC, &ca_usb_cdc_ops },
 *          .on_rx   = my_rx_cb,         // data received from host
 *          .on_line = my_line_cb,       // optional: line_coding/DTR change
 *      };
 *      ca_usb_dev_mng_init(USB_NUM);
 *      ca_usb_dev_mng_add(&s_cdc.header);
 *      ca_usb_dev_mng_start();
 *      ...
 *      ca_usb_cdc_write(p_str message);      // send to host
 ******************************************************************************
 */

#ifndef USB_CDC_H
#define USB_CDC_H

#include "def_mcu.h"
#include "os_config.h"
#include "usb_mgmt.h"

#if EN_USB && EN_USB_CDC

#ifndef USB_CDC_EP_NOTIF
#define USB_CDC_EP_NOTIF  2U
#endif
#ifndef USB_CDC_EP_DATA
#define USB_CDC_EP_DATA   3U
#endif
/* USB_PID_BASE_CDC: os_config.h (usb group) */

#define USB_CDC_FIFO_WORDS_NOTIF  8U
#define USB_CDC_FIFO_WORDS_DATA   64U

/* Serial line status (CDC SET_LINE_CODING, 7 bytes on wire). */
typedef struct {
    u32 baud;        /* dwDTERate */
    u8  stop_bits;   /* 0=1, 1=1.5, 2=2 */
    u8  parity;      /* 0=none,1=odd,2=even,3=mark,4=space */
    u8  data_bits;   /* 5,6,7,8,16 */
} ca_usb_cdc_line_t;

typedef struct {
    ca_usb_device_type_t header;        /* tag + ops (D19) — first field */
    u16  mps;                            /* Data bulk MPS (64 FS, 512 HS); 0 = 64 */
    void (*on_rx)  (const u8 *buf, u16 len);          /* data from host */
    void (*on_line)(const ca_usb_cdc_line_t *line, u8 control);  /* opt. */
} __attribute__((aligned(4))) ca_usb_cdc_cfg_t;

extern const ca_usb_class_ops_t ca_usb_cdc_ops;

/* ── Application API ─────────────────────────────────────────────────────── */

/* Sends len bytes to the host on the bulk IN EP. Returns false if the driver's
   TX queue is full (retry after ca_os_yield). */
bool ca_usb_cdc_write(p_str message);

/* true when the host has asserted DTR (terminal opened). */
bool ca_usb_cdc_connected(void);
/* Waits for CDC connection until timeout_ms; returns true if connected. */
bool ca_usb_cdc_wait_connected(u32 timeout_ms);

/* Current line coding (set by host via SET_LINE_CODING). */
const ca_usb_cdc_line_t *ca_usb_cdc_get_line(void);

/* "All-in-one" CDC console: creates a task that performs init+add+start of the USB
   mgmt with an internal CDC cfg that acts as a system command-line. on_rx forwards
   every byte to ca_cli_rx_byte() (like the UART): the CLI handles line accumulation,
   echo, backspace, VT100 and sending to STDIN on CR/LF. on_line prints the terminal
   status (connected/closed).
   Replaces the old ca_usb_cdc_init()/ca_usb_cdc_create_task() pair.
   Returns true if the task was created. */
bool ca_usb_dev_mng_cdc_cli(void);

#endif /* EN_USB && EN_USB_CDC */
#endif /* USB_CDC_H */
