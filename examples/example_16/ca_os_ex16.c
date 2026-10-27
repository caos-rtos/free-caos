// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#include "caos.h"

/*
 * Example 16 — Thread Pool + RNG + SHA-256
 *
 * A producer task periodically generates a buffer of random bytes using
 * the hardware RNG. For each buffer, it calculates the SHA-256 by sending the
 * job to the thread pool: the calculation happens on a worker thread without
 * blocking the producer.
 *
 * Structure:
 *   task_producer  — generates RNG buffer, sends job to pool every 500ms
 *   pool_worker    — (internal to the pool) calculates SHA-256 and prints the digest
 *   task_monitor   — every 5s prints pool statistics and global counters
 *
 * CLI COMMANDS:
 *   start  — enables verbose digest printing
 *   stop   — disables verbose printing
 *   show   — shows pool statistics and counters
 *   help   — command list
 *
 * COUNTERS (both in 'show' and in the 5s monitor block):
 *   submitted  jobs the pool accepted
 *   done       of those, hashed successfully
 *   failed     of those, the hash failed
 *   rejected   never entered the pool (rng failure, or queue full)
 * "rejected" is deliberately NOT part of "submitted": the pool cannot be held
 * accountable for a job it was never given.
 *
 * os_config.h REQUIREMENTS:
 *   EN_RNG   1
 *   EN_HASH  1
 *   EN_POOL  1
 *   MAX_TASK >= 12
 *
 * The MAX_TASK threshold counts the tasks this example really brings up:
 *   3 pool workers (POOL_THREADS) + producer + monitor = 5,
 *   plus the system's idle, stdout and cli = 8. Twelve leaves a margin.
 *
 * It used to be MAX_TASK >= 50, and that number did not come from the need: it
 * shut out ALL the F1/F4/F7 parts (MAX_TASK between 5 and 25) and left the
 * example to H5/H7/U5 (90-93), which would use 8 as well. On the F767 the pool
 * takes 24% of the PSP arena and 8 tasks out of 25: it fits comfortably. A part
 * that really cannot make it stays excluded anyway - the F103, with MAX_TASK 5.
 */

#if EN_RNG == 1 && EN_HASH == 1 && EN_POOL == 1 && MAX_TASK >= 12

/* ============================================================================
 * Configuration
 * ========================================================================== */

#define BUF_SIZE       64    /* bytes per RNG buffer */
#define POOL_THREADS   3
#define PRODUCE_MS     500   /* production interval */

/* ============================================================================
 * Shared state
 * ========================================================================== */

/* Counters. "submitted" is the population the pool is accountable for, so a job
 * that never entered the pool must NOT be counted against it: rejections live in
 * their own counter. Keeping both in one bucket makes the accounting unreadable
 * (submitted - done - failed would go negative as soon as the queue fills). */
static volatile u32 jobs_submitted; /* accepted by the pool */
static volatile u32 jobs_done;      /* submitted, hashed successfully */
static volatile u32 jobs_failed;    /* submitted, but the hash failed */
static volatile u32 jobs_rejected;  /* never entered: rng failure or queue full */
static volatile u8  print_digest;   /* 1=print digest, 0=silent */

/* Invariant, on any true instant:
 *     submitted == done + failed + busy + queued
 * "rejected" stands outside it by construction. */

/* ============================================================================
 * Static pool buffer — avoids malloc in the producer
 * Buffers are statically allocated and reused in round-robin.
 * The pool worker reads p1 as a pointer to the buffer.
 * ========================================================================== */

#define N_BUFS  (POOL_THREADS + 2)   /* a bit more than the threads for overlap */

typedef struct {
    u8   data[BUF_SIZE];
    u32  seq;
} ex16_buf_t;

static ex16_buf_t buf_pool[N_BUFS];
static volatile u8 buf_idx;          /* round-robin index, accessed only by the producer */

/* ============================================================================
 * Job executed by the pool worker: calculates SHA-256 and prints the digest
 * p1 = pointer to ex16_buf_t
 * ========================================================================== */

static u32 job_sha256(void *arg) {
    const u32 worker_idx = ca_os_thid_from_handle(ca_os_self_handle());
    ex16_buf_t *b = (ex16_buf_t *)arg;
    u8  digest[HASH_SHA256_DIGEST_SIZE];
    u32 dlen = 0;

    bool ok = ca_hash_compute(CA_HASH_ALGO_SHA256, b->data, BUF_SIZE, digest, &dlen);

    if (ok) {
        jobs_done++;
        if (print_digest) {
            /* Print seq + first 8 bytes of the digest as compact hex */
            ca_stdout_printf_n("SHA256 seq=%u w=%u : %02x%02x%02x%02x%02x%02x%02x%02x...",
                b->seq, worker_idx,
                digest[0], digest[1], digest[2], digest[3],
                digest[4], digest[5], digest[6], digest[7]);
        }
    } else {
        jobs_failed++;
    }
    return 0;
}

