// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    udp_client.h
 * @brief   caOS UDP client service — blocking connect + async/blocking RX
 *
 * @note    Architecture:
 *
 *          ca_udp_client_connect(&cfg)
 *            ├─ resolve server MAC via ca_arp_resolve()
 *            ├─ assign local_port (auto-ephemeral or user-specified)
 *            ├─ register handler with ca_net_proto_bind_udp(local_port)
 *            └─ return handle (1..UDPCLI_MAX_HANDLES) or 0 on error
 *
 *          ca_udp_client_send(handle, payload, len)
 *            └─ build Eth+IP+UDP frame → ca_net_send()
 *
 *          RX async  (on_rx != NULL):
 *            net task → udp_cli_rx_handler → call on_rx() directly
 *
 *          RX blocking (on_rx == NULL):
 *            net task → udp_cli_rx_handler → push ring + post recv_sem
 *            application → ca_udp_client_recv(handle, buf, ..., timeout_ms)
 *
 *          ca_udp_client_disconnect(handle)
 *            └─ ca_net_proto_bind_udp(local_port, NULL) + drain ring
 *
 ******************************************************************************
 */

#ifndef UDP_CLIENT_H_
#define UDP_CLIENT_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_UDP_CLIENT && EN_NET

/* ----------------------------------------------------------------------- */
/* Callback                                                                 */
/* ----------------------------------------------------------------------- */

/** Async RX callback: called in net task — must be fast, no blocking.
 *  @param handle    client handle (1..UDPCLI_MAX_HANDLES)
 *  @param src_ip    sender IPv4
 *  @param src_port  sender UDP port
 *  @param payload   UDP payload pointer (valid only during callback)
 *  @param len       payload length
 */
typedef void (*ca_udp_cli_rx_t)(u8 handle,
                                 ca_ipv4_t src_ip, u16 src_port,
                                 const u8 *payload, u16 len);

/* ----------------------------------------------------------------------- */
/* Configuration                                                            */
/* ----------------------------------------------------------------------- */

typedef struct {
    ca_ipv4_t          server_ip;
    u16             server_port;
    u16             local_port;         /* 0 = ephemeral (49152 + slot) */
    u32             arp_timeout_ms;
    ca_mac_t           local_mac;          /* our MAC (for TX frames) */
    ca_ipv4_t          local_ip;           /* our IP  (for TX frames) */
    ca_udp_cli_rx_t on_rx;             /* NULL = use blocking recv */
} ca_udp_client_cfg_t;

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Open connection: resolve server MAC, bind local port.
 * Returns handle (1..UDPCLI_MAX_HANDLES) on success, 0 on error.
 * Blocks for up to arp_timeout_ms during MAC resolution.
 */
u8   ca_udp_client_connect(const ca_udp_client_cfg_t *cfg);

/**
 * Send payload to server.
 * Returns false if handle invalid, not connected, or TX buffer unavailable.
 */
bool ca_udp_client_send(u8 handle, const u8 *payload, u16 len);

/**
 * Blocking receive (only when on_rx == NULL in cfg).
 * Returns false on timeout or invalid handle.
 * out_len receives actual payload length copied into buf.
 */
bool ca_udp_client_recv(u8 handle,
                         u8 *buf, u16 buf_len, u16 *out_len,
                         u32 timeout_ms);

/**
 * Close connection: unbind port, drain ring.
 */
void ca_udp_client_disconnect(u8 handle);

/**
 * Refresh server MAC (e.g. if ARP entry expired).
 * Returns false if ARP resolve fails within arp_timeout_ms.
 */
bool ca_udp_client_refresh_mac(u8 handle, u32 arp_timeout_ms);

#endif /* EN_UDP_CLIENT && EN_NET */
#endif /* UDP_CLIENT_H_ */
