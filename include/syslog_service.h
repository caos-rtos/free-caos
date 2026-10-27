// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    syslog_service.h
 * @brief   caOS Syslog client service — remote log over UDP (RFC 3164)
 *
 * @note    Architecture:
 *
 *            Application / ca_os_configure:
 *              ca_syslog_service_init(server_ip, facility)
 *
 *            Application / ca_os_main:
 *              ca_syslog_service_start()
 *
 *            net task (after ca_net_proto_init):
 *              ca_syslog_service_net_ready()   ← unblocks the syslog task
 *
 *            Syslog task (CA_TH_LEVEL4 — lowest priority):
 *              ca_sem_wait(s_net_ready_sem)    ← gate: waits until net is ready
 *              loop forever:
 *                ca_sem_wait(s_queue_sem)      ← blocks until a message is queued
 *                drain queue: for each entry
 *                  ARP-resolve server MAC (cached after first resolve)
 *                  ca_net_proto_syslog_build() → frame in pool buffer
 *                  ca_net_send()               ← zero-copy DMA
 *
 *            Any task / ISR:
 *              ca_syslog_send(severity, msg)   ← non-blocking, drops if queue full
 *
 *          Configuration (os_config.h under EN_SYSLOG_SERVICE):
 *            SYSLOG_QUEUE_SIZE   — message ring queue depth (default 8)
 *            SYSLOG_MSG_MAX      — max message length in bytes (default 128)
 ******************************************************************************
 */

#ifndef SYSLOG_SERVICE_H_
#define SYSLOG_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"
#include "net_proto.h"

#if EN_SYSLOG_SERVICE && EN_NET && EN_NET_SYSLOG

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Initialise syslog service state and synchronisation primitives.
 * Call from ca_os_configure() BEFORE ca_syslog_service_start().
 *
 * @param server_ip   IPv4 of the syslog server (UDP port 514)
 * @param local_mac   local MAC address (for frame builder)
 * @param facility    syslog facility: SYSLOG_FAC_USER, SYSLOG_FAC_LOCAL0..7
 */
void ca_syslog_service_init(ca_ipv4_t server_ip, ca_mac_t local_mac, u8 facility);

/**
 * Update server IP at runtime (e.g. after DHCP bound).
 * Thread-safe.
 */
void ca_syslog_service_set_server(ca_ipv4_t server_ip);

/**
 * Create and register the syslog task (CA_TH_LEVEL4).
 * Call from ca_os_main() BEFORE ca_os_run().
 */
void ca_syslog_service_start(void);

/**
 * Signal that the net stack is ready to send.
 * Call from task_net AFTER ca_net_proto_init() and local IP is valid.
 */
void ca_syslog_service_net_ready(void);

/**
 * Enqueue a syslog message for async delivery.
 * Non-blocking: drops the message silently if the queue is full.
 * Safe to call from any task context.
 *
 * @param severity  SYSLOG_SEV_EMERG .. SYSLOG_SEV_DEBUG
 * @param msg       null-terminated string, truncated to SYSLOG_MSG_MAX
 */
void ca_syslog_send(u8 severity, const char *msg);

/**
 * Return number of messages dropped due to queue full since last init.
 */
u32 ca_syslog_dropped(void);



#endif /* EN_SYSLOG_SERVICE && EN_NET && EN_NET_SYSLOG */
#endif /* SYSLOG_SERVICE_H_ */
