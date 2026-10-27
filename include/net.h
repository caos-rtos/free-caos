// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    net.h
 * @brief   caOS bare-metal network stack - public API
 *
 * @note    Layer model:
 *            driver (ETH / USB-NCM / ...)  ->  ca_net_iface_t (net_iface.h)
 *              |  iface->send() / iface->recv() | ca_net_rx_push()
 *            net.c  -- L2 dispatcher (EtherType -> handler table) + routing
 *              |  ca_net_handler_t callbacks
 *            arp.c / ip4.c / icmp.c / udp.c
 *
 *          Usage from application:
 *            1. Fill ca_eth_cfg_t (mac, phy_addr, rmii, link_cb).
 *            2. Call ca_net_init(ca_eth_iface_get(&cfg)) from ca_os_configure().
 *            3. Create a net task that loops:
 *                 ca_sem_wait(&ca_net_rx_sem);
 *                 ca_net_process();
 *            4. Register protocol handlers with ca_net_register().
 *
 *          ca_net_process() drains all pending RX frames from the ETH driver,
 *          allocates a pool buffer per frame, dispatches by EtherType.
 *          If no handler is registered for an EtherType the buffer is freed.
 *
 *          Handlers run in net task context.  They take ownership of the buffer
 *          and must call ca_net_buf_free(idx) when done.
 ******************************************************************************
 */

#ifndef NET_H_
#define NET_H_

#include "common.h"
#include "os_config.h"

#if EN_NET

#include "net_iface.h"
#include "net_buf.h"
#include "net_queue.h"
#include "sync.h"

/* ----------------------------------------------------------------------- */
/* EtherType constants                                                      */
/* ----------------------------------------------------------------------- */
#define ETH_P_IP    0x0800U
#define ETH_P_ARP   0x0806U
#define ETH_P_IPV6  0x86DDU
#define ETH_P_VLAN  0x8100U

/* ----------------------------------------------------------------------- */
/* Protocol handler type                                                    */
/* ----------------------------------------------------------------------- */
/* Called in net task context.  buf_idx is a valid pool buffer with:
 *   data_off  = NET_BUF_HEADROOM (start of Ethernet frame)
 *   data_len  = received byte count (includes Ethernet header)
 * Handler owns the buffer and must ca_net_buf_free(buf_idx) when done. */
typedef void (*ca_net_handler_t)(u16 buf_idx);

#define NET_HANDLER_MAX  8U   /* max registered EtherType handlers */

/* ----------------------------------------------------------------------- */
/* Statistics                                                               */
/* ----------------------------------------------------------------------- */
typedef struct {
    u32 rx_frames;        /* frames received */
    u32 rx_dropped_pool;  /* dropped: pool exhausted */
    u32 rx_dropped_small; /* dropped: frame < 14 bytes */
    u32 rx_no_handler;    /* received but no handler registered */
    u32 tx_frames;        /* frames sent */
    u32 tx_failed;        /* iface->send() returned false */
    u32 err_bus;          /* CA_ETH_ERR_BUS events */
    u32 err_rbu;          /* CA_ETH_ERR_RBU events */
    u32 process_calls;    /* how many times ca_net_process() was called */
} ca_net_stats_t;

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/* Initialise buffer pool + queues, then register iface as the default
 * interface and start it (iface->start()).
 * iface must stay valid for the lifetime of the stack (typically static).
 * For ETH use ca_eth_iface_get(&cfg) to obtain the iface (see eth_iface.h).
 * Returns false if iface is NULL or iface->start() fails. */
bool ca_net_init(ca_net_iface_t *iface);

/* Register an additional interface (e.g. USB-NCM alongside ETH).
 * Assigns iface->id and starts it. Returns false on table full / start fail.
 * The first registered iface (via ca_net_init) is the default TX route. */
bool ca_net_register_iface(ca_net_iface_t *iface);

/* Push-based RX: a driver (e.g. NCM) calls this when it has a complete L2
 * frame in buf_idx. The core enqueues it and posts ca_net_rx_sem.
 * Used by iface implementations that leave ops.recv = NULL.
 * Returns false (and frees buf_idx) if the RX queue is full. */
bool ca_net_rx_push(ca_net_iface_t *iface, u16 buf_idx);

/* Process all pending RX frames - call from net task after ca_sem_wait().
 * For pull ifaces (ops.recv != NULL) drains the driver recv ring;
 * for push ifaces drains the RX queue. Dispatches by EtherType,
 * frees unhandled buffers. */
void ca_net_process(void);

/* Transmit buffer buf_idx on the default TX interface.
 * buf->data_off and buf->data_len must be set by caller.
 * OWNERSHIP: the buffer is handed to the interface and is never the caller's
 * again, whatever this returns. The interface releases it - immediately if it
 * copies (NCM), or when the TX slot is reused if it is zero-copy (ETH). The
 * caller must NOT free buf_idx after calling this. */
bool ca_net_send(u16 buf_idx);

/* As ca_net_send(), on interface iface_id (ca_net_iface_t.id). Same ownership
 * rule. Fails (and frees buf_idx) if the id is unknown or the iface is down.
 * Fills the checksums in software when the iface has no offload but the
 * builders left them to the hardware. */