/* ============================================================================
 * Producer task: generates RNG buffer and sends job to the pool
 * ========================================================================== */

static u32 task_producer(void *arg) {
    (void)arg;
    u32 seq = 0;

    while (1) {
        /* Select buffer in round-robin */
        u8 bi = buf_idx;
        buf_idx = (u8)((bi + 1u) % N_BUFS);

        ex16_buf_t *b = &buf_pool[bi];
        b->seq = seq++;

        /* Fill with HW random data */
        if (!ca_rng_fill(b->data, BUF_SIZE)) {
            jobs_rejected++;    /* never reached the pool */
            ca_os_sleep(PRODUCE_MS);
            continue;
        }

        /* Send job to the pool (fire-and-forget) */
        i32 res = ca_pool_submit(job_sha256, b);
        if (res == 0) {
            jobs_submitted++;
        } else {
            jobs_rejected++;    /* queue full: never reached the pool */
            if (print_digest)
                ca_stdout_printf_n("POOL FULL: job seq=%u dropped", b->seq);
        }

        ca_os_sleep(PRODUCE_MS);
    }
    return 0;
}

/* ============================================================================
 * Monitor task: statistics every 5s
 * ========================================================================== */

static u32 task_monitor(void *arg) {
    (void)arg;
    ca_os_sleep(2000);

    while (1) {
        u8 busy, free_th, queued;
        ca_pool_stats(&busy, &free_th, &queued);
        ca_stdout_printf_n("=== POOL STATS ===");
        ca_stdout_printf_n("  submitted : %u", jobs_submitted);
        ca_stdout_printf_n("  done      : %u", jobs_done);
        ca_stdout_printf_n("  failed    : %u", jobs_failed);
        ca_stdout_printf_n("  rejected  : %u", jobs_rejected);
        ca_stdout_printf_n("  busy      : %u / %u", busy, (u8)POOL_THREADS);
        ca_stdout_printf_n("  free      : %u", free_th);
        ca_stdout_printf_n("  queued    : %u / %u", queued, (u8)POOL_QUEUE_SIZE);
        ca_stdout_printf_n("==================");
        ca_os_sleep(5000);
    }
    return 0;
}

/* ============================================================================
 * CLI — chain of responsibility
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd);
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

static bool user_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("ex16: start | stop | show  (rng get - driver)");
        return false;
    }
    if (ca_strncmp(p, "start", 5) == 0) {
        print_digest = 1;
        CLI_PUT_N("verbose ON");
        return true;
    }
    if (ca_strncmp(p, "stop", 4) == 0) {
        print_digest = 0;
        CLI_PUT_N("verbose OFF");
        return true;
    }
    if (ca_strncmp(p, "show", 4) == 0) {
        u8 busy, free_th, queued;
        ca_pool_stats(&busy, &free_th, &queued);
        CLI_PRINTF_N("submitted=%u  done=%u  failed=%u  rejected=%u  "
                     "busy=%u/%u  queued=%u/%u",
                     jobs_submitted, jobs_done, jobs_failed, jobs_rejected,
                     (u32)busy, (u32)POOL_THREADS,
                     (u32)queued, (u32)POOL_QUEUE_SIZE);
        return true;
    }
    return false;
}

/* ============================================================================
 * ca_os_configure — init modules
 * ========================================================================== */

void ca_os_configure(void) {

    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )

    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    /* Init hardware RNG and HASH */
    ca_rng_init();
    ca_hash_init();
}

/* ============================================================================
 * ca_os_main — entry point
 * ========================================================================== */

void ca_os_main(void) {
    jobs_submitted = 0;
    jobs_done      = 0;
    jobs_failed    = 0;
    jobs_rejected  = 0;
    print_digest   = 0;
    buf_idx        = 0;

    /* Init thread pool: 3 workers, medium stack, low priority */
    ca_pool_cfg_t pool_cfg = {
        .n_threads  = POOL_THREADS,
        .stack_size = STACK_MEDIUM,
        .priority   = CA_TH_LEVEL1,
    };
    ca_pool_init(&pool_cfg);

    /* Producer task: generates buffers and sends jobs */
    ca_os_job("producer", CA_TH_LEVEL2, task_producer, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    /* Monitor task: periodic statistics */
    ca_os_job("monitor", CA_TH_LEVEL1, task_monitor, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    /* Register CLI */
    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    ca_os_run(true);
}

#else /* EN_RNG==0 || EN_HASH==0 || EN_POOL==0 || MAX_TASK<12 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m16a, "=== Example 16 ===");
PRIVATE_STRING(m16b, "EN_RNG==0 || EN_HASH==0 || EN_POOL==0 || MAX_TASK<12 (skipping thread pool example)");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m16a);
    PUT_N(&priv_m16b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_RNG == 1 && EN_HASH == 1 && EN_POOL == 1 */
