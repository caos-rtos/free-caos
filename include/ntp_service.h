// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    ntp_service.h
 * @brief   caOS NTP client service — periodic sync with a NTP server
 *
 * @note    Architecture:
 *
 *            Application / ca_os_configure:
 *              ca_ntp_service_init(server_ip, on_sync, on_fail)
 *
 *            Application / ca_os_main:
 *              ca_ntp_service_start()
 *
 *            net task (after ca_net_proto_init + bind_udp(123,...)):
 *              ca_ntp_service_net_ready()    ← unblocks the NTP task
 *
 *            NTP task (CA_TH_LEVEL3):
 *
 *              ca_sem_wait(s_net_ready_sem)   ← gate: waits until net is ready
 *              loop forever:
 *                ARP-resolve server MAC (if EN_ARP_SERVICE)
 *                send NTP request (ca_net_proto_ntp_build_request)
 *                ca_sem_timedwait(s_resp_sem, NTP_RESPONSE_TIMEOUT_MS)
 *                  OK  → update s_unix_time, fire on_sync(unix_time)
 *                  timeout → retry up to NTP_RETRY_N times
 *                           → fire on_fail(), back-off NTP_RETRY_BACKOFF_MS
 *                ca_os_sleep(NTP_POLL_MS)    ← normal polling interval
 *                (interruptible by ca_ntp_service_sync_now → s_sync_now_sem)
 *
 *            net task: ca_ntp_service_rx_handler() called for every
 *              UDP frame on port 123. Posts s_resp_sem.
 *
 *          Configuration (os_config.h under EN_NTP_SERVICE):
 *            NTP_RESPONSE_TIMEOUT_MS — wait for response per attempt (default 5000)
 *            NTP_RETRY_N             — retries before on_fail (default 3)
 *            NTP_RETRY_BACKOFF_MS    — pause after failed cycle (default 10000)
 *            NTP_POLL_MS             — normal polling interval (default 64000)
 ******************************************************************************
 */

#ifndef NTP_SERVICE_H_
#define NTP_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"
#include "net_proto.h"

#if EN_NTP_SERVICE && EN_NET && EN_NET_NTP

/* ----------------------------------------------------------------------- */
/* Callbacks                                                                */
/* ----------------------------------------------------------------------- */

/**
 * Called from NTP task on successful sync.
 * @param unix_time  seconds since UNIX epoch (UTC), from transmit timestamp
 */
typedef void (*ca_ntp_sync_cb_t)(u32 unix_time);

/**
 * Called from NTP task after NTP_RETRY_N failed attempts.
 * Application may log the event or take corrective action.
 * NULL is accepted.
 */
typedef void (*ca_ntp_fail_cb_t)(void);

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Initialise NTP service state and synchronisation primitives.
 * Call from ca_os_configure() BEFORE ca_ntp_service_start().
 *
 * @param server_ip   IPv4 of the NTP server (e.g. pool.ntp.org resolved IP)
 * @param on_sync     callback fired on successful sync (mandatory)
 * @param on_fail     callback fired after NTP_RETRY_N failures (NULL ok)
 */
void ca_ntp_service_init(ca_ipv4_t           server_ip,
                         ca_mac_t            local_mac,
                         ca_ntp_sync_cb_t on_sync,
                         ca_ntp_fail_cb_t on_fail);

/** Update primary server IP (index 0). Backward-compatible with DHCP opt42 usage. */
void ca_ntp_service_set_server(ca_ipv4_t server_ip);

/** Update server at index n (0 = primary, 1..NTP_SERVER_N-1 = fallback). No-op if n >= NTP_SERVER_N. */
void ca_ntp_service_set_server_n(u8 n, ca_ipv4_t server_ip);

/**
 * Create and register the NTP service task (CA_TH_LEVEL3).
 * Call from ca_os_main() BEFORE ca_os_run().
 * The task starts blocked on s_net_ready_sem.
 */
void ca_ntp_service_start(void);

/**
 * Register the NTP client port (123) with the UDP dispatcher.
 * Call from task_net AFTER ca_net_proto_init().
 */
bool ca_ntp_service_bind(void);

/**
 * Signal that the net stack is ready to send/receive on port 123.
 * Call from task_net AFTER ca_net_proto_bind_udp(NTP_PORT, handler)
 * and AFTER the local IP is valid (i.e. after DHCP bound).
 */
void ca_ntp_service_net_ready(void);

/**
 * RX handler: call from net task for every UDP frame on port 123.
 * Parses the NTP response and signals the NTP task.
 */
void ca_ntp_service_rx_handler(const ca_udp_frame_t *udp, u16 len);

/**
 * Force an immediate sync cycle, interrupting the polling sleep.
 * Safe to call from any task or from CLI.
 */
void ca_ntp_service_sync_now(void);

/* Exposed for inline — written only by the NTP task */
extern volatile bool s_ntp_synced_flag;
/** Returns true if at least one successful sync has occurred. */
static inline bool ca_ntp_service_synced(void) { return s_ntp_synced_flag; }

/**
 * Return the last Unix timestamp received from the NTP server.
 * Returns 0 if never synced.
 * Protected by internal mutex.
 */
u32 ca_ntp_service_get_unix_time(void);

#endif /* EN_NTP_SERVICE && EN_NET && EN_NET_NTP */
#endif /* NTP_SERVICE_H_ */
