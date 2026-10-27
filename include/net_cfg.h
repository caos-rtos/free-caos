// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 * @file  net_cfg.h
 * @brief caOS network stack configuration structures.
 *
 *        ca_net_cfg_t groups all protocol-level settings separately from the
 *        ETH hardware config (ca_eth_cfg_t).  Each sub-protocol has its own
 *        struct so application code can fill only what it needs.
 *
 *        Usage:
 *          ca_net_cfg_t net_cfg = {
 *              .ip  = { 192, 168, 10, 33 },
 *              .mac = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x33 },
 *              .arp = { .renew_ms = 30000U },
 *              .udp = { 0 },
 *          };
 *          ca_net_proto_init(&net_cfg);
 */

#ifndef NET_CFG_H_
#define NET_CFG_H_

#ifdef CAOS_PC_TEST
#include "caos_pc.h"
#else
#include "common.h"
#include "os_config.h"
#endif

/* ----------------------------------------------------------------------- */
/* Optimized types for network addresses                                   */
/* Always use these types for IPv4 and MAC fields in caos/net modules      */
/* ----------------------------------------------------------------------- */

/** IPv4 address: fast comparison via .addr (1 CMP instruction on ARM) */
typedef union {
    u32 addr;   /* comparison: a.addr == b.addr  |  assignment: dst = src */
    u8  v[4];   /* octet access: ip.v[0] .. ip.v[3]                     */
} ca_ipv4_t;

/** MAC address: fast comparison via .lh (1 extended instruction) or via .raw (2 instructions) */
typedef union {
    struct {
        u32 l;  // First 4 bytes of the MAC (0, 1, 2, 3)
        u16 h; // Last 2 bytes of the MAC (4, 5)        
    } __attribute__((packed)) raw;
    u8  v[6];     // For access to individual bytes
} ca_mac_t;

/* ----------------------------------------------------------------------- */
/* UDP                                                                      */
/* ----------------------------------------------------------------------- */

/* Callback: called when a UDP frame arrives on a bound port.
 *   buf_idx : pool buffer index — handler reads all context from meta:
 *     b->meta.rx.src_ip    sender IPv4
 *     b->meta.rx.src_port  sender UDP port
 *     b->meta.rx.dst_port  our UDP port (the one that matched)
 *     b->meta.rx.l4_off    frame offset to UDP header
 *     b->data + b->data_off + b->meta.rx.l4_off + 8  → payload start
 *   Handler OWNS buf_idx and MUST call ca_net_buf_free(buf_idx) when done,
 *   or pass it to another task that will free it (zero-copy async path). */
typedef void (*ca_udp_rx_t)(u16 buf_idx);

#define CA_UDP_MAX_PORTS  8U    /* max simultaneously bound UDP ports */

typedef struct {
    u16         port;           /* UDP port number (0 = unused slot) */
    ca_udp_rx_t handler;        /* callback, NULL = port open but no handler */
} ca_udp_binding_t;

typedef struct {
    ca_udp_binding_t bindings[CA_UDP_MAX_PORTS];
    u16              count;     /* number of active bindings */
} ca_udp_cfg_t;

/* ----------------------------------------------------------------------- */
/* ARP                                                                      */
/* ----------------------------------------------------------------------- */

typedef struct {
    u32 renew_ms;   /* ARP cache entry lifetime in ms (0 = no cache) */
    u8  cache_size; /* number of entries in static ARP cache (0 = disabled) */
} ca_arp_cfg_t;

/* ----------------------------------------------------------------------- */
/* ICMP                                                                     */
/* ----------------------------------------------------------------------- */

typedef struct {
    bool echo_reply; /* true = respond to ICMP echo request (ping) */
} ca_icmp_cfg_t;

/* ----------------------------------------------------------------------- */
/* DHCP client                                                              */
/* ----------------------------------------------------------------------- */

#if EN_NET_DHCP
typedef void (*ca_dhcp_bound_t)(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns);
typedef struct {
    u32             timeout_ms;  /* discover timeout (0 = 5000 ms default) */
    ca_dhcp_bound_t on_bound;    /* called when IP is assigned (may be NULL) */
} ca_dhcp_cfg_t;
#endif

/* ----------------------------------------------------------------------- */
/* NTP client                                                               */
/* ----------------------------------------------------------------------- */

#if EN_NET_NTP
typedef void (*ca_ntp_synced_t)(u32 unix_time);  /* called on sync */
typedef struct {
    ca_ipv4_t          server_ip;   /* NTP server IPv4 */
    u32             poll_ms;     /* poll interval in ms (0 = 64000 default) */
    ca_ntp_synced_t on_synced;   /* called when time is updated (may be NULL) */
} ca_ntp_cfg_t;
#endif

/* ----------------------------------------------------------------------- */
/* Syslog client (RFC 5424 subset, UDP)                                    */
/* ----------------------------------------------------------------------- */

#if EN_NET_SYSLOG
typedef struct {
    ca_ipv4_t server_ip;   /* syslog server IPv4 */
    u16    server_port; /* syslog UDP port (0 = 514 default) */
    u8     facility;    /* syslog facility (0-23, 1=user) */
} ca_syslog_cfg_t;
#endif

/* ----------------------------------------------------------------------- */
/* Top-level network config                                                 */
/* ----------------------------------------------------------------------- */

typedef struct {
    ca_ipv4_t        ip;    /* local IPv4 address */
    ca_ipv4_t        mask;  /* local subnet mask (0 = unknown, set by DHCP) */
    ca_mac_t         mac;   /* local MAC address (used for L2 filter) */
    u16           mtu;            /* max TX payload bytes (0 = default 1500) */
    u8            phy_nrst_port;  /* GPIO port index for PHY nRST (0xFF = not wired) */
    u8            phy_nrst_pin;   /* GPIO pin 0..15 */
    ca_arp_cfg_t  arp;
    ca_udp_cfg_t  udp;
    ca_icmp_cfg_t icmp;
#if EN_NET_DHCP
    ca_dhcp_cfg_t dhcp;
#endif
#if EN_NET_NTP
    ca_ntp_cfg_t  ntp;
#endif
#if EN_NET_SYSLOG
    ca_syslog_cfg_t syslog;
#endif
} ca_net_cfg_t;

#endif /* NET_CFG_H_ */
