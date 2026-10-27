// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    usb.h
 * @brief   USB device driver — bare-metal low-level API
 * @note    HW implementation: hw/<family>/drivers/usb_<family>.c
 *
 *  Typical usage:
 *    1. ca_usb_init(usb_num, &cb)           — init HW + enable IRQ
 *    2. Respond to on_reset/on_setup callbacks from the management task
 *    3. ca_usb_ep_open() after SET_CONFIGURATION
 *    4. ca_usb_ep_write() / ca_usb_ep_read() for bulk/intr transfers
 *
 *  usb_num:
 *    H7xx  → 0 = OTG_HS (USB1, ULPI), 1 = OTG_FS (USB2, PA11/PA12)
 *    F4/F7 → 0 = OTG_FS (PA11/PA12), 1 = OTG_HS (ULPI if available)
 *    F1/H5/U5 → 0 = USB_DRD_FS
 *
 *  ep_addr: 0x00–0x07 OUT, 0x80–0x87 IN (bit 7 = IN direction)
 ******************************************************************************
 */

#ifndef USB_H
#define USB_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_USB

/* ── Enumerations ─────────────────────────────────────────────────────────── */

typedef enum {
    CA_USB_EP_TYPE_CTRL = 0,
    CA_USB_EP_TYPE_ISOC = 1,
    CA_USB_EP_TYPE_BULK = 2,
    CA_USB_EP_TYPE_INTR = 3,
} ca_usb_ep_type_t;

typedef enum {
    CA_USB_SPEED_FS = 0,   /* Full Speed 12 Mbit/s  */
    CA_USB_SPEED_HS = 1,   /* High Speed 480 Mbit/s */
} ca_usb_speed_t;

/* ── Callbacks (invoked by ISR) ──────────────────────────────────────────── */

typedef void (*ca_usb_reset_cb_t)(void);
typedef void (*ca_usb_mounted_cb_t)(ca_usb_speed_t speed);
typedef void (*ca_usb_unmounted_cb_t)(void);
typedef void (*ca_usb_setup_cb_t)(const u8 setup[8]);
typedef void (*ca_usb_rx_cb_t)(u8 ep_addr, u16 len);
typedef void (*ca_usb_tx_done_cb_t)(u8 ep_addr);

typedef struct {
    ca_usb_reset_cb_t     on_reset;
    ca_usb_mounted_cb_t   on_mounted;
    ca_usb_unmounted_cb_t on_unmounted;
    ca_usb_setup_cb_t     on_setup;
    ca_usb_rx_cb_t        on_rx;
    ca_usb_tx_done_cb_t   on_tx_done;
} ca_usb_cb_t;

/* ── Driver Ops-table (ISR excluded — drivers invoke the ISR directly) ── */
typedef struct {
    void           (*clock_init)    (u32 usb_num);
    void           (*gpio_init)     (u32 usb_num);
    bool           (*init)          (u32 usb_num, const ca_usb_cb_t *cb);
    bool           (*ep_open)       (u32 usb_num, u8 ep_addr, ca_usb_ep_type_t type, u16 mps);
    bool           (*ep_write)      (u32 usb_num, u8 ep_addr, const u8 *buf, u16 len);
    u16            (*ep_read)       (u32 usb_num, u8 ep_addr, u8 *buf, u16 max_len);
    void           (*set_address)   (u32 usb_num, u8 addr);
    ca_usb_speed_t (*get_speed)     (u32 usb_num);
    void           (*connect)       (u32 usb_num);
    void           (*disconnect)    (u32 usb_num);
    void           (*ep_stall)      (u32 usb_num, u8 ep_addr);
    bool           (*ep_close)      (u32 usb_num, u8 ep_addr);
    bool           (*ep_clear_stall)(u32 usb_num, u8 ep_addr);
    bool           (*ep_is_stalled) (u32 usb_num, u8 ep_addr);
} ca_usb_dev_ops_t;

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Init HW (clock, GPIO, DWC2/FSDEV core) and register callbacks. */
bool ca_usb_init(ca_usb_speed_t speed, u32 usb_num, const ca_usb_cb_t *cb);

/* Opens an endpoint after SET_CONFIGURATION.
 * ep_addr: 0x00–0x0F OUT, 0x80–0x8F IN */
bool ca_usb_ep_open(u32 usb_num, u8 ep_addr, ca_usb_ep_type_t type, u16 mps);

/* Starts IN transmission — returns false if endpoint is busy */
bool ca_usb_ep_write(u32 usb_num, u8 ep_addr, const u8 *buf, u16 len);

/* Reads data received on OUT endpoint (call after on_rx callback) */
u16  ca_usb_ep_read(u32 usb_num, u8 ep_addr, u8 *buf, u16 max_len);

/* Sets device address (response to SET_ADDRESS, call in task) */
void ca_usb_set_address(u32 usb_num, u8 addr);

/* Reads negotiated speed after ENUMDNE */
ca_usb_speed_t ca_usb_get_speed(u32 usb_num);

/* ISR dispatcher — called by the HW IRQ handler */
void ca_usb_isr(u32 usb_num);

