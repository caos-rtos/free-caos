// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    eth.h
 * @brief   Ethernet MAC driver — public API
 *
 * @note    Supported families:
 *            F1xx : no ETH (stub)
 *            F4xx : legacy GMAC (F407/F429 only; F446 = stub)
 *            F7xx : legacy GMAC (same IP as F4xx)
 *            H7xx : GMAC 4.x (FDCAN-style registers, MTL layer)
 *            H5xx : GMAC 4.x (same IP as H7xx; different RCC/SBS regs)
 *
 *          GPIO pins must be configured by the caller before ca_eth_init().
 *          ca_eth_init() enables clocks, configures SYSCFG/SBS RMII/MII,
 *          resets and initialises the PHY, then starts MAC+DMA.
 *          MDIO access (ca_eth_phy_read/write) is available after ca_eth_init().
 *
 *          Internal descriptor ring: 4 TX + 4 RX, 1536-byte buffers (static).
 *
 *          Interrupt mode: set rx_cb (and optionally tx_done_cb / error_cb) in
 *          ca_eth_cfg_t before calling ca_eth_init(). The driver enables
 *          ETH_IRQn automatically. Callbacks run in ISR context — keep them
 *          minimal (post a semaphore / event, do not process frames inline).
 *          If all three ISR callbacks are NULL, ETH_IRQn is not enabled and
 *          the driver operates in pure polling mode (backward compatible).
 *
 *          Link monitoring: call ca_eth_link_poll() periodically from a task.
 *          It reads PHY BMSR, detects state transitions and invokes the
 *          registered callback.
 ******************************************************************************
 */

#ifndef ETH_H_
#define ETH_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_ETH

/* --- Callback types ---------------------------------------------------- */

/* Task context: link state changed (UP=true / DOWN=false) */
typedef void (*ca_eth_link_cb_t)(bool up);

/* ISR context: at least one RX frame is available in the ring.
 * Typical use: post a semaphore to wake the net task. */
typedef void (*ca_eth_rx_cb_t)(void);

/* ISR context: TX descriptor returned to host (TX slot free).
 * Useful to unblock a task waiting for TX ring space. NULL = ignore. */
typedef void (*ca_eth_tx_done_cb_t)(void);

/* ISR context: DMA error. flags is a bitmask of CA_ETH_ERR_* below.
 * NULL = ignore (errors are silently cleared). */
typedef void (*ca_eth_error_cb_t)(u32 flags);

/* Error flag bits passed to error_cb */
#define CA_ETH_ERR_BUS  (1U << 0)   /* Fatal DMA bus error — reinit required */
#define CA_ETH_ERR_RBU  (1U << 1)   /* RX buffer unavailable (packet loss) */
#define CA_ETH_ERR_TPS  (1U << 2)   /* TX process stopped */
#define CA_ETH_ERR_RPS  (1U << 3)   /* RX process stopped */

/* --- Configuration struct ---------------------------------------------- */

typedef struct {
    u8                  phy_addr;    /* MDIO PHY address 0-31 */
    bool                rmii;        /* true = RMII, false = MII */
    ca_mac_t               mac;         /* MAC address */
    ca_eth_link_cb_t    link_cb;     /* task ctx: link UP/DOWN; NULL = ignore */
    ca_eth_rx_cb_t      rx_cb;       /* ISR ctx: RX frame available; NULL = polling */
    ca_eth_tx_done_cb_t tx_done_cb;  /* ISR ctx: TX complete; NULL = ignore */
    ca_eth_error_cb_t   error_cb;    /* ISR ctx: DMA error; NULL = ignore */
} ca_eth_cfg_t;

/* Initialise MAC + DMA, set MAC address, configure RMII/MII via SYSCFG/SBS.
 * GPIO and PHY must already be configured by the caller.
 * Returns false on timeout or unsupported MCU. */
bool ca_eth_init  (const ca_eth_cfg_t *cfg);

/* ca_eth_send() results. */
#define CA_ETH_SEND_OK         0      /* frame handed to the DMA */
#define CA_ETH_SEND_ERR_TX   (-101)   /* TX ring full or MAC not initialised */
#define CA_ETH_SEND_ERR_LINK (-102)   /* link down (as last seen by ca_eth_link_poll) */

/* Transmit one Ethernet frame from a net pool buffer (zero-copy).
 * buf_idx: pool buffer index. The driver OWNS it from the call on, whatever
 * the result: on success it is freed when its TX slot is reused, on any error
 * it is freed before returning. The caller never frees it (the same rule as
 * ca_net_send(); freeing on failure was a double free).
 * Returns CA_ETH_SEND_OK, or CA_ETH_SEND_ERR_LINK when the link is down: the
 * frame would leave the MAC and be lost on a dead wire while looking sent.
 * The link state is the one cached by ca_eth_link_poll() - no MDIO access on
 * the TX path - so it is DOWN until the first poll sees the link up. */
i32  ca_eth_send  (u16 buf_idx);

/* Receive one Ethernet frame (zero-copy swap, non-blocking).
 * new_idx: pool buffer to load into the RX descriptor as replacement.
 * out_idx: pool buffer that holds the received frame (data at data+data_off).
 * Returns true if a frame was swapped out; false if no frame available.
 * On false, new_idx is not consumed — caller must free it. */
bool ca_eth_recv  (u16 *out_idx, u16 new_idx);

/* Stop MAC + DMA, disable clocks. */
void ca_eth_deinit(void);

/* MDIO (SMI) PHY register access — available after ca_eth_init(). */
u16  ca_eth_phy_read (u8 phy, u8 reg);
void ca_eth_phy_write(u8 phy, u8 reg, u16 val);

/* Poll PHY BMSR for link state change; invoke link_cb (from cfg) on transition.
 * Call periodically from a task (e.g. every 500 ms).
 * Returns true if link is currently up. */
bool ca_eth_link_poll(void);

/* Dump debug registers to stdout (task context only). */
void ca_eth_dbg_dump(void);

/* Re-initialize ETH with the config saved at ca_eth_init() time.
 * Use after ca_eth_deinit() to restart clock + DMA. */
bool ca_eth_restart(void);

/* Number of net pool buffers currently held by the ETH driver
 * (RX descriptors pre-allocated + TX deferred-free slots).
 * Use to correct leak checks: leaked = NET_BUF_COUNT - free - held. */
u16 ca_eth_pool_held_count(void);

#endif /* EN_ETH */
#endif /* ETH_H_ */
