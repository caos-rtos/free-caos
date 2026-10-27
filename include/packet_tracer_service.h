// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    packet_tracer_service.h
 * @brief   Lightweight packet tracer — zero-copy metadata snapshot
 *
 * Hook points in net_proto.c (RX) and net.c (TX) copy only the
 * ca_net_buf_rx_t metadata (20 bytes) into a static ring.  A dedicated
 * low-priority task drains the ring, applies src/dst IP filter, and
 * prints each frame to the audit log.
 *
 * CLI: trace on | off | info
 *      trace filter src <ip> | dst <ip> | clear
 ******************************************************************************
 */

#ifndef PACKET_TRACER_SERVICE_H_
#define PACKET_TRACER_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_buf.h"

#if EN_PACKET_TRACER

/* Start tracer task + register CLI command */
void ca_pktrace_start(void);

/* Called from net_proto.c handlers after meta.rx is fully populated */
void ca_pktrace_push_rx(u16 buf_idx);

/* Called from ca_net_send() before ca_eth_send() */
void ca_pktrace_push_tx(u16 buf_idx);

/* Runtime control */
void ca_pktrace_enable(bool en);
void ca_pktrace_filter_src(ca_ipv4_t ip);   /* .addr=0 → match any */
void ca_pktrace_filter_dst(ca_ipv4_t ip);   /* .addr=0 → match any */

/* Set output channel — independent from stdout and audit_log.
 * Must be called before ca_pktrace_start() or right after.
 * Example: ca_pktrace_set_sender(STDOUT); */
void ca_pktrace_set_sender(sender fn);

#endif /* EN_PACKET_TRACER */
#endif /* PACKET_TRACER_SERVICE_H_ */
