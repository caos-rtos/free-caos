// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    arp_service.h
 * @brief   caOS ARP cache with neighbour reachability (NUD) + resolve service
 *
 * @note    Entry states:
 *
 *            REACHABLE --(ARP_ENTRY_TTL without confirmation)--> STALE
 *            STALE     --(used for TX)--> PROBE: unicast ARP, ARP_RETRY_N
 *                                         times, ARP_RETRY_MS apart
 *            PROBE     --(ARP frame or confirmation)--> REACHABLE
 *            PROBE     --(no answer)--> FAILED: entry cleared, the next
 *                                       packet broadcasts again
 *
 *          The MAC stays usable in STALE and PROBE. A confirmation is strict:
 *          an ARP frame, a TCP ACK that advances snd_una, a DNS/NTP reply
 *          matching one of our requests. Not just any frame from that MAC.
 *
 *          Application task (any priority)
 *            ca_arp_lookup()   never blocks: for every packet
 *            ca_arp_resolve()  blocks only at first contact (no entry)
 *
 *          ARP task (CA_TH_LEVEL0 — highest, blocked when idle)
 *            waits on its request queue until the next retry deadline, and
 *            drives every broadcast resolution and unicast probe from there
 *
 *          net task (CA_TH_LEVEL2) — processes received frames
 *            ca_arp_set(spa, sha)    ARP frames: entry REACHABLE
 *            ca_arp_learn(ip, mac)   IP frames: creates a missing entry only
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
 * Non-blocking lookup for the TX path, lock-free. True + MAC for a REACHABLE,
 * STALE or PROBE entry (a STALE one starts a unicast probe). False with no
 * entry: a broadcast resolution starts in the background.
 */
bool ca_arp_lookup(ca_ipv4_t ip, ca_mac_t *mac_out);

/**
 * Upper-layer proof that ip is reachable: entry back to REACHABLE, nothing on
 * the wire. Call only on strict evidence (see the file header).
 */
void ca_arp_confirm(ca_ipv4_t ip);

/**
 * Blocking resolve: returns true + mac_out[6] on success, false on timeout.
 * Fast path (entry in cache, even STALE): returns immediately.
 * Slow path: sends ARP_RETRY_N requests spaced ARP_RETRY_MS ms apart.
 * timeout_ms should be >= ARP_RETRY_N * ARP_RETRY_MS.
 */
bool ca_arp_resolve(ca_ipv4_t ip, ca_mac_t *mac_out, u32 timeout_ms);

/**
 * Insert or update a cache entry, REACHABLE.
 * Called by net_proto ARP handler for every ARP frame seen (request+reply).
 * Also callable from application (e.g. from DHCP ACK, manual config).
 * Automatically unblocks a pending ca_arp_resolve() if ip matches.
 */
void ca_arp_set(ca_ipv4_t ip, ca_mac_t mac);

/**
 * Passive learning from a received IP frame: creates a missing entry, born
 * STALE, and never refreshes an existing one (that would defeat the NUD).
 */
void ca_arp_learn(ca_ipv4_t ip, ca_mac_t mac);

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
