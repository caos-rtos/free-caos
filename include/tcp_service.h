// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 * @file  tcp_service.h
 * @brief caOS TCP client service — active open, max 4 connections.
 *
 *        Constraints:
 *          - TCP_MAX_CONN concurrent connections (default 4)
 *          - TCP_MAX_INFLIGHT TX segments in flight per connection (default 4)
 *          - Zero-copy: buf_idx held in PCB until peer ACK
 *          - RTO calculated on ca_os_tick_ms(), no dedicated HW timer
 *          - No congestion control: only window-based flow control
 *
 *        Client state machine (active open):
 *          CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT_1 → FIN_WAIT_2
 *                                                        → TIME_WAIT → CLOSED
 *        Passive close (peer closes):
 *          ESTABLISHED → CLOSE_WAIT → LAST_ACK → CLOSED
 */

#ifndef TCP_SERVICE_H_
#define TCP_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"
#include "net_buf.h"
#include "sync.h"

#if EN_TCP_SERVICE && EN_NET

/* ----------------------------------------------------------------------- */
/* Callbacks                                                               */
/* ----------------------------------------------------------------------- */
typedef void (*ca_tcp_rx_t)(u8 handle, const u8 *data, u16 len);
typedef void (*ca_tcp_conn_t)(u8 handle);     /* called when ESTABLISHED */
typedef void (*ca_tcp_close_t)(u8 handle);    /* called on close/error */

/* ----------------------------------------------------------------------- */
/* Config for ca_tcp_connect()                                             */
/* ----------------------------------------------------------------------- */
typedef struct {
    ca_ipv4_t         remote_ip;
    u16            remote_port;
    ca_ipv4_t         local_ip;
    ca_mac_t          local_mac;
    u32            arp_timeout_ms;     /* 0 → TCP_ARP_TIMEOUT_MS default */
    u32            connect_timeout_ms; /* 0 → TCP_CONNECT_TIMEOUT_MS default */
    ca_tcp_rx_t    on_rx;             /* NULL = blocking ca_tcp_recv() mode */
    ca_tcp_conn_t  on_connected;      /* NULL = blocking ca_tcp_connect() */
    ca_tcp_close_t on_closed;
} ca_tcp_connect_cfg_t;

/* ----------------------------------------------------------------------- */
/* Public API                                                              */
/* ----------------------------------------------------------------------- */

/**
 * Start TCP timer task — call from ca_os_main() before any ca_tcp_connect().
 */
void ca_tcp_service_start(void);

/**
 * Open TCP connection (active open / connect).
 * If cfg->on_connected == NULL: blocks until ESTABLISHED or timeout.
 * Returns 1-based handle on success, 0 on failure.
 */
u8   ca_tcp_connect(const ca_tcp_connect_cfg_t *cfg);

/**
 * Send data on an ESTABLISHED connection.
 * Data is segmented into MSS chunks; each segment is sent zero-copy.
 * Returns false if connection not ESTABLISHED or TX inflight pool exhausted.
 */
/** ca_tcp_send() outcomes.
 *
 * ⚠️ ALWAYS compare against CA_TCP_SENT. Both shorthand forms are wrong:
 *
 *     if (!ca_tcp_send(...))        WRONG - CA_TCP_GONE is -1, !(-1) is 0,
 *                                   so a refused send looks like a success
 *     return ca_tcp_send(...);      WRONG in a bool function - -1 becomes true
 *
 *     if (ca_tcp_send(...) != CA_TCP_SENT)     correct
 *
 * Both mistakes were made here on 15/09, the day this return changed from
 * bool, and the first one cost a round on the bench: the SSH banner write
 * failed on a stale handle, the negated test missed it, and the session sent
 * its KEXINIT with no banner in front. The client saw a key exchange where it
 * expected "SSH-2.0-" and reset the connection.
 */
#define CA_TCP_SENT    ( 1)   /* queued for transmission                     */
#define CA_TCP_RETRY   ( 0)   /* transient: no slot / window 0 - send again  */
#define CA_TCP_GONE    (-1)   /* final: handle stale, or not ESTABLISHED     */

i32  ca_tcp_send(u8 handle, const u8 *data, u16 len);

/**
 * Return true as soon as a TX inflight slot is free (at once if one already
 * is), false on timeout or if the connection goes away meanwhile.
 * Use in send loops instead of polling with sleep.
 */
bool ca_tcp_wait_writable(u8 handle, u32 timeout_ms);

/**
 * Blocking receive (only in on_rx==NULL mode).
 * Waits up to timeout_ms for received data.
 * Returns false on timeout or wrong mode.
 */
bool ca_tcp_recv(u8 handle, u8 *buf, u16 buf_len,
                 u16 *out_len, u32 timeout_ms);

/**
 * Initiate graceful close (send FIN).
 * Non-blocking: FIN handshake completes in background.
 */
void ca_tcp_close(u8 handle);

/**
 * Abort a connection in ANY state, freeing its slot immediately.
 * Unlike ca_tcp_close(), which only acts on ESTABLISHED connections and
 * starts a FIN handshake, this drops the PCB without talking to the peer.
 * Use it to reclaim a slot left in SYN_SENT by an attempt that never
 * completed (e.g. a service retry loop against an unreachable host):
 * without it the PCB lingers for the whole SYN backoff and TCP_MAX_CONN
 * slots run out.
 */
void ca_tcp_abort(u8 handle);

/**
 * Returns true if connection is in ESTABLISHED state.
 */
bool ca_tcp_is_connected(u8 handle);

/* ----------------------------------------------------------------------- */
/* TCP Server — passive open (listen/accept)                               */
/* ----------------------------------------------------------------------- */

/**
 * Config for ca_tcp_listen().
 * If on_accept == NULL: use blocking ca_tcp_accept().
 * If on_accept != NULL: callback called in net-task context on ESTABLISHED.
 * on_rx / on_closed apply to each accepted data connection.
 */
typedef struct {
    u16            local_port;
    ca_ipv4_t         local_ip;
    ca_mac_t          local_mac;
    ca_tcp_conn_t  on_accept;   /* NULL = blocking ca_tcp_accept() mode */
    ca_tcp_rx_t    on_rx;       /* NULL = blocking ca_tcp_recv() on data conn */
    ca_tcp_close_t on_closed;
} ca_tcp_listen_cfg_t;

/**
 * Start listening on local_port.
 * Returns 1-based listen handle or 0 on failure (pool full).
 * Call from ca_os_main() after ca_tcp_service_start().
 */
u8   ca_tcp_listen(const ca_tcp_listen_cfg_t *cfg);

/**
 * Stop listening and free the listen slot.
 * In-progress data connections are NOT closed.
 */
void ca_tcp_unlisten(u8 listen_handle);

/**
 * Blocking accept: waits for next incoming connection on listen_handle.
 * Returns data handle (1-based) when a client reaches ESTABLISHED,
 * or 0 on timeout or wrong mode (on_accept must be NULL).
 */
u8   ca_tcp_accept(u8 listen_handle, u32 timeout_ms);

#endif /* EN_TCP_SERVICE && EN_NET */
#endif /* TCP_SERVICE_H_ */
