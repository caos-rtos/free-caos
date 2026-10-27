// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    usb_mgmt.h
 * @brief   USB device management — builds composite config descriptors
 *          on top of the low-level driver (ca_usb_*) and dispatches standard/class
 *          SETUP requests to class services (ca_usb_cdc, ca_usb_msc, ca_usb_ncm, ...).
 *
 *  Architecture: D9 of the plan (doc/usb_stack_workplan.md). Lives in caos/usb/
 *  in symmetry with caos/net/ and caos/crypto/.
 *
 *  2-level model:
 *    1) compile-time:  EN_USB_CDC, EN_USB_MSC, ... → which classes are
 *                      *available* in the binary
 *    2) runtime:       ca_usb_dev_mng_add(&cfg) → which class is *active* now
 *
 *  Typical usage:
 *      ca_usb_dev_mng_init(USB_NUM);
 *      ca_usb_dev_mng_add(&my_cdc.header);     // my_cdc is static const
 *      ca_usb_dev_mng_add(&my_msc.header);
 *      ca_usb_dev_mng_start();
 *
 *  Lifetime (D20): the management keeps the pointer to the cfg without copying.
 *  The cfg struct must be `static` (ideally `static const`).
 ******************************************************************************
 */

#ifndef USB_MGMT_H
#define USB_MGMT_H

#include "def_mcu.h"
#include "os_config.h"
#include "usb.h"

#if EN_USB

/* ── Type tag (D19): first field of every ca_usb_<class>_cfg_t ─────────────── */

typedef enum {
    CA_USB_TYPE_NONE  = 0,
    CA_USB_TYPE_CDC   = 1,
    CA_USB_TYPE_MSC   = 2,
    CA_USB_TYPE_NCM   = 3,
    CA_USB_TYPE_DFU   = 4,
    CA_USB_TYPE_CUSTOM = 5,   /* custom vendor service (step 6) */
    CA_USB_TYPE__MAX
} ca_usb_type_t;

/* ── Class operations table (class-specific callbacks) ──────────────
 * Each class service (cdc/msc/ncm/custom) defines a singleton
 * `const ca_usb_class_ops_t` with its handlers. The class cfg points
 * to that table; the mgmt invokes methods via ops without switching on type.  */
typedef struct ca_usb_device_type_s ca_usb_device_type_t;

typedef struct ca_usb_class_ops_s {
    /* Appends interface(+IAD)+endpoint descriptors to the buffer; returns appended bytes.
       *iface_num_inout is incremented by the used iface count. */
    u16  (*build_iface_desc)(const ca_usb_device_type_t *cfg,
                              u8 *buf, u16 max, u8 *iface_num_inout);
    /* Composite PID contribution (D14): PID_BASE_x + USB_x_EP_DATA, or 0. */
    u16  (*pid_contribution)(const ca_usb_device_type_t *cfg);
    /* Driver notifications (NULL-safe, fan-out to all active services). */
    void (*on_reset)    (const ca_usb_device_type_t *cfg);
    void (*on_mounted)  (const ca_usb_device_type_t *cfg, ca_usb_speed_t speed);
    void (*on_unmounted)(const ca_usb_device_type_t *cfg);
    /* Opens/closes service EPs (SET_CONFIGURATION). */
    void (*on_configure)(const ca_usb_device_type_t *cfg);
    /* SET_INTERFACE: the mgmt invokes it on EVERY active service passing the requested
       iface and alt. The service compares iface with its own range and, if it
       is its responsibility, opens/closes the bulk EPs (NCM alt0=idle, alt1=active).
       Returns true if iface is its responsibility (stops iteration). */
    bool (*on_set_interface)(const ca_usb_device_type_t *cfg, u8 iface, u8 alt);
    /* Class SETUP (bmRequestType.Type=1). Returns true if handled, false → STALL. */
    bool (*on_setup)    (const ca_usb_device_type_t *cfg, const u8 setup[8]);
    /* Fan-out EP I/O: the service filters ep_addr (can be ep_owns); returns
       true if the EP is its own (stops iteration). */
    bool (*on_rx)       (const ca_usb_device_type_t *cfg, u8 ep_addr, u16 len);
    bool (*on_tx_done)  (const ca_usb_device_type_t *cfg, u8 ep_addr);
    /* String descriptor for index > 3 (iMACAddress NCM, iInterface, ...). Writes
       a complete USB string descriptor (bLength, bDescriptorType=0x03,
       UTF-16LE) into buf and returns the length; 0 if the index is not its own. */
    u16  (*get_string)  (const ca_usb_device_type_t *cfg, u8 idx, u8 *buf, u16 max);
} ca_usb_class_ops_t;

