// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    udp_server.h
 * @brief   caOS UDP server service — multi-client, per-client worker task
 *
 * @note    Architecture:
 *
 *            ca_udp_server_start(&cfg)           (from ca_os_main)
 *              ├─ inits client table + mutex
 *              ├─ spawns UDPSRV_MAX_CLIENTS worker tasks (CA_TH_LEVEL3)
 *              └─ registers handler with ca_net_proto_bind_udp(port)
 *
 *            net task  →  udp_srv_rx_handler(buf_idx)
 *              ├─ lookup/assign client slot by src_ip:src_port
 *              ├─ push buf_idx into client ring
 *              └─ post pkt_sem  (+ start_sem if new client)
 *
 *            worker task (per slot)
 *              ├─ wait start_sem       (new client assigned)
 *              ├─ loop: timedwait pkt_sem(client_timeout_ms)
 *              │         pop ring → call cfg.on_rx()
 *              └─ timeout → disconnect, wait next client
 *
 *          Initialization (from ca_os_main, before ca_os_run):
 *            ca_udp_server_start(&cfg)
 *
 *          Send reply:
 *            ca_udp_server_send(client_id, payload, len)
 *
 ******************************************************************************
 */

#ifndef UDP_SERVER_H_
#define UDP_SERVER_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_UDP_SERVER && EN_NET

/* ----------------------------------------------------------------------- */
/* Callbacks                                                                */
/* ----------------------------------------------------------------------- */

/** RX callback: called in the worker task of the client.
 *  @param client_id  slot index (0 .. UDPSRV_MAX_CLIENTS-1)
 *  @param src_ip     sender IPv4 (4 bytes, network order)
 *  @param src_port   sender UDP port
 *  @param payload    pointer to UDP payload (valid for callback duration)
 *  @param len        payload length in bytes
 */
typedef void (*ca_udp_srv_rx_t)(u8 client_id,
                                 ca_ipv4_t src_ip, u16 src_port,
                                 const u8 *payload, u16 len);

/** Event callback: called on client connect / disconnect. */
typedef void (*ca_udp_srv_event_t)(u8 client_id,
                                    ca_ipv4_t src_ip, u16 src_port);

/* ----------------------------------------------------------------------- */
/* Configuration                                                            */
/* ----------------------------------------------------------------------- */

/** Absolute floor for a worker's stack, in bytes.
 *
 * STACK_SMALL is a ratio (STACK_MIN*2), and STACK_MIN differs by part: 512 on
 * the F4 family against 2048 on the U575, so the same constant means 1 KB in
 * one place and 4 KB in another. A worker carrying traffic reached 89.5% of
 * 1 KB on an F446. The depth comes from the user callback, which is the same
 * code everywhere, so the requirement is an amount rather than a ratio.
 */
#define UDPSRV_WORKER_STACK_MIN   2048U

typedef struct {
    u16                 port;               /* UDP port to listen on */
    u8                  max_clients;        /* <= UDPSRV_MAX_CLIENTS */
    u32                 client_timeout_ms;  /* inactivity timeout */
    ca_mac_t               local_mac;          /* our MAC (for TX frames) */
    ca_ipv4_t              local_ip;           /* our IP  (for TX frames) */
    ca_udp_srv_rx_t     on_rx;              /* mandatory */
    ca_udp_srv_event_t  on_connect;         /* NULL ok */
    ca_udp_srv_event_t  on_disconnect;      /* NULL ok */
} ca_udp_server_cfg_t;

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Initialise server data structures and spawn worker tasks.
 * Call from ca_os_main() BEFORE ca_os_run().
 * Does NOT bind the UDP port — call ca_udp_server_bind() after
 * ca_net_proto_init() from within the net task.
 */
bool ca_udp_server_start(const ca_udp_server_cfg_t *cfg);

/**
 * Register the server UDP port with the protocol stack.
 * Call from the net task AFTER ca_net_proto_init().
 */
bool ca_udp_server_bind(void);

/**
 * Send payload to a connected client.
 * Uses the src_mac saved at connect time — no ARP needed.
 * Returns false if client_id invalid, not connected, or TX buffer unavailable.
 */
bool ca_udp_server_send(u8 client_id, const u8 *payload, u16 len);

/* Exposed for inline — use only ca_udp_server_set_local_ip() to write */
extern ca_udp_server_cfg_t s_udp_srv_cfg;
/** Update local IP used for TX frames (call after DHCP bound). */
static inline void ca_udp_server_set_local_ip(ca_ipv4_t ip)
{
    s_udp_srv_cfg.local_ip.addr = ip.addr;
}

/**
 * Force-disconnect a client: drain ring, mark slot free.
 */
void ca_udp_server_disconnect(u8 client_id);

/**
 * Return number of currently connected clients.
 */
u8 ca_udp_server_client_count(void);

/**
 * Fill info for a specific slot (client_id 0..UDPSRV_MAX_CLIENTS-1).
 * Returns false if client_id out of range or slot not connected.
 */
typedef struct {
    bool   connected;
    ca_ipv4_t src_ip;
    u16    src_port;
    u8   ring_used;   /* packets queued in ring */
} ca_udp_srv_client_info_t;

bool ca_udp_server_get_client(u8 client_id, ca_udp_srv_client_info_t *info);

#endif /* EN_UDP_SERVER && EN_NET */
#endif /* UDP_SERVER_H_ */
