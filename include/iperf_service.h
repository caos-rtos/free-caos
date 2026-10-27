// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

#pragma once
#include "def_mcu.h"
#include "os_config.h"

#if EN_IPERF_SERVICE && EN_NET && EN_TCP_SERVICE

/* Result filled by client and server calls */
typedef struct {
    u32 bytes_total;
    u32 duration_ms;
    u32 bps;           /* bits per second */
} ca_iperf_result_t;

/* Blocking utility functions — usable standalone without the service task */
bool ca_iperf_client(ca_ipv4_t remote_ip, u16 port,
                     ca_ipv4_t local_ip, ca_mac_t local_mac,
                     u32 duration_ms, ca_iperf_result_t *out);

bool ca_iperf_server(u16 port,
                     ca_ipv4_t local_ip, ca_mac_t local_mac,
                     u32 idle_timeout_ms, ca_iperf_result_t *out);

/* Service start — creates background task + registers CLI.
 * Call once from ca_os_main() after TCP service start.
 * local_ip / mac can be zero and updated later via ca_iperf_update_local_ip(). */
void ca_iperf_service_start(ca_ipv4_t local_ip, ca_mac_t local_mac);

/* Update local IP after DHCP bind */
void ca_iperf_update_local_ip(ca_ipv4_t ip);

#endif /* EN_IPERF_SERVICE */
