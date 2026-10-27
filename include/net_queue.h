// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    net_queue.h
 * @brief   Lock-free SPSC ring queue of net buffer indices (u16)
 *
 * @note    Single-Producer / Single-Consumer design:
 *            Producer = ISR (writes .tail with DMB)
 *            Consumer = net task (writes .head with DMB)
 *          No critical section needed for push/pop.
 *          Full check and empty check are safe from both sides.
 ******************************************************************************
 */

#ifndef NET_QUEUE_H_
#define NET_QUEUE_H_

#include "common.h"
#include "os_config.h"

#if EN_NET

#define NET_QUEUE_DEPTH  128U  /* must be power of 2; >= NET_BUF_COUNT */

typedef struct {
    u16          buf[NET_QUEUE_DEPTH];
    volatile u16 head;    /* consumer (net task) advances head */
    volatile u16 tail;    /* producer (ISR)      advances tail */
} ca_net_queue_t;

/* Initialise queue to empty state. */
void ca_net_queue_init(ca_net_queue_t *q);

/* Push index from ISR (producer).  Returns false if queue full (drop). */
bool ca_net_queue_push(ca_net_queue_t *q, u16 idx);

/* Pop index in task context (consumer).  Returns false if empty. */
bool ca_net_queue_pop(ca_net_queue_t *q, u16 *idx);

bool ca_net_queue_full (const ca_net_queue_t *q);
bool ca_net_queue_empty(const ca_net_queue_t *q);

#endif /* EN_NET */
#endif /* NET_QUEUE_H_ */
