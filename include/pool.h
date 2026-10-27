// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    pool.h
 * @brief   Thread Pool API for caOS
 * @details N permanent pre-allocated threads that execute fire-and-forget jobs.
 *          Requires MAX_TASK >= 12: the pool creates n_threads workers plus a
 *          spare one-shot thread when the queue is full, and ca_pool_init()
 *          returns -1 if MAX_TASK is exhausted - so the module already
 *          defends itself at runtime. It used to be >= 50, which shut out ALL
 *          the F1/F4/F7 parts (MAX_TASK 5-25) and left the pool to H5/H7/U5:
 *          a static precaution six times the real need (see ex16, which with
 *          3 workers uses 8 tasks in all and 24%% of the PSP arena on F767).
 *          Enable with EN_POOL=1 in os_config.h.
 ******************************************************************************
 */

#ifndef POOL_H
#define POOL_H

#include "common.h"
#include "scheduler.h"

#if EN_POOL == 1 && MAX_TASK >= 12

/** Pool configuration */
typedef struct {
    u8           n_threads;   /**< number of worker threads (max 99) */
    ca_stack_size_t stack_size;  /**< stack for each worker */
    u16          priority;    /**< worker priority and one-shot fallback */
} ca_pool_cfg_t;

/**
 * @brief Initializes the thread pool.
 * @param cfg  Pointer to the configuration.
 * @return 0=OK, -1=error (n_threads==0 or MAX_TASK exhausted).
 * @note   Call before ca_os_run().
 */
i32 ca_pool_init(const ca_pool_cfg_t *cfg);

/**
 * @brief Sends a job to the pool (fire-and-forget).
 * @param func  Function to execute (signature: u32 func(void *arg)).
 * @param arg   Argument passed to the function.
 * @return  0=OK (job queued or one-shot thread started),
 *         -1=queue full and MAX_TASK exhausted (job discarded).
 */
i32 ca_pool_submit(ca_task_fn_t func, void *arg);

/**
 * @brief Current pool statistics.
 * @param busy    [out] threads currently running.
 * @param free_th [out] threads waiting (free).
 * @param queued  [out] jobs in queue.
 */
void ca_pool_stats(u8 *busy, u8 *free_th, u8 *queued);

#endif /* EN_POOL == 1 && MAX_TASK >= 12 */
#endif /* POOL_H */
