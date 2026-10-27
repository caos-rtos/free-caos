// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 * @file  ssh_service.h
 * @brief caOS minimal SSH-2.0 server service — one algorithm per category.
 *
 *        Negotiated set (server picks, no fallback):
 *          - KEX     : curve25519-sha256
 *          - hostkey : ssh-ed25519      (volatile: regenerated every boot)
 *          - cipher  : aes256-gcm@openssh.com   (implicit MAC = GCM tag)
 *          - MAC     : implicit (GCM)   — hmac keys derived but inert
 *          - auth    : publickey (Ed25519)      [phase 3]
 *
 *        Transport runs on top of the caOS TCP service; the underlying link
 *        can be Ethernet or USB-NCM (ca_ncm_iface) — SSH is link-agnostic.
 *
 *        Phase 2 scope (transport layer only):
 *          version exchange → binary packet protocol → KEXINIT →
 *          X25519 handshake → 6 session keys → NEWKEYS.
 *        Target: `ssh -v` reaches "expecting SSH2_MSG_SERVICE_ACCEPT".
 *        Auth / channel / CLI attach = phase 3.
 */

#ifndef SSH_SERVICE_H_
#define SSH_SERVICE_H_

#include "common.h"
#include "os_config.h"

#if EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET

/* ----------------------------------------------------------------------- */
/* Public API                                                              */
/* ----------------------------------------------------------------------- */

/**
 * Initialize the SSH server service and start listening.
 * @param port  TCP port to listen on (0 → SSH_DEFAULT_PORT = 22).
 * @return true on success, false if the listen slot / key store is unavailable.
 *
 * Preconditions: ca_tcp_service_start() and the network interface must be up;
 * ca_keys_store_init() and the RNG must be available (host key generation).
 * A fresh Ed25519 host key is generated on the first successful init.
 */
bool ca_ssh_service_init(u16 port);

/**
 * Ed25519 public key (32 raw bytes) of the ONE client allowed to log in.
 * Weak default in the library = SSH_AUTHORIZED_ED25519_PUB of os_config.h;
 * the application overrides it by defining the symbol itself. With caOS as
 * a precompiled library (FREE) this is the only way: generate a key pair
 * (ssh-keygen -t ed25519), keep the private key on the client, and put the
 * 32 bytes of the public one here. All zeros = no client: every login fails.
 */
extern const u8 ca_ssh_authorized_pub[32];

/**
 * Stop the SSH service: drop the listen slot and close active sessions.
 */
void ca_ssh_service_stop(void);

/**
 * Register the "ssh" CLI command (currently: `ssh test` — BPP self-test).
 * Call once during ca_os_main(), before the scheduler starts. No-op unless
 * EN_CLI_SSH && EN_SSH_TEST && ENABLE_CLI.
 */
void ca_ssh_test_init(void);

#if EN_SSH_TEST
/* Offline known-answer tests for the binary packet protocol framing.
 * Returns the number of failed cases (0 = all pass). Pure computation,
 * no network required. */
u32 ca_ssh_bpp_selftest(void);
#endif

#endif /* EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET */
#endif /* SSH_SERVICE_H_ */
