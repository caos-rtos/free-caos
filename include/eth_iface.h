// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    eth_iface.h
 * @brief   Adapter ETH driver -> ca_net_iface_t (pull / zero-copy)
 *
 * @note    Wraps the Ethernet driver (ca_eth_*) into a ca_net_iface_t with
 *          ops.recv != NULL (pull zero-copy DMA model). The driver ISR
 *          callbacks (rx/error) are registered here and post to ca_net_rx_sem.
 *
 *          Usage:
 *            static ca_eth_cfg_t s_eth = { .mac=..., .phy_addr=..., .rmii=... };
 *            ca_net_init(ca_eth_iface_get(&s_eth));
 *
 *          The ca_eth_cfg_t must remain valid for the entire lifetime of the stack
 *          (the driver keeps the pointer). The rx_cb/tx_done_cb/error_cb fields
 *          are overwritten by the adapter.
 ******************************************************************************
 */

#ifndef ETH_IFACE_H_
#define ETH_IFACE_H_

#include "common.h"
#include "os_config.h"

#if EN_NET && EN_ETH

#include "eth.h"
#include "net_iface.h"

/* Returns the iface (static singleton) configured over the ETH driver.
 * cfg->rx_cb/tx_done_cb/error_cb are set by the adapter. */
ca_net_iface_t *ca_eth_iface_get(ca_eth_cfg_t *cfg);

#endif /* EN_NET && EN_ETH */
#endif /* ETH_IFACE_H_ */
