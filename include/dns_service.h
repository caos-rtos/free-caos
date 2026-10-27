// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    dns_service.h
 * @brief   caOS DNS client service — blocking hostname resolution over UDP
 *
 * @note    Architecture:
 *
 *            Application / ca_os_configure:
 *              ca_dns_service_init(server_ip, local_mac)
 *
 *            Application / ca_os_main:
 *              ca_dns_service_start()
 *
 *            net task (after ca_net_proto_init):
 *              ca_dns_service_bind()        ← register UDP port 53
 *              ca_dns_service_net_ready()   ← unblock DNS task
 *
 *            DNS task (CA_TH_LEVEL3):
 *              ca_sem_wait(s_net_ready_sem)
 *              loop:
 *                ca_queue_static_receive(s_req_q, &idx)  ← wait for request
 *                ARP-resolve server MAC (if EN_ARP_SERVICE)
 *                send DNS A-query (ca_net_proto_dns_build_query)
 *                ca_sem_timedwait(slot.reply_sem, DNS_RESPONSE_TIMEOUT_MS)
 *                  OK  → slot.resolved = true
 *                  timeout → retry DNS_RETRY_N times → slot.resolved = false
 *                ca_sem_post(slot.done_sem)  ← wake up caller
 *
 *            net task: ca_dns_service_rx_handler() called for every
 *              UDP frame on port 53. Matches txid → writes slot.result,
 *              posts slot.reply_sem. It must NOT write the caller's ip
 *              pointer: the caller's stack sits below the net task's
 *              stack_end, inside the MPU stack guard (→ DACCVIOL).
 *              ca_dns_resolve() copies slot.result out in caller context.
 *
 *          Caller:
 *            bool ok = ca_dns_resolve("pool.ntp.org", &ip, 10000);
 *
 *          Configuration (os_config.h under EN_DNS_SERVICE):
 *            DNS_PENDING_MAX          — concurrent resolve slots (default 4)
 *            DNS_RESPONSE_TIMEOUT_MS  — wait per attempt (default 3000)
 *            DNS_RETRY_N              — retries before failure (default 2)
 ******************************************************************************
 */

#ifndef DNS_SERVICE_H_
#define DNS_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"
#include "net_proto.h"

#if EN_DNS_SERVICE && EN_NET && EN_NET_DNS

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Initialise DNS service state and slot pool.
 * Call from ca_os_configure() BEFORE ca_dns_service_start().
 *
 * @param server_ip  IPv4 of the DNS server (from DHCP opt6 or static)
 * @param local_mac  local MAC address (used to build Ethernet frames)
 */
void ca_dns_service_init(ca_ipv4_t server_ip, ca_mac_t local_mac);

/**
 * Update DNS server IP (e.g. after DHCP renew with different opt6).
 * Safe to call at any time.
 */
void ca_dns_service_set_server(ca_ipv4_t server_ip);

/**
 * Create and register the DNS service task (CA_TH_LEVEL3).
 * Call from ca_os_main() BEFORE ca_os_run().
 */
void ca_dns_service_start(void);

/**
 * Register UDP port 53 with the dispatcher.
 * Call from net task AFTER ca_net_proto_init().
 */
bool ca_dns_service_bind(void);

/**
 * Signal that the net stack is ready to send/receive on port 53.
 * Call from net task AFTER ca_dns_service_bind() and AFTER local IP is valid.
 */
void ca_dns_service_net_ready(void);

/**
 * RX handler: call from net task for every UDP frame on port 53.
 * Matches transaction ID → signals the waiting DNS task slot.
 */
void ca_dns_service_rx_handler(const ca_udp_frame_t *udp, u16 len);

/**
 * Resolve a hostname to an IPv4 address (blocking).
 * Returns true if resolved, false on timeout or error.
 * *out is written only on success.
 *
 * @param hostname    null-terminated hostname (e.g. "pool.ntp.org")
 * @param out         receives the resolved IPv4
 * @param timeout_ms  max wait in ms (should be >= DNS_RETRY_N * DNS_RESPONSE_TIMEOUT_MS)
 */
bool ca_dns_resolve(const char *hostname, ca_ipv4_t *out, u32 timeout_ms);

#endif /* EN_DNS_SERVICE && EN_NET && EN_NET_DNS */
#endif /* DNS_SERVICE_H_ */
