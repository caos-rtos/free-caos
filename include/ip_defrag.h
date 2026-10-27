// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    ip_defrag.h
 * @brief   IP fragment reassembly service
 *
 * @note    Supports up to IP_DEFRAG_MAX_SESSIONS (4) simultaneous reassembly
 *          sessions. Each session stores fragment payloads in a pre-allocated
 *          heap buffer; net_buf pool buffers are freed immediately on fragment
 *          arrival to avoid pool starvation under load.
 *
 *          Fragment identification key: (src_ip, dst_ip, proto, ip_id).
 *          Timeout per session: IP_DEFRAG_TIMEOUT_MS (default 15 000 ms).
 *          Max reassembled payload: ip_defrag_service_start(max_payload_size).
 *
 *          When EN_IP_DEFRAG=0 any fragmented packet is dropped and optionally
 *          traced via EN_PACKET_TRACER.
 ******************************************************************************
 */

#ifndef IP_DEFRAG_H_
#define IP_DEFRAG_H_

#include "common.h"
#include "os_config.h"

#if EN_NET && EN_IP_DEFRAG

/* ----------------------------------------------------------------------- */
/* Constants (overridable before include via os_config.h)                  */
/* ----------------------------------------------------------------------- */

#ifndef IP_DEFRAG_MAX_SESSIONS
#define IP_DEFRAG_MAX_SESSIONS  4U        /* simultaneous reassembly sessions */
#endif

#ifndef IP_DEFRAG_TIMEOUT_MS
#define IP_DEFRAG_TIMEOUT_MS    15000U    /* session lifetime without last frag */
#endif

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * @brief  Initialise defrag service and allocate reassembly memory.
 *
 * Must be called once before ca_ip_defrag_service_start().
 * Allocates IP_DEFRAG_MAX_SESSIONS × max_payload_size bytes from heap.
 *
 * @param  max_payload_size  Max IP payload (data only, no IP header) in bytes.
 *                           Typically 1480 (1500 MTU - 20 IP hdr).
 * @return true on success, false if heap allocation failed.
 */
bool ca_ip_defrag_init(u32 max_payload_size);

/**
 * @brief  Start the defrag background task.
 *
 * Spawns a caOS task that waits for fragment notifications, performs
 * timeout cleanup every IP_DEFRAG_TIMEOUT_MS, and injects reassembled
 * packets back into the IP dispatch chain.
 * Call after ca_ip_defrag_init().
 */
void ca_ip_defrag_service_start(void);

/**
 * @brief  Submit a fragment buffer to the defrag service.
 *
 * Called from ca_net_proto_ip4_handler() when a fragmented IP packet is
 * detected (MF=1 or fragment offset != 0).
 * The function copies the fragment payload into the session buffer and
 * releases buf_idx back to the net_buf pool immediately.
 *
 * @param  buf_idx  Index of the net_buf holding the fragment.
 */
void ca_ip_defrag_submit(u16 buf_idx);

#endif /* EN_NET && EN_IP_DEFRAG */
#endif /* IP_DEFRAG_H_ */
