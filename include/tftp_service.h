// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    tftp_service.h
 * @brief   caOS TFTP client service — RFC 1350, blocking GET and PUT
 *
 * @note    Architecture:
 *
 *          GET (read from server):
 *            ca_tftp_get(&cfg, on_block, arg)
 *              ├─ ARP-resolve server MAC
 *              ├─ bind local TID port
 *              ├─ send RRQ to server:69
 *              ├─ loop: recv DATA → call on_block → send ACK
 *              └─ unbind port, return true on last block ACKed
 *
 *          PUT (write to server):
 *            ca_tftp_put(&cfg, data, total_len)
 *              ├─ ARP-resolve server MAC
 *              ├─ bind local TID port
 *              ├─ send WRQ to server:69
 *              ├─ wait ACK 0 → send DATA blocks
 *              └─ unbind port, return true on last ACK received
 *
 *          RX path (runs in net task):
 *            net task → tftp_rx_handler → push buf_idx ring → post rx_sem
 *          TX path (runs in caller task):
 *            tftp_send_pkt → build Eth+IP+UDP → ca_net_send
 *
 *          Up to TFTP_MAX_XFER transfers (default 2) run at the same time,
 *          each in a slot of its own (state, port, receive ring): a slow
 *          peer no longer holds the others back. A caller waits only when
 *          every slot is busy.
 *
 *          Configuration (os_config.h under EN_TFTP_SERVICE):
 *            TFTP_BLOCK_TIMEOUT_MS  — wait per block (default 5000)
 *            TFTP_RETRY_N           — retries before failure (default 3)
 *            TFTP_MAX_XFER          — transfers at the same time (default 2)
 ******************************************************************************
 */

#ifndef TFTP_SERVICE_H_
#define TFTP_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_TFTP_SERVICE && EN_NET

/* ----------------------------------------------------------------------- */
/* Constants                                                                */
/* ----------------------------------------------------------------------- */

#define TFTP_PORT        69U    /* TFTP well-known port */
#define TFTP_BLOCK_SIZE  512U   /* standard block size (RFC 1350) */

/* TFTP error codes (RFC 1350 §5) */
#define TFTP_ERR_UNDEF    0U    /* Not defined */
#define TFTP_ERR_NOTFOUND 1U    /* File not found */
#define TFTP_ERR_ACCESS   2U    /* Access violation */
#define TFTP_ERR_DISKFULL 3U    /* Disk full or allocation exceeded */
#define TFTP_ERR_ILLEGAL  4U    /* Illegal TFTP operation */
#define TFTP_ERR_TID      5U    /* Unknown transfer ID */
#define TFTP_ERR_EXISTS   6U    /* File already exists */
#define TFTP_ERR_NOUSER   7U    /* No such user */

/* The peer's ERROR packet, when a transfer ends on one (RFC 1350 §5) */
#define CA_TFTP_NO_ERROR  0xFFFFU   /* code: no ERROR packet received */
typedef struct {
    u16  code;      /* TFTP_ERR_*, or CA_TFTP_NO_ERROR */
    char msg[48];   /* its text, printable ASCII */
} ca_tftp_err_t;

/* ----------------------------------------------------------------------- */
/* Callback                                                                 */
/* ----------------------------------------------------------------------- */

/**
 * GET block callback: called for each DATA block received.
 *
 * @param block_num  1-based block number.
 * @param data       Block payload pointer (valid only during callback).
 * @param len        Payload length (0..512); len < 512 signals last block.
 * @param arg        User argument passed to ca_tftp_get().
 * @return           true to continue; false to abort transfer.
 */
typedef bool (*ca_tftp_rx_block_t)(u16 block_num,
                                   const u8 *data, u16 len,
                                   void *arg);

/* ----------------------------------------------------------------------- */
/* Configuration                                                            */
/* ----------------------------------------------------------------------- */

typedef struct {
    ca_ipv4_t      server_ip;
    const char *filename;
    ca_mac_t       local_mac;
    ca_ipv4_t      local_ip;
    u16         local_port;         /* 0 = auto ephemeral */
    u32         arp_timeout_ms;     /* 0 = 2000 ms */
    u32         block_timeout_ms;   /* 0 = TFTP_BLOCK_TIMEOUT_MS */
    u8          retry_n;            /* 0 = TFTP_RETRY_N */
    ca_tftp_err_t *err;             /* NULL, or filled with the peer's ERROR packet */
} ca_tftp_cfg_t;

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Download a file from a TFTP server (blocking).
 *
 * Calls on_block for each received 512-byte block; last block has len < 512.
 * Returns true if transfer completed successfully (all blocks received).
 * Runs beside other transfers; waits only while all TFTP_MAX_XFER slots are busy.
 */
bool ca_tftp_get(const ca_tftp_cfg_t *cfg,
                 ca_tftp_rx_block_t on_block, void *arg);

/**
 * Upload a memory buffer to a TFTP server (blocking).
 *
 * Sends data[0..total_len-1] as filename on the server.
 * Returns true if transfer completed successfully (all ACKs received).
 * Runs beside other transfers; waits only while all TFTP_MAX_XFER slots are busy.
 */
bool ca_tftp_put(const ca_tftp_cfg_t *cfg,
                 const u8 *data, u32 total_len);

/* ----------------------------------------------------------------------- */
/* Server side: receive one file pushed by a client (WRQ)                  */
/* ----------------------------------------------------------------------- */

typedef struct {
    ca_mac_t    local_mac;
    ca_ipv4_t   local_ip;           /* 0 = the current address of the stack */
    u16         port;               /* listening port, 0 = TFTP_PORT */
    u32         block_timeout_ms;   /* 0 = TFTP_BLOCK_TIMEOUT_MS */
    u8          retry_n;            /* 0 = TFTP_RETRY_N */
    ca_tftp_err_t *err;             /* NULL, or filled with the peer's ERROR packet */
} ca_tftp_srv_cfg_t;

/**
 * WRQ callback: a client wants to write 'filename'. Runs in the receiving
 * task before block 0 is acknowledged, so it may take its time (e.g. erase
 * the destination). Returns 0 to accept, or a TFTP_ERR_* code to refuse.
 */
typedef u16 (*ca_tftp_wrq_t)(const char *filename, ca_ipv4_t client, void *arg);

/**
 * Wait up to wait_ms (CA_WAIT_FOREVER allowed) for a WRQ on cfg->port, then
 * receive that file block by block through on_block (blocking).
 * One file per call: the port is listened to only while waiting, and a WRQ
 * arriving outside that window is not answered. One waiting task at a time.
 * Returns true when the whole file arrived; false on timeout, refusal,
 * transfer error or abort by on_block.
 */
bool ca_tftp_receive(const ca_tftp_srv_cfg_t *cfg, ca_tftp_wrq_t on_wrq,
                     ca_tftp_rx_block_t on_block, void *arg, u32 wait_ms);

/**
 * Initialise the TFTP service and register CLI (if EN_CLI_TFTP).
 *
 * @param local_mac  our MAC address — stored for CLI-initiated transfers.
 */
void ca_tftp_service_init(ca_mac_t local_mac);

#endif /* EN_TFTP_SERVICE && EN_NET */
#endif /* TFTP_SERVICE_H_ */
