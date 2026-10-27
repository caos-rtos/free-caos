// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

#ifndef NET_UTILS_H_
#define NET_UTILS_H_

#include "common.h"
#include "net_cfg.h"

#if EN_NET

/**
 * @brief Parse string "a.b.c.d" into ca_ipv4_t
 * @return true if format is valid
 */
bool ca_parse_ipv4(const char *s, ca_ipv4_t *ip);

/**
 * @brief Parse string "AA:BB:CC:DD:EE:FF" into ca_mac_t (separator ':')
 * @return true if format is valid
 */
bool ca_parse_mac(const char *s, ca_mac_t *mac);

#endif /* EN_NET */
#endif /* NET_UTILS_H_ */
