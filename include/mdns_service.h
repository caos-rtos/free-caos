// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    mdns_service.h
 * @brief   caOS mDNS responder service (RFC 6762)
 ******************************************************************************
 */

#ifndef MDNS_SERVICE_H_
#define MDNS_SERVICE_H_

#include "def_mcu.h"
#include "os_config.h"

#if EN_MDNS_SERVICE && EN_NET && EN_NET_MDNS

/**
 * Initialise mDNS service. Call before ca_os_run().
 * @param local_mac  board MAC address
 * @param hostname   device name without ".local" (e.g. "caos-board")
 */
void ca_mdns_service_init(ca_mac_t local_mac, const char *hostname);

/** Bind UDP port 5353 — call from the net task after interface is up. */
bool ca_mdns_service_bind(void);

/** Signal that the network is up and send gratuitous announce. */
void ca_mdns_service_net_ready(ca_ipv4_t local_ip);

/** Handle incoming mDNS UDP frame (called from net task). */
void ca_mdns_service_rx_handler(const ca_udp_frame_t *udp, u16 len);

/** Create the mDNS responder task. */
void ca_mdns_service_start(void);

#endif /* EN_MDNS_SERVICE && EN_NET && EN_NET_MDNS */

#endif /* MDNS_SERVICE_H_ */