/* Soft connect / disconnect (toggle DCTL.SDIS) */
void ca_usb_connect(u32 usb_num);
void ca_usb_disconnect(u32 usb_num);

/* STALL endpoint — response to unsupported SETUP (e.g., Device Qualifier on FS-only) */
void ca_usb_ep_stall(u32 usb_num, u8 ep_addr);

/* "Hot" closing of an EP (SET_INTERFACE with alt change).
 * Executes the DWC2 HW sequence (see note A.1 in doc/usb_stack_workplan.md):
 *   IN : SNAK → EPDIS → flush FIFO TX → clear USBAEP → reset driver state
 *   OUT: global NAK → EPDIS → clear USBAEP → reset driver state
 * EP0 cannot be closed. Returns false if ep_addr is invalid or EP is not open. */
bool ca_usb_ep_close(u32 usb_num, u8 ep_addr);

/* CLEAR_FEATURE(ENDPOINT_HALT): removes stall, resets DATA0/1 toggle
 * (SD0PID_SEVNFRM), re-arms DMA OUT or frees tx_busy IN. */
bool ca_usb_ep_clear_stall(u32 usb_num, u8 ep_addr);

/* GET_STATUS(ENDPOINT): true if the EP is currently in STALL state. */
bool ca_usb_ep_is_stalled(u32 usb_num, u8 ep_addr);

/* Driver diagnostics (ISR session event counters + per-EP), only with EN_USB_DBG.
   Implemented on U5 (575) and H7 (753/747) and F4 (407). No-op on other families. */
#if EN_USB_DBG
typedef struct {
    /* event counters (all families) */
    u32 rst;
    u32 enumdne;
    u32 susp;
    u32 wkup;
    u32 setup;
    u32 rxflvl;
    u32 rx_push;
    u32 rx_drop_full;
    u32 oep_xfrc[USB_EP_MAX];
    u32 iep_xfrc[USB_EP_MAX];
    u32 ep_read [USB_EP_MAX];
    u32 ep_write[USB_EP_MAX];
    u32 wr_full [USB_EP_MAX];
    /* EP state snapshot (H5xx USB_DRD_FS; zero on other families) */
    u32 chep    [USB_EP_MAX];   /* Raw R_CHEP: STAT_RX[13:12], STAT_TX[5:4], DTOG, VTRX/VTTX */
    u8  rxq_h   [USB_EP_MAX];   /* ring RX head (ISR) / tail (ep_read) */
    u8  rxq_t   [USB_EP_MAX];
    u8  txq_h   [USB_EP_MAX];   /* ring TX head (ep_write) / tail (ISR) */
    u8  txq_t   [USB_EP_MAX];
    u8  tx_act  [USB_EP_MAX];   /* an IN packet is armed in PMA */
    u32 rx_drop [USB_EP_MAX];   /* OUT ACKed by HW but RX ring full -> lost */
    u32 rx_cnt0 [USB_EP_MAX];   /* OUT received by ISR with cnt==0 */
    u32 rx_len0 [USB_EP_MAX];   /* rxq entry with len==0 consumed by ep_read */
    /* DWT cycle counter measurements (H7xx — zero on other families) */
    u32 isr_rx_sum;
    u32 isr_rx_max;
    u32 isr_rx_cnt;
    u32 iat_sum;
    u32 iat_max;
    u32 iat_min;
    u32 iat_cnt;
    u32 lat_sum[USB_EP_MAX];
    u32 lat_max[USB_EP_MAX];
    u32 lat_cnt[USB_EP_MAX];
} ca_usb_dbg_t;
/* Implemented by HW drivers — do not call directly from the application. */
#if EN_USB_FS
void ca_usb_dbg_fs_stat(ca_usb_dbg_t *out, u32 usb_num);
void ca_usb_dbg_fs_reset(void);
#endif
/* Public API (usb_mgmt.c): dispatch to the correct driver based on usb_num. */
void ca_usb_dbg_stat(ca_usb_dbg_t *out, u32 usb_num);
void ca_usb_dbg_reset(void);
/* Prints diagnostic fields for usb_num to stdout. */
void ca_usb_dbg_print(u32 usb_num);
#else
#define ca_usb_dbg_stat(out, n)  do { (void)(out); (void)(n); } while (0)
#define ca_usb_dbg_reset()       do {} while (0)
#define ca_usb_dbg_print(n)      do { (void)(n); } while (0)
#endif

#else

#define ca_usb_init(...)            (false)
#define ca_usb_ep_open(...)         (false)
#define ca_usb_ep_write(...)        (false)
#define ca_usb_ep_read(...)         (0)
#define ca_usb_set_address(...)
#define ca_usb_get_speed(...)       (CA_USB_SPEED_FS)
#define ca_usb_isr(...)
#define ca_usb_connect(...)
#define ca_usb_disconnect(...)
#define ca_usb_ep_stall(...)
#define ca_usb_ep_close(...)        (false)
#define ca_usb_ep_clear_stall(...)  (false)
#define ca_usb_ep_is_stalled(...)   (false)

#endif /* EN_USB */
#endif /* USB_H */
