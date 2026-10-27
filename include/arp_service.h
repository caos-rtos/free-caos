// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    arp_service.h
 * @brief   caOS ARP cache + blocking resolve service
 *
 * @note    Architecture (VPP-inspired):
 *
 *            Application task (any priority)
 *              ca_arp_resolve(ip, mac_out, timeout_ms)
 *                ├─ cache hit  → return immediately (fast path)
 *                └─ cache miss → post s_request_sem (wake ARP task)
 *                                wait s_done_sem with timeout
 *
 *            ARP task (CA_TH_LEVEL0 — highest, blocked when idle)
 *              ca_sem_wait(&s_request_sem)
 *              for retry in ARP_RETRY_N:
 *                send ARP request frame
 *                ca_sem_timedwait(&reply_sem, ARP_RETRY_MS)
 *              post s_done_sem → wake caller
 *
 *            net task (CA_TH_LEVEL2) — processes received frames
 *              ca_net_proto_arp_handler() receives ARP frame:
 *                ca_arp_set(spa, sha)    → updates cache
 *                if s_pending.active && ip==pending.ip:
 *                  ca_sem_post(&reply_sem) → unblocks ARP task
 *
 *          Initialization (from ca_os_configure, BEFORE ca_arp_service_start):
 *            ca_arp_service_init(local_mac, local_ip)
 *
 *          Task creation (from ca_os_main, AFTER ca_arp_service_init):
 *            ca_arp_service_start()
 *
 *          IMPORTANT: ca_arp_service_init() MUST be called before
 *            ca_arp_service_start() because the ARP task runs at LEVEL0
 *            (highest priority) and will call ca_sem_wait() immediately
 *            when the scheduler starts.
 *
 *          Typical timeout_ms: >= ARP_RETRY_N * ARP_RETRY_MS (default 1500)
 ******************************************************************************
 */

#ifndef ARP_SERVICE_H_
#define ARP_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_ARP_SERVICE && EN_NET

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Initialise ARP cache and synchronisation primitives.
 * Call from ca_os_configure() BEFORE creating the ARP task.
 * local_mac / local_ip are used to build ARP request frames.
 */
void ca_arp_service_init(ca_mac_t local_mac, ca_ipv4_t local_ip);

/**
 * Create and register the ARP service task.
 * Call from ca_os_main() BEFORE ca_os_run().
 */
void ca_arp_service_start(void);

/**
 * Non-blocking cache lookup (valid + non-expired only).
 */
bool ca_arp_avail(ca_ipv4_t ip);

/**
 * Non-blocking cache lookup ignoring TTL — returns MAC even if expired.
 */
bool ca_arp_find(ca_ipv4_t ip, ca_mac_t *mac_out);

/**
 * Blocking resolve: returns true + mac_out[6] on success, false on timeout.
 * Fast path (cache hit): returns immediately without waking the ARP task.
 * Slow path: sends ARP_RETRY_N requests spaced ARP_RETRY_MS ms apart.
 * timeout_ms should be >= ARP_RETRY_N * ARP_RETRY_MS.
 */
bool ca_arp_resolve(ca_ipv4_t ip, ca_mac_t *mac_out, u32 timeout_ms);

/**
 * Insert or update a cache entry.
 * Called by net_proto ARP handler for every ARP frame seen (request+reply).
 * Also callable from application (e.g. from DHCP ACK, manual config).
 * Automatically unblocks a pending ca_arp_resolve() if ip matches.
 */
void ca_arp_set(ca_ipv4_t ip, ca_mac_t mac);

/**
 * Update only the local IP (after DHCP bind/renew) without reinitialising
 * semaphores or mutexes. Safe to call while the ARP task is running.
 */
void ca_arp_service_update_local_ip(ca_ipv4_t local_ip);

/**
 * Print ARP cache to stdout (valid entries only, with index).
 */
void ca_arp_dump(void);

/**
 * Invalidate all cache entries.
 */
void ca_arp_flush(void);

/**
 * Invalidate entry by display index (same order as ca_arp_dump).
 * Returns false if idx is out of range.
 */
bool ca_arp_del(u8 idx);

/**
 * Invalidate ARP cache entry by IPv4 address.
 * Returns false if no matching entry found.
 */
bool ca_arp_del_ip(ca_ipv4_t ip);

/**
 * Build and send a Gratuitous ARP (GARP) using the local MAC/IP
 * configured via ca_arp_service_init() / ca_arp_service_update_local_ip().
 * Allocates a pool buffer, builds the frame and calls ca_net_send().
 * Returns false if local IP is 0.0.0.0 or the buffer pool is exhausted.
 */
bool ca_arp_send_garp(void);

#endif /* EN_ARP_SERVICE && EN_NET */
#endif /* ARP_SERVICE_H_ */
