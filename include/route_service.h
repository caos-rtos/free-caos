// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    route_service.h
 * @brief   caOS IPv4 routing table — longest prefix match + default gateway.
 *
 *  Lookup logic in ca_route_lookup(dst):
 *    1. If (dst & local_mask) == (local_ip & local_mask) → on-link, ARP dst
 *    2. Scan route table: longest prefix match → return entry.gw
 *    3. Default gateway set → return default_gw
 *    4. No route → return dst (ARP direct, will likely fail)
 *
 *  ca_net_resolve_mac(dst, mac_out, timeout_ms):
 *    Wrapper: ca_route_lookup(dst) → ca_arp_resolve(next_hop, mac_out, timeout)
 *    Use in place of ca_arp_resolve() everywhere in the net stack.
 ******************************************************************************
 */

#ifndef ROUTE_SERVICE_H_
#define ROUTE_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_ROUTE_SERVICE && EN_NET

#ifndef NET_ROUTE_MAX
#define NET_ROUTE_MAX  8U
#endif

typedef struct {
    ca_ipv4_t dst;
    ca_ipv4_t mask;
    ca_ipv4_t gw;
    bool   active;
} ca_route_entry_t;

/**
 * Initialise routing table.
 * Call from ca_net_proto_init() after filling local IP and mask.
 * local_mask == 0 disables on-link detection until ca_route_update_local().
 */
void ca_route_init(ca_ipv4_t local_ip, ca_ipv4_t local_mask);

/**
 * Update local IP and mask at runtime (e.g. after DHCP bind/renew).
 */
void ca_route_update_local(ca_ipv4_t local_ip, ca_ipv4_t local_mask);

/**
 * Set or replace the default gateway (0.0.0.0/0).
 */
bool ca_route_set_gw(ca_ipv4_t gw);

/**
 * Add a route: dst/prefix_len via gw.
 * Returns false if table is full (NET_ROUTE_MAX entries).
 */
bool ca_route_add(ca_ipv4_t dst, u8 prefix_len, ca_ipv4_t gw);

/**
 * Delete route by table index (0..NET_ROUTE_MAX-1).
 */
bool ca_route_del(u8 idx);

/**
 * Delete all routes (default gateway preserved).
 */
void ca_route_flush(void);

/**
 * Copy route idx (0..NET_ROUTE_MAX-1) into out. Read-only, for applications
 * that need the gateways themselves (app_3 resolves its peer router's MAC).
 * Returns false if idx is out of range or the slot is empty.
 */
bool ca_route_get(u8 idx, ca_route_entry_t *out);

/**
 * Longest-prefix-match lookup.
 * Returns next-hop IP to ARP-resolve.
 */
ca_ipv4_t ca_route_lookup(ca_ipv4_t dst);

#if EN_ARP_SERVICE
/**
 * Route-aware ARP resolve: route_lookup(dst) then arp_resolve(next_hop).
 * Drop-in replacement for ca_arp_resolve() across all net services.
 */
bool ca_net_resolve_mac(ca_ipv4_t dst, ca_mac_t *mac_out, u32 timeout_ms);

/* Same next hop, per packet: non-blocking lookup, confirmation, passive learn */
bool ca_net_lookup_mac(ca_ipv4_t dst, ca_mac_t *mac_out);
void ca_net_confirm_mac(ca_ipv4_t dst);
void ca_net_learn_mac(ca_ipv4_t src, ca_mac_t mac);
#endif

#else  /* !EN_ROUTE_SERVICE */

/* Fallback when routing is disabled: on-link or default gateway */
#if EN_ARP_SERVICE && EN_NET
#include "arp_service.h"
#define ca_net_resolve_mac(dst, mac_out, timeout_ms) \
    ca_arp_resolve(ca_net_proto_next_hop(dst), (mac_out), (timeout_ms))
#define ca_net_lookup_mac(dst, mac_out) \
    ca_arp_lookup(ca_net_proto_next_hop(dst), (mac_out))
#define ca_net_confirm_mac(dst) \
    ca_arp_confirm(ca_net_proto_next_hop(dst))
#define ca_net_learn_mac(src, mac) \
    ca_arp_learn(ca_net_proto_next_hop(src), (mac))
#endif

#endif /* EN_ROUTE_SERVICE && EN_NET */
#endif /* ROUTE_SERVICE_H_ */
