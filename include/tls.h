// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : tls.h
 * @brief          : TLS 1.3 server (RFC 8446), one cipher suite
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Minimal TLS 1.3 server, independent of the transport: it reads and writes
 * through two callbacks, so the same code runs over ca_tcp_* (HTTPS) or any
 * other byte stream. Services (https_service, ...) are built on top of it.
 *
 * What is negotiated - and nothing else:
 *   version        TLS 1.3 only (supported_versions 0x0304)
 *   cipher suite   TLS_AES_128_GCM_SHA256 (0x1301), the mandatory one
 *   key exchange   X25519 key share
 *   signature      ecdsa_secp256r1_sha256 (0x0403), server certificate P-256
 *   ALPN           the configured protocol if the client offers it
 * Not supported: PSK / resumption (no tickets are issued), HelloRetryRequest,
 * client certificates, 0-RTT. A client that offers no X25519 key share gets
 * a handshake_failure alert.
 *
 * Memory: one ca_tls_t holds a full 16 KB record buffer - a client may send
 * records of up to 2^14 bytes and TLS gives no way to refuse them (Chrome
 * ignores record_size_limit). Declare it static, not on a task stack.
 *
 * Typical usage (one session):
 *   static ca_tls_t tls;
 *   ca_tls_io_t io = { my_send, my_recv, &my_conn };
 *   if (ca_tls_accept(&tls, &cfg, &io)) {
 *       n = ca_tls_read(&tls, buf, sizeof(buf), 5000);
 *       ca_tls_write(&tls, reply, reply_len);
 *       ca_tls_close(&tls);
 *   }
 ******************************************************************************
 */

#ifndef NET_TLS_H
#define NET_TLS_H

#include "common.h"
#include "sha256.h"
#include "aes.h"

#if EN_TLS

/* ============================================================================
 * CONSTANTS
 * ========================================================================== */

#define TLS_MAX_PLAINTEXT   16384U                   /* 2^14 */
#define TLS_RX_BUF_SIZE     (5U + TLS_MAX_PLAINTEXT + 256U)
#define TLS_TX_RECORD       2048U                    /* our records: plaintext max */
#define TLS_TX_BUF_SIZE     (5U + TLS_TX_RECORD + 1U + 16U)
#define TLS_HS_BUF_SIZE     4096U                    /* ClientHello reassembly */

/* ca_tls_read() results other than a byte count */
#define TLS_READ_TIMEOUT    0
#define TLS_READ_CLOSED     (-1)                     /* close_notify or EOF */
#define TLS_READ_ERROR      (-2)                     /* alert sent, session dead */

/* ============================================================================
 * DATA TYPES
 * ========================================================================== */

/**
 * @brief Byte transport under TLS.
 *
 * send: writes ALL len bytes, returns len, or < 0 on a broken connection.
 * recv: returns > 0 bytes read (up to len), 0 on timeout, < 0 when the
 *       connection is closed or broken.
 */
typedef struct {
    i32  (*send)(void *io, const u8 *data, u32 len);
    i32  (*recv)(void *io, u8 *buf, u32 len, u32 timeout_ms);
    void  *io;
} ca_tls_io_t;

/**
 * @brief Server identity and policy, shared by all sessions.
 *
 * The certificate is the DER of a self-signed (or CA-issued) X.509 whose
 * public key is the P-256 point of private_key. Both buffers must outlive
 * every session that uses the config.
 */
typedef struct {
    const u8   *cert_der;
    u32         cert_len;
    const u8   *private_key;            /* 32 bytes, P-256 */
    const char *alpn;                   /* e.g. "http/1.1", NULL = none */
    u32         handshake_timeout_ms;   /* each read during the handshake */
} ca_tls_config_t;

/* One direction of the record layer */
typedef struct {
    ca_aes_ctx_t aes;
    u8           iv[12];
    u64          seq;
    u8           secret[32];            /* traffic secret, for KeyUpdate */
} ca_tls_dir_t;

typedef struct {
    ca_tls_io_t             io;
    const ca_tls_config_t  *cfg;
    ca_sha256_ctx_t         transcript;
    ca_tls_dir_t            rd;
    ca_tls_dir_t            wr;
    bool                    rd_encrypted;
    bool                    wr_encrypted;
    bool                    established;
    bool                    closed;
    u8                      alert;          /* last alert sent or received */

    /* Application data received but not yet handed to ca_tls_read */
    u32                     app_off;
    u32                     app_len;

    u8                      rx[TLS_RX_BUF_SIZE];
    u8                      tx[TLS_TX_BUF_SIZE];
    u8                      hs[TLS_HS_BUF_SIZE];
    u32                     hs_len;
} ca_tls_t;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Runs the server handshake on an already accepted connection.
 * @return true when the session is established; false on any failure (the
 *         alert, if any, has been sent; tls->alert says which).
 */
bool ca_tls_accept(ca_tls_t *tls, const ca_tls_config_t *cfg, const ca_tls_io_t *io);

/**
 * @brief Reads application data.
 * @return bytes read (> 0), TLS_READ_TIMEOUT, TLS_READ_CLOSED or TLS_READ_ERROR
 */
i32 ca_tls_read(ca_tls_t *tls, u8 *buf, u32 len, u32 timeout_ms);

/**
 * @brief Writes application data, split into records of TLS_TX_RECORD.
 * @return true if every byte was sent
 */
bool ca_tls_write(ca_tls_t *tls, const u8 *data, u32 len);

/**
 * @brief Sends close_notify and wipes the session keys. The transport is
 *        left to the caller.
 */
void ca_tls_close(ca_tls_t *tls);

#endif /* EN_TLS */

#endif /* NET_TLS_H */
