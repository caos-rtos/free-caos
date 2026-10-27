// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : https_service.h
 * @brief          : HTTPS server - http_service routes over TLS 1.3
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * A second listener (443 by default) next to http_service: each connection
 * runs the TLS 1.3 handshake (tls.c), then the very same request parser, route
 * table and response builder as plain HTTP, with the bytes going through the
 * TLS session instead of the TCP socket.
 *
 * One connection at a time, like http_service (decision 22/10: single
 * session for security). Browsers open several connections and wait their
 * turn; no session tickets are issued, so each connection is a full
 * handshake (one ECDSA P-256 signature).
 *
 * The identity (P-256 key + X.509 certificate) is made on the PC by
 * tools/gen_tls_identity.py. The private key is copied into keys_store at
 * start: the caller can wipe its own copy afterwards.
 ******************************************************************************
 */

#pragma once
#include "def_mcu.h"
#include "os_config.h"

#if EN_HTTPS_SERVICE && EN_HTTP_SERVICE && EN_NET && EN_TCP_SERVICE && EN_TLS

#ifndef HTTPS_DEFAULT_PORT
#define HTTPS_DEFAULT_PORT  443U
#endif

/**
 * @brief Starts the HTTPS task, serving the routes given to
 *        ca_http_service_start() (call that one first).
 * @param cert_der    Server certificate, DER - must stay valid (flash)
 * @param cert_len    Its length
 * @param private_key P-256 private key, 32 bytes - copied into keys_store
 * @param port        0 = HTTPS_DEFAULT_PORT
 * @return false if the key cannot be stored (the key itself is checked by
 *         the task, which stops with a message if it is not a P-256 key)
 */
bool ca_https_service_start(const u8 *cert_der, u32 cert_len,
                            const u8 *private_key, u16 port);

#endif /* EN_HTTPS_SERVICE */
