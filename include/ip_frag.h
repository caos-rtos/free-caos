// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    ip_frag.h
 * @brief   IPv4 fragmentation service (transmit side)
 *
 * @note    For links whose MTU is below Ethernet's 1500 (radio, 512..1480).
 *          Every IPv4 datagram larger than the MTU of its EGRESS interface is
 *          taken out of ca_net_send_on() and cut into fragments by a service
 *          task, so no builder has to know about fragmentation.
 *
 *            ca_net_send_on(iface, buf)        [caller's task]
 *              L2 payload <= iface MTU  -> iface->send() as before
 *              DF set                   -> dropped, ICMP 3/4 to a remote
 *                                          sender (forwarded datagram)
 *              otherwise                -> L4 checksum filled, buffer queued
 *                                          (IP_FRAG_IN_DEPTH), returns true
 *            ip_frag task
 *              new IP id, cut into (mtu - ihl) & ~7 byte pieces, every piece
 *              its own net_buf (max IP_FRAG_QUEUE_DEPTH), original freed,
 *              pieces sent in order - the task WAITS while the driver refuses
 *
 *          Asynchronous: ca_net_send*() returns true once the datagram is in
 *          the queue; what goes wrong later shows only in the counters
 *          ('ip stats', ca_ip_frag_stats()).
 *
 *          Until ca_ip_frag_service_start() runs nothing is diverted: an
 *          oversized frame reaches the driver as it always did.
 *
 *          TCP never needs it: the MSS follows the MTU of the egress iface.
 *          A TCP segment over the MTU is counted (tcp_oversize) as a bug.
 *
 *          Plan and boundaries: ip_frag_mtu.md (repository root).
 ******************************************************************************
 */

#ifndef IP_FRAG_H_
#define IP_FRAG_H_

#include "common.h"
#include "os_config.h"

#if EN_NET && EN_IP_FRAG

#include "net_iface.h"

#ifndef IP_FRAG_IN_DEPTH
#define IP_FRAG_IN_DEPTH     4U    /* datagrams waiting for the service */
#endif

#ifndef IP_FRAG_QUEUE_DEPTH
#define IP_FRAG_QUEUE_DEPTH  16U   /* fragments of one datagram held at once */
#endif

/* Counters. All monotonic since boot except in_hwm. */
typedef struct {
    u32 dgrams;        /* datagrams fragmented and fully sent              */
    u32 frags;         /* fragments handed to an iface                     */
    u32 drop_df;       /* DF set and over the MTU: dropped                 */
    u32 drop_queue;    /* input queue full: dropped                        */
    u32 drop_pool;     /* not enough free net_buf for every fragment       */
    u32 drop_big;      /* more fragments than IP_FRAG_QUEUE_DEPTH          */
    u32 drop_tx;       /* driver kept refusing: rest of the datagram lost  */
    u32 drop_bad;      /* malformed header, or the iface went away         */
    u32 wait_tx;       /* times the service waited on a refusing driver    */
    u32 tcp_oversize;  /* local TCP segment over the MTU (MSS bug)         */
    u32 icmp_sent;     /* ICMP 3/4 "fragmentation needed" sent             */
    u16 in_hwm;        /* most datagrams ever waiting in the input queue   */
} ca_ip_frag_stats_t;

/* Start the fragmentation task. Call once, from ca_os_main() (it creates a
 * task), after ca_net_init(). Repeated calls do nothing. */
void ca_ip_frag_service_start(void);

/* true once the service is running: only then does ca_net_send_on() divert. */
bool ca_ip_frag_active(void);

/* Used by ca_net_send_on(): true if buf_idx is an IPv4 datagram whose L2
 * payload exceeds the MTU of tx and the service is running. */
bool ca_ip_frag_oversize(const ca_net_iface_t *tx, u16 buf_idx);

/* Used by ca_net_send_on() when ca_ip_frag_oversize() said true. TAKES the
 * buffer whatever it returns. true = queued for fragmentation. */
bool ca_ip_frag_submit(const ca_net_iface_t *tx, u16 buf_idx);

/* Counter snapshot / reset (in_hwm restarts from the current queue fill). */
void ca_ip_frag_stats(ca_ip_frag_stats_t *out);
void ca_ip_frag_stats_reset(void);

#endif /* EN_NET && EN_IP_FRAG */
#endif /* IP_FRAG_H_ */