struct ca_usb_device_type_s {
    ca_usb_type_t              type;     /* MUST be the first field (D19) */
    const ca_usb_class_ops_t  *ops;      /* class callback table */
} __attribute__((aligned(4)));

/* ── Runtime EP allocation (D16) ─────────────────────────────────────────────
 * Classes are *available* at compile-time (EN_USB_X) but *active* only if
 * added with ca_usb_dev_mng_add(). Enabling all classes in the binary does NOT
 * imply using them together: EP collision must therefore be detected at runtime,
 * in ca_usb_dev_mng_add(), by comparing the class EP mask with those already
 * occupied (s_ep_used in usb_mgmt.c). If the EP is occupied, add() returns false.
 *
 * The USB_EPMASK_* macros provide the EP bitmap of fixed-EP classes (CDC/MSC/NCM);
 * CUSTOM uses an EP chosen at runtime (cfg->ep_data) → mask calculated in add().
 * EP0 (control) excluded. */
#define USB_EPBIT(n)  (1U << (n))

#define USB_EPMASK_CDC  (USB_EPBIT(USB_CDC_EP_NOTIF) | USB_EPBIT(USB_CDC_EP_DATA))
#define USB_EPMASK_MSC  (USB_EPBIT(USB_MSC_EP_DATA))
#define USB_EPMASK_NCM  (USB_EPBIT(USB_NCM_EP_NOTIF) | USB_EPBIT(USB_NCM_EP_DATA))

/* ── Device identity: os_config.h, usb group ─────────────────────────────────
 * USB_VID, USB_BCD_DEVICE, USB_STR_MANUFACTURER, USB_STR_PRODUCT,
 * USB_STR_SERIAL and the USB_PID_BASE_<class> contributions live in the
 * project's configuration (caos_config.py --set), not here. 0xCA01 is the
 * development VID: whoever builds a product on caOS sets its own. */

/* ── Public API (D18: 5 functions) ──────────────────────────────────────── */

/* init: registers driver callbacks, clears device register, stops USB. */
bool ca_usb_dev_mng_init  (ca_usb_speed_t speed, u32 usb_num);

/* add: registers a class cfg; returns false if EN_USB_<type>=0 or slot full. */
bool ca_usb_dev_mng_add   (const ca_usb_device_type_t *cfg);

/* remove: symmetric to add. */
bool ca_usb_dev_mng_remove(ca_usb_type_t type);

/* start: build descriptors + ca_usb_connect. */
bool ca_usb_dev_mng_start (void);

/* stop: ca_usb_disconnect; cfgs remain registered (start remounts them). */
bool ca_usb_dev_mng_stop  (void);

/* usb_num passed to init() — accessible to services (e.g., on_configure). */
u32  ca_usb_dev_mng_get_num(void);

#else

#define ca_usb_dev_mng_init(...)   (false)
#define ca_usb_dev_mng_add(...)    (false)
#define ca_usb_dev_mng_remove(...) (false)
#define ca_usb_dev_mng_start(...)  (false)
#define ca_usb_dev_mng_stop(...)   (false)

#endif /* EN_USB */
#endif /* USB_MGMT_H */