bool ca_net_send_on(u8 iface_id, u16 buf_idx);

/* As ca_net_send_on(), but the frame leaves EXACTLY as it is: no checksum
 * filled in software, no checksum inserted by the MAC (ETH CIC=00). For a
 * frame relayed from another iface, which already carries its checksums -
 * on ETH, CIC=11 would insert a wrong ICMP checksum into it. */
bool ca_net_send_raw(u8 iface_id, u16 buf_idx);

/* Registered interfaces: count, and the one with a given id (NULL if none). */
u8              ca_net_iface_count(void);
ca_net_iface_t *ca_net_iface_at(u8 id);

/* Start (up=true) or stop one interface without touching the others. */
bool ca_net_iface_link(u8 id, bool up);

/* Register (or replace) EtherType handler. fn=NULL removes handler. */
void ca_net_register(u16 ethertype, ca_net_handler_t fn);

#if EN_NET_FILTER
/* ----------------------------------------------------------------------- */
/* RX filter --- one slot, ahead of the dispatcher                          */
/* ----------------------------------------------------------------------- */
/* A single EtherType can be pulled out of the receive path before anything
 * else looks at it: no VLAN/EtherType parse, no b->meta fill, no linear scan
 * of the handler table, no dispatch. The frame goes straight to fn.
 *
 * WHAT THIS IS AND IS NOT
 *
 * It is a SHORTER path, not a more urgent one. The filter runs in the same
 * net task, on the same wakeup, inside the same ca_net_process() drain as
 * every other frame - nothing here preempts anything. What it removes is the
 * per-frame work between the driver and the consumer, and - the reason it was
 * written - everything above L2: a filtered frame never reaches IP, never
 * enters a TCP receive ring, never waits on an application to drain it.
 *
 * That makes it a measuring instrument: point it at an EtherType and whatever
 * it echoes back tells you what the driver and the net task can do on their
 * own, with the transport taken out of the picture (see example_84).
 *
 * The callback runs in net task context and OWNS buf_idx: it must either free
 * it or hand it to the TX path. It must not block - the whole receive drain
 * is behind it.
 *
 * Frames of every other EtherType are untouched and keep their normal route.
 * fn = NULL (or ca_net_filter_clear) removes the filter. */
typedef void (*ca_net_filter_fn_t)(u16 buf_idx);

void ca_net_filter_set(u16 ethertype, ca_net_filter_fn_t fn);
void ca_net_filter_clear(void);

/* Frames taken by the filter since it was set. */
u32  ca_net_filter_count(void);

/* MAC filters - by SOURCE MAC, checked before the EtherType filter above.
 * fn gets the frame and the id of the iface it came in on, and returns true
 * if it took the buffer (it must then free it or send it), false to let the
 * frame go on through the stack as usual. Runs in the net task, must not
 * block. fn = NULL clears the slot. Written for an application that relays
 * frames between two ifaces (example_95). */
#ifndef NET_MAC_FILTER_MAX
#define NET_MAC_FILTER_MAX  2U
#endif
typedef bool (*ca_net_mac_filter_fn_t)(u16 buf_idx, u8 iface);

bool ca_net_filter_mac_set(u8 slot, const ca_mac_t *src, ca_net_mac_filter_fn_t fn);
#endif /* EN_NET_FILTER */

/* Read statistics snapshot. */
void ca_net_stats(ca_net_stats_t *out);

/* Account a driver-level error (iface-agnostic). Called by iface adapters
 * from ISR/task context. bus = fatal DMA bus error, rbu = RX buffer lost. */
void ca_net_count_err(bool bus, bool rbu);

/* true if ANY registered interface performs HW checksum offload (ETH DMA):
 * the builders then leave IP/L4 checksum fields = 0, and ca_net_send_on()
 * fills them in software for a frame leaving from an iface without offload.
 * false (NCM only, no iface) → the builders calculate them. */
bool ca_net_tx_csum_offload(void);

/* L2 payload (MTU) every registered interface accepts: the smallest one,
 * since a datagram is cut before its egress iface is known. The medium
 * decides it: NCM will not send a frame over 1514 bytes, an ETH driver has
 * more room. Returns IP_TX_MTU_DEFAULT when no interface is registered or the
 * field is 0.
 * Used by ca_ip_fragment_send() to decide whether to fragment. */
u16 ca_net_tx_mtu(void);

/* Soft link control: block/unblock all TX and RX without reinitialising ETH.
 * ca_net_link_down() drives PHY nRST low (if wired) and sets the link flag.
 * ca_net_link_up()   drives PHY nRST high (if wired) and clears the flag.
 * ca_net_link_status() returns true when the link is enabled.
 * ca_net_set_phy_nrst() configures the optional GPIO (port 0xFF = not wired). */
void ca_net_link_down(void);
void ca_net_link_up(void);
bool ca_net_link_status(void);
void ca_net_set_phy_nrst(u8 port, u8 pin);

/* Semaphore posted by RX ISR - take in net task to block until RX arrives. */
extern ca_semaphore_t ca_net_rx_sem;

#endif /* EN_NET */
#endif /* NET_H_ */
