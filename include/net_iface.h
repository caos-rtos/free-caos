// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    net_iface.h
 * @brief   caOS network interface abstraction (driver <-> stack glue)
 *
 * @note    Decouples the net.c core from the physical driver (ETH, USB-NCM, ...).
 *          A ca_net_iface_t is a struct of function pointers ("ops")
 *          plus L2 metadata (mac, mtu, id). The net.c core uses only the ops,
 *          never direct calls to ca_eth_*.
 *
 *          Hybrid RX model (D: hybrid method):
 *            - recv() OPTIONAL: zero-copy pull (ETH/DMA). If present,
 *              ca_net_process() calls it in a loop as before.
 *            - rx_push() of the core: push-based. The driver (e.g. NCM) calls
 *              ca_net_rx_push(iface, buf_idx) when it has a frame; the core
 *              queues it and posts ca_net_rx_sem. ca_net_process() drains the queue.
 *          An iface implements ONE of the two: recv for pull, NULL for push.
 *
 *          Multi-iface: the core maintains an iface table (NET_IFACE_MAX).
 *          ca_net_register_iface() assigns the id. ca_net_send() uses the
 *          default iface (first registered); ca_net_send_on() picks one, and
 *          replies (ARP, ICMP echo) leave from the iface the request came in.
 ******************************************************************************
 */

#ifndef NET_IFACE_H_
#define NET_IFACE_H_

#include "common.h"
#include "os_config.h"

#if EN_NET

#include "net_cfg.h"   /* ca_mac_t */

#ifndef NET_IFACE_MAX
#define NET_IFACE_MAX  2U   /* e.g. eth0 + usb0 */
#endif

#define NET_IFACE_INVALID  0xFFU

typedef struct ca_net_iface ca_net_iface_t;

/**
 * @brief L2 network interface — driver/stack connection.
 *
 * The driver compiles the ops and metadata, then registers it with
 * ca_net_register_iface(&iface). Lifetime: the struct must remain valid
 * for the entire life of the stack (typically static).
 */
struct ca_net_iface {
    cstr  name;                 /* "eth0", "usb0" — diagnostics only */
    u8    id;                   /* assigned by ca_net_register_iface */
    ca_mac_t mac;                  /* L2 MAC address of this iface */
    u16   mtu;                  /* L2 MTU (max payload), 0 = default 1500 */
    bool  up;                   /* state managed by the core via start/stop */
    bool  csum_offload;         /* true = HW calculates IP/L4 checksum in TX (ETH DMA);
                                   false = SW must calculate them (NCM, no offload) */

    void *priv;                 /* driver ctx (e.g. ca_eth_cfg_t*, NCM ctx) */

    /* Per-iface counters, kept by the core (CLI 'ip if') */
    u32   rx_frames;
    u32   tx_frames;
    u32   tx_failed;

    /* --- driver -> stack ops -------------------------------------------- */

    /* Starts/restarts the driver. Returns false on error. Mandatory. */
    bool (*start)(ca_net_iface_t *self);

    /* Stops the driver (deinit). Mandatory. */
    bool (*stop)(ca_net_iface_t *self);

    /* TX of a net_buf (zero-copy: in case of success the driver owns
     * the buffer until the slot is reused). Mandatory. */
    bool (*send)(ca_net_iface_t *self, u16 buf_idx);

    /* OPTIONAL zero-copy RX pull (ETH/DMA model).
     *   new_idx: replacement buffer to load into the RX descriptor.
     *   out_idx: buffer containing the received frame.
     * Returns true if a frame was extracted; false if no frame
     * (in that case new_idx is NOT consumed — the core frees it).
     * NULL for push-based iface (NCM): in that case the driver calls
     * ca_net_rx_push(). */
    bool (*recv)(ca_net_iface_t *self, u16 *out_idx, u16 new_idx);
};

#endif /* EN_NET */
#endif /* NET_IFACE_H_ */
