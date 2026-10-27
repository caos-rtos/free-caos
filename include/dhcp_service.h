// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    dhcp_service.h
 * @brief   caOS DHCP client service — DORA + T1/T2 renew state machine
 *
 * @note    Architecture:
 *
 *            Application / ca_os_configure:
 *              ca_dhcp_service_init(local_mac, on_bound, on_expire)
 *
 *            Application / ca_os_main:
 *              ca_dhcp_service_start()
 *
 *            net task (after ca_net_proto_init is called):
 *              ca_dhcp_service_bind()   ← called once, blocks until BOUND
 *
 *            DHCP task (CA_TH_LEVEL1 — high priority, blocked when idle):
 *
 *              DISCOVERING:
 *                send DISCOVER (broadcast, xid random)
 *                wait ca_sem_timedwait(s_offer_sem, DHCP_OFFER_TIMEOUT_MS)
 *                on timeout retry DHCP_DISCOVER_RETRY_N times
 *
 *              REQUESTING:
 *                send REQUEST (broadcast, ciaddr=0, opt50+opt54)
 *                wait ca_sem_timedwait(s_ack_sem, DHCP_REQUEST_TIMEOUT_MS)
 *                on NAK or timeout → back to DISCOVERING
 *
 *              BOUND:
 *                post s_bound_sem (unblocks ca_dhcp_service_bind())
 *                fire on_bound callback
 *                sleep until T1 (lease/2)
 *
 *              RENEWING (T1 expired — unicast to server):
 *                send REQUEST (unicast, ciaddr=assigned_ip, no opt50/54)
 *                wait ca_sem_timedwait(s_ack_sem, DHCP_RENEW_TIMEOUT_MS)
 *                on ACK → re-arm timers, stay BOUND
 *                on timeout → try until T2
 *
 *              REBINDING (T2 expired — broadcast):
 *                send REQUEST (broadcast, ciaddr=assigned_ip, no opt50/54)
 *                on ACK → re-arm timers, back to BOUND
 *                on NAK or lease expiry → on_expire(), back to DISCOVERING
 *
 *            net task: ca_dhcp_service_rx_handler() called for every
 *              UDP frame on port 68.  Posts s_offer_sem or s_ack_sem
 *              depending on msg_type, stores ca_dhcp_offer_t.
 *
 *          Configuration (os_config.h under EN_DHCP_SERVICE):
 *            DHCP_DISCOVER_RETRY_N   — retries before giving up (default 3)
 *            DHCP_OFFER_TIMEOUT_MS   — wait for OFFER per attempt (default 4000)
 *            DHCP_REQUEST_TIMEOUT_MS — wait for ACK (default 4000)
 *            DHCP_RENEW_TIMEOUT_MS   — unicast renew retry interval (default 10000)
 ******************************************************************************
 */

#ifndef DHCP_SERVICE_H_
#define DHCP_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"
#include "net_proto.h"

#if EN_DHCP_SERVICE && EN_NET && EN_NET_DHCP

/* ----------------------------------------------------------------------- */
/* Callbacks                                                                */
/* ----------------------------------------------------------------------- */

/** Absolute floor for the DHCP task's stack, in bytes.
 *
 * STACK_SMALL is a ratio (STACK_MIN*2) and STACK_MIN is 512 on the F4 family
 * against 2048 on the U575, so the same constant means 1 KB in one place and
 * 4 KB in another. Measured on an F446 the task peaks at 60.4% of 1 KB during
 * the DORA exchange - the same figure across four different examples, since
 * the depth belongs to parsing and formatting the lease, which is the same
 * code on every part.
 */
#define DHCP_TASK_STACK_MIN   2048U

/**
 * Called from DHCP task when the address is successfully leased (BOUND state).
 * @param ip   assigned IP address
 * @param mask subnet mask
 * @param gw   default gateway
 * @param dns  primary DNS server
 */
typedef void (*ca_dhcp_bound_cb_t)(ca_ipv4_t ip, ca_ipv4_t mask,
                                   ca_ipv4_t gw,  ca_ipv4_t dns);

/**
 * Called from DHCP task when the lease expires and cannot be renewed.
 * The application should stop using the IP address.
 */
typedef void (*ca_dhcp_expire_cb_t)(void);

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Initialise DHCP service state and synchronisation primitives.
 * Call from ca_os_configure() BEFORE ca_dhcp_service_start().
 */
void ca_dhcp_service_init(ca_mac_t             local_mac,
                          ca_dhcp_bound_cb_t  on_bound,
                          ca_dhcp_expire_cb_t on_expire);

/**
 * Create and register the DHCP service task (CA_TH_LEVEL1).
 * Call from ca_os_main() BEFORE ca_os_run().
 */
void ca_dhcp_service_start(void);

/**
 * Blocking call: returns true when DHCP enters BOUND state.
 * Call from net task after ca_net_proto_init().
 * Blocks until lease obtained or timeout (DHCP_BIND_TIMEOUT_MS).
 */
bool ca_dhcp_service_bind(void);

/**
 * RX handler: call from net task for every UDP frame on port 68.
 * Parses OFFER / ACK / NAK and signals the DHCP task.
 */
void ca_dhcp_service_rx_handler(const ca_udp_frame_t *udp, u16 len);

/* Exposed for inline — written only by the DHCP task */
extern volatile bool s_dhcp_bound_flag;
static inline bool ca_dhcp_service_bound(void) { return s_dhcp_bound_flag; }

/**
 * Signal that the net stack is ready to receive on port 68.
 * Call from net task AFTER ca_net_proto_init() + ca_net_proto_bind_udp(68,...).
 * Until this is called the DHCP task blocks before sending the first DISCOVER.
 */
void ca_dhcp_service_net_ready(void);

/**
 * Force an early unicast renew (e.g. before moving to a new network). */
void ca_dhcp_service_renew(void);

/* Leased parameters — valid only when ca_dhcp_service_bound() == true */
ca_ipv4_t ca_dhcp_service_get_ip(void);
ca_ipv4_t ca_dhcp_service_get_mask(void);
ca_ipv4_t ca_dhcp_service_get_gw(void);
ca_ipv4_t ca_dhcp_service_get_dns(void);
/* opt42 NTP server from last OFFER/ACK; 0.0.0.0 if server did not send it */
ca_ipv4_t ca_dhcp_service_get_ntp_server(void);

/**
 * Current FSM state as string (INIT/DISCOVERING/REQUESTING/BOUND/RENEWING/REBINDING) */
const char *ca_dhcp_service_get_state(void);

#endif /* EN_DHCP_SERVICE && EN_NET && EN_NET_DHCP */
#endif /* DHCP_SERVICE_H_ */
