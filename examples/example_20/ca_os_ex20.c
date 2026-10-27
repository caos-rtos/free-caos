// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 20 — Block Pool + ca_os_sleep_until + Timed Primitives
 *
 * Demonstrates three concepts in a single integrated example:
 *
 * PART A — Block Pool:
 *   A producer allocates blocks from the pool, fills them with data, and sends
 *   them to a consumer via queue. The consumer processes and returns
 *   the block to the pool. No heap, zero fragmentation.
 *
 * PART B — ca_os_sleep_until:
 *   Periodic task at precise frequency (500ms) without cumulative drift.
 *   Comparison with classic ca_os_sleep to demonstrate anti-drift.
 *
 * PART C — Timed Primitives:
 *   Demonstrates mutex_timedlock and sem_timedwait with explicit timeout.
 *   A slow task holds a mutex; a fast task tries with timeout
 *   and handles the "resource not available" case.
 *
 * CLI COMMANDS:
 *   show   — alloc/free statistics, drift, timeout
 *   help   — command list
 *
 * os_config.h REQUIREMENTS:
 *   EN_BLOCK_POOL  1
 */

#include "caos.h"

#if EN_BLOCK_POOL == 1

/* ============================================================================
 * PART A — Block Pool
 * ========================================================================== */

#define BLOCK_DATA_SIZE  32   /* useful bytes per block */
#define POOL_N_BLOCKS    8

typedef struct {
    u32 seq;
    u8  data[BLOCK_DATA_SIZE];
    u32 checksum;
} DataBlock;

/* Static buffer for the pool */
static u8 pool_buf[POOL_N_BLOCKS * sizeof(DataBlock)];
static ca_block_pool_t  block_pool;

/* Block pointer queue (u32 = uintptr_t of the pointer) */
CAQUEUE_STATIC_DEFINE(block_queue, POOL_N_BLOCKS, sizeof(u32));

static volatile u32 alloc_ok;
static volatile u32 alloc_fail;
static volatile u32 consumed;

/* ============================================================================
 * PART B — sleep_until
 * ========================================================================== */

static volatile u32 periodic_count;
static volatile u32 drift_max_ms;   /* maximum jitter measurement */

/* ============================================================================
 * PART C — Timed primitives
 * ========================================================================== */

static ca_mutex_t      slow_mutex;
static ca_semaphore_t  slow_sem;

static volatile u32 timedlock_ok;
static volatile u32 timedlock_timeout;
static volatile u32 timedwait_ok;
static volatile u32 timedwait_timeout;

/* ============================================================================
 * Producer task (Block Pool)
 * ========================================================================== */

static u32 task_producer(void *arg) {
    (void)arg;
    u32 seq = 0;
    while (1) {
        ca_os_sleep(150);

        DataBlock *blk = (DataBlock *)ca_block_alloc(&block_pool);
        if (blk == NULL) {
            alloc_fail++;
            continue;
        }

        blk->seq = seq++;
        for (u32 i = 0; i < BLOCK_DATA_SIZE; i++) {
            blk->data[i] = (u8)(seq + i);
        }
        blk->checksum = blk->seq ^ 0xDEADBEEFu;

        alloc_ok++;

        /* Send pointer to queue (as u32) */
        u32 ptr_val = (u32)(uintptr_t)blk;
        bool sent = ca_queue_static_send(&block_queue.q, &ptr_val, CA_WAIT_FOREVER);
        if (!sent) {
            /* Queue full — return block */
            ca_block_free(&block_pool, blk);
            alloc_fail++;
        }
    }
    return 0;
}

/* ============================================================================
 * Consumer task (Block Pool)
 * ========================================================================== */

static u32 task_consumer(void *arg) {
    (void)arg;
    while (1) {
        u32 ptr = 0;
        bool got = ca_queue_static_receive(&block_queue.q, &ptr, 500);
        if (!got) continue;

        DataBlock *blk = (DataBlock *)(uintptr_t)ptr;

        /* Verify checksum */
        u32 expected = blk->seq ^ 0xDEADBEEFu;
        if (blk->checksum != expected) {
            ca_stdout_printf_n("BLOCK ERR seq=%u chk=%X exp=%X",
                blk->seq, blk->checksum, expected);
        }

        consumed++;
        /* Return block to pool */
        ca_block_free(&block_pool, blk);
    }
    return 0;
}

/* ============================================================================
 * Anti-drift periodic task (sleep_until)
 * ========================================================================== */

static u32 task_periodic(void *arg) {
    (void)arg;
    u32 last_wake = ca_os_tick_ms();
    u32 prev_wake = last_wake;

    while (1) {
        ca_os_sleep_until(&last_wake, 500);

        periodic_count++;

        /* Measure jitter relative to ideal */
        u32 now = ca_os_tick_ms();
        u32 actual_period = now - prev_wake;
        prev_wake = now;

        /* Deviation from the expected 500ms */
        u32 err = (actual_period > 500u) ? (actual_period - 500u) : (500u - actual_period);
        if (err > drift_max_ms) {
            drift_max_ms = err;
        }
    }
    return 0;
}

/* ============================================================================
 * Slow task: holds mutex for 1s, then posts to semaphore
 * ========================================================================== */

static u32 task_slow(void *arg) {
    (void)arg;
    while (1) {
        ca_mutex_lock(&slow_mutex);
        ca_os_sleep(1000);   /* holds the mutex for 1 second */
        ca_mutex_unlock(&slow_mutex);

        ca_sem_post(&slow_sem);

        ca_os_sleep(500);    /* pause before the next cycle */
    }
    return 0;
}

/* ============================================================================
 * Fast task: attempts timedlock (200ms) and timedwait (200ms)
 * ========================================================================== */

static u32 task_fast(void *arg) {
    (void)arg;
    while (1) {
        /* Timedlock: mutex held for 1s by slow, timeout 200ms → fails */
        bool got = ca_mutex_timedlock(&slow_mutex, 200);
        if (got) {
            timedlock_ok++;
            ca_mutex_unlock(&slow_mutex);
        } else {
            timedlock_timeout++;
        }

        /* Semaphore timedwait: slow posts every ~1.5s, timeout 200ms */
        bool sem_ok = ca_sem_timedwait(&slow_sem, 200);
        if (sem_ok) {
            timedwait_ok++;
        } else {
            timedwait_timeout++;
        }

        ca_os_sleep(50);
    }
    return 0;
}

/* ============================================================================
 * Stats task: prints summary every 5s
 * ========================================================================== */

static u32 task_stats(void *arg) {
    (void)arg;
    ca_os_sleep(3000);
    while (1) {
        u32 pool_free = ca_block_pool_free_count(&block_pool);
        u32 pool_tot  = ca_block_pool_total(&block_pool);

        ca_stdout_printf_n("=== EX20 STATS ===");
        ca_stdout_printf_n("  BlockPool: alloc=%u fail=%u consumed=%u free=%u/%u",
            alloc_ok, alloc_fail, consumed, pool_free, pool_tot);
        ca_stdout_printf_n("  Periodic : count=%u drift_max=%ums",
            periodic_count, drift_max_ms);
        ca_stdout_printf_n("  Timedlock: ok=%u timeout=%u",
            timedlock_ok, timedlock_timeout);
        ca_stdout_printf_n("  Timedwait: ok=%u timeout=%u",
            timedwait_ok, timedwait_timeout);
        ca_stdout_printf_n("==================");
        ca_os_sleep(5000);
    }
    return 0;
}

/* ============================================================================
 * CLI hook
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("show");
        return false;
    }
    if (ca_strncmp(p, "show", 4) == 0) {
        u32 pool_free = ca_block_pool_free_count(&block_pool);
        u32 pool_tot  = ca_block_pool_total(&block_pool);
        CLI_PUT_N("--- Example 20 ---");
        CLI_PRINTF_N("  BlockPool  alloc    : %u", alloc_ok);
        CLI_PRINTF_N("  BlockPool  fail     : %u", alloc_fail);
        CLI_PRINTF_N("  BlockPool  consumed : %u", consumed);
        CLI_PRINTF_N("  BlockPool  free     : %u / %u", pool_free, pool_tot);
        CLI_PRINTF_N("  Periodic   count    : %u", periodic_count);
        CLI_PRINTF_N("  Periodic   drift_max: %u ms", drift_max_ms);
        CLI_PRINTF_N("  Timedlock  ok       : %u", timedlock_ok);
        CLI_PRINTF_N("  Timedlock  timeout  : %u", timedlock_timeout);
        CLI_PRINTF_N("  Timedwait  ok       : %u", timedwait_ok);
        CLI_PRINTF_N("  Timedwait  timeout  : %u", timedwait_timeout);
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/* ============================================================================
 * 'twkill [sec]': handoff to a waiter that is removed while it waits (27/09)
 *
 * mutex and semaphore HAND the resource to the waiter they dequeue. A waiter
 * killed while it waits is marked REMOVE at once but freed only at the next
 * scheduler refresh: a release landing in between handed it the mutex, or
 * the count, and it never ran again - the mutex owned by a dead task for
 * good, the count gone. The release now skips it. Each round: a victim
 * (timed or not) waits, a survivor waits behind it, the victim is killed and
 * the resource released at once. The survivor must get it within 100 ms.
 * ========================================================================== */
#if ENABLE_CLI

static ca_mutex_t      tk_mtx;
static ca_semaphore_t  tk_sem;
static bool         tk_ready;
static volatile u8  tk_kind;                    /* 0 mutex, 1 semaphore */
static volatile u32 tk_got;
static ca_thread_context_t *volatile tk_ctx[2];     /* 0 victim, 1 survivor */

static bool tk_listed(ca_thread_context_t *t) {
    const ca_wait_queue_t *wq = (tk_kind == 0U) ? &tk_mtx.wait_queue : &tk_sem.wait_queue;
    ca_thread_context_t *c = wq->head;
    for (u32 n = 0U; c != NULL && n < 32U; n++) {
        if (c == t) return true;
        c = ca_scheduler_get_next_wait(c);
    }
    return false;
}

/* p1: bit0 = survivor, bit1 = timed */
static u32 task_tk_waiter(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const bool timed = (p1 & 2U) != 0U;
    tk_ctx[p1 & 1U] = (ca_thread_context_t *)ca_scheduler_get_current_context();
    if (tk_kind == 0U) {
        if (timed) {
            if (!ca_mutex_timedlock(&tk_mtx, 60000U)) return 0;
        } else {
            ca_mutex_lock(&tk_mtx);
        }
        tk_got++;
        ca_mutex_unlock(&tk_mtx);
    } else {
        if (timed) {
            if (!ca_sem_timedwait(&tk_sem, 60000U)) return 0;
        } else {
            ca_sem_wait(&tk_sem);
        }
        tk_got++;
    }
    return 0;
}

static bool tk_wait_listed(u32 who) {
    for (u32 t = 0U; t < 200U; t += 2U) {
        ca_thread_context_t *c = tk_ctx[who];
        if (c != NULL && tk_listed(c)) return true;
        ca_os_sleep(2);
    }
    return false;
}

static void tk_run(u8 kind, u32 sec) {
    const u32 rounds = sec * 10U;
    u32 done = 0U, lost = 0U, setup = 0U, owner = 0U;
    tk_kind = kind;
    for (u32 r = 0U; r < rounds; r++) {
        tk_ctx[0] = tk_ctx[1] = NULL;
        const u32 got0 = tk_got;
        if (kind == 0U) ca_mutex_lock(&tk_mtx);   /* held here: both must wait */

        if (ca_os_job("tk_vic", CA_TH_LEVEL2, task_tk_waiter, (void *)(uintptr_t)((r & 1U) ? 2U : 0U),
                      STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT) == CA_TASK_HANDLE_NONE ||
            !tk_wait_listed(0U) ||
            ca_os_job("tk_sur", CA_TH_LEVEL2, task_tk_waiter, (void *)(uintptr_t)((r & 2U) ? 3U : 1U),
                      STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT) == CA_TASK_HANDLE_NONE ||
            !tk_wait_listed(1U)) {
            setup++;
            if (kind == 0U) ca_mutex_unlock(&tk_mtx);
            break;
        }

        /* Kill, then release before the refresh frees the victim */
        (void)ca_os_remove_job_handle(ca_os_task_handle(tk_ctx[0]));
        if (kind == 0U) ca_mutex_unlock(&tk_mtx); else ca_sem_post(&tk_sem);

        for (u32 t = 0U; tk_got == got0 && t < 100U; t += 2U) {
            ca_os_sleep(2);
        }
        ca_os_sleep(5);
        if (tk_got == got0) {
            lost++;
            if (tk_ctx[1] != NULL) (void)ca_os_remove_job_handle(ca_os_task_handle(tk_ctx[1]));
            ca_os_sleep(10);
            if (kind == 0U) ca_mutex_init(&tk_mtx); else ca_sem_init(&tk_sem, "TK_SEM_", 0, 16);
        } else if (kind == 0U) {
            /* The survivor unlocked it: nobody may still own it */
            if (!ca_mutex_trylock(&tk_mtx)) owner++; else ca_mutex_unlock(&tk_mtx);
        } else if (ca_sem_getvalue(&tk_sem) != 0U) {
            owner++;
        }
        done++;
    }
    const bool ok = done == rounds && lost == 0U && setup == 0U && owner == 0U;
    CLI_PRINTF_N("TK %-5s : rounds=%u/%u lost=%u owner=%u%s  %s",
                 kind == 0U ? "mutex" : "sem", done, rounds, lost, owner,
                 setup ? " SETUP_FAILED" : "", ok ? "PASS" : "FAIL");
}

static bool tk_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twkill    : [sec] mutex/sem handoff skips a waiter killed while waiting");
        return false;
    }
    if (ca_strncmp(p, "twkill", 6) != 0) return false;
    p = ca_skip_sp(p + 6);
    u32 sec = 3U;
    if (*p >= '0' && *p <= '9') {
        const i32 v = ca_strtol(p, 10);
        if (v >= 1 && v <= 30) sec = (u32)v;
    }
    if (!tk_ready) {
        ca_mutex_init(&tk_mtx);
        ca_sem_init(&tk_sem, "TK_SEM_", 0, 16);
        tk_ready = true;
    }
    tk_run(0U, sec);
    tk_run(1U, sec);
    return true;
}
static ca_cli_node_t s_tk_cli_node = { tk_cli_handler, NULL };

#endif /* ENABLE_CLI */

/* ============================================================================
 * 'twait [sec]': timed wait check on the mutex (27/09)
 *
 * A timed waiter parks in the primitive's wait list, SLEEP until its
 * deadline, and the peer wakes it. It used to poll every 10 ms, off the list:
 * woken up to 10 ms late, and served in the order it happened to poll.
 *   latency : a peer task releases the mutex it holds after 1..8 ms; the waiter, with a
 *             20 ms timeout, must get it and be running within TW_LAT_MS.
 *   timeout : the peer holds the mutex throughout; T = 1..20 ms, never back before T,
 *             at most TW_SLACK_MS after it.
 * One line, ends with PASS or FAIL.
 * ========================================================================== */
#if ENABLE_CLI

#define TW_T_MS      20U
#define TW_LAT_MS    2U
#define TW_SLACK_MS  2U

/* 0 idle, 1 arm, 3 armed, 2 signalled, 4 hold, 5 held, 6 release */
static volatile u8   tw_state;
static volatile u32  tw_stamp;
static volatile bool tw_busy;
static bool          tw_ready;
static ca_mutex_t tw_obj;
static void tw_init(void) { ca_mutex_init(&tw_obj); }
static void tw_hold(void) { ca_mutex_lock(&tw_obj); }
static void tw_give(void) { ca_mutex_unlock(&tw_obj); }
static bool tw_wait(u32 ms) { return ca_mutex_timedlock(&tw_obj, ms); }
static void tw_after(void) { ca_mutex_unlock(&tw_obj); }
static void tw_drain(void) {  }

static u32 task_tw_peer(void *arg) {
    (void)arg;
    u32 rng = 0x7A17E000U;
    while (1) {
        const u8 st = tw_state;
        if (st == 1U) {
            tw_hold();
            tw_state = 3U;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            ca_os_sleep(1U + rng % 8U);
            tw_stamp = ca_os_tick();
            tw_state = 2U;
            tw_give();
        } else if (st == 4U) {
            tw_hold();
            tw_state = 5U;
        } else if (st == 6U) {
            tw_give();
            tw_state = 0U;
        } else {
            ca_os_sleep(1);
        }
    }
    return 0;
}

/* Waits for the peer to reach @p st, false after 100 ms */
static bool tw_until(u8 st) {
    for (u32 t = 0U; tw_state != st && t < 100U; t++) {
        ca_os_sleep(1);
    }
    return tw_state == st;
}

/* Waits for the peer to arm (3) - or to have signalled already (2): its
 * 1 ms sleep can end in the same tick as the waiter's poll and run first,
 * and 3 is then never seen (equal deadlines, the CFS tie decides) */
static bool tw_until_armed(void) {
    for (u32 t = 0U; tw_state != 3U && tw_state != 2U && t < 100U; t++) {
        ca_os_sleep(1);
    }
    return tw_state == 3U || tw_state == 2U;
}

static void tw_run(u32 sec) {
    if (!tw_ready) {
        tw_init();
        tw_ready = ca_os_job("tw_peer", CA_TH_LEVEL2, task_tw_peer, NULL, STACK_SMALL,
                             CA_JOB_STD, CA_FAULT_ASSERT) != CA_TASK_HANDLE_NONE;
        if (!tw_ready) {
            CLI_PUT_N("TW mutex : peer task not created  FAIL");
            return;
        }
    }
    
    const u32 lat_lim = ca_os_tick_x_ms(TW_LAT_MS);
    const u32 slack   = ca_os_tick_x_ms(TW_SLACK_MS);
    u32 rounds = 0U, lost = 0U, spur = 0U, stuck = 0U, lat_max = 0U;

    /* latency: the peer signals, the waiter is parked with a deadline */
    for (u32 r = 0U; r < sec * 100U && stuck == 0U; r++) {
        tw_state = 1U;
        if (!tw_until_armed()) { stuck++; break; }
        const bool ok = tw_wait(TW_T_MS);
        const u32 now = ca_os_tick();
        if (!ok) {
            lost++;
        } else if (tw_state != 2U) {
            spur++;                         /* served before the signal */
        } else if (now - tw_stamp > lat_max) {
            lat_max = now - tw_stamp;
        }
        if (ok) tw_after();
        if (!tw_until(2U)) { stuck++; break; }
        tw_drain();
        tw_state = 0U;
        rounds++;
    }

    /* timeout: nobody signals */
    u32 n = 0U, early = 0U, late = 0U, got = 0U, over_max = 0U;
    tw_state = 4U;
    if (!tw_until(5U)) stuck++;
    const u32 end = ca_os_tick() + ca_os_tick_x_ms(sec * 1000U);
    while (stuck == 0U && (i32)(ca_os_tick() - end) < 0) {
        const u32 t_ms = 1U + (n % 20U);
        const u32 need = ca_os_tick_x_ms(t_ms);
        const u32 t0   = ca_os_tick();
        if (tw_wait(t_ms)) { got++; tw_after(); }
        const u32 dt = ca_os_tick() - t0;
        if (dt < need) {
            early++;
        } else {
            if (dt - need > over_max) over_max = dt - need;
            if (dt - need > slack) late++;
        }
        n++;
    }
    tw_state = 6U;
    (void)tw_until(0U);

    const bool ok = stuck == 0U && rounds == sec * 100U && lost == 0U && spur == 0U &&
                    lat_max <= lat_lim && n >= sec * 50U && early == 0U && late == 0U &&
                    got == 0U;
    CLI_PRINTF_N("TW mutex : rounds=%u lat_max=%u/%u lost=%u spur=%u | tmo n=%u early=%u late=%u got=%u over_max=%u/%u%s  %s",
                 rounds, lat_max, lat_lim, lost, spur, n, early, late, got, over_max, slack,
                 stuck ? " STUCK" : "", ok ? "PASS" : "FAIL");
}

static bool tw_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twait     : [sec] timed wait check on the mutex: latency + timeout");
        return false;
    }
    if (ca_strncmp(p, "twait", 5) != 0) return false;
    p = ca_skip_sp(p + 5);
    u32 sec = 3U;
    if (*p >= '0' && *p <= '9') {
        const i32 s = ca_strtol(p, 10);
        if (s >= 1 && s <= 30) sec = (u32)s;
    }
    if (tw_busy) {
        CLI_PUT_N("twait: a check is running - wait for its result");
        return true;
    }
    tw_busy = true;
    tw_run(sec);
    tw_busy = false;
    return true;
}
static ca_cli_node_t s_tw_cli_node = { tw_cli_handler, NULL };

#endif /* ENABLE_CLI */

/* ============================================================================
 * ca_os_configure
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
}

/* ============================================================================
 * ca_os_main
 * ========================================================================== */

void ca_os_main(void) {
    alloc_ok         = 0;
    alloc_fail       = 0;
    consumed         = 0;
    periodic_count   = 0;
    drift_max_ms     = 0;
    timedlock_ok     = 0;
    timedlock_timeout = 0;
    timedwait_ok     = 0;
    timedwait_timeout = 0;

    /* Init Block Pool */
    ca_block_pool_init(&block_pool, pool_buf, sizeof(pool_buf), sizeof(DataBlock));

    /* The block_queue is already initialized by the CAQUEUE_STATIC_DEFINE macro */

    /* Init mutex/sem for timed primitives */
    ca_mutex_init(&slow_mutex);
    ca_sem_init(&slow_sem,"EX20_S1", 0, 16);

    /* Block Pool tasks */
    ca_os_job("producer", CA_TH_LEVEL2, task_producer, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("consumer", CA_TH_LEVEL2, task_consumer, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    /* Periodic anti-drift */
    ca_os_job("periodic", CA_TH_LEVEL2, task_periodic, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    /* Timed primitives */
    ca_os_job("slow",     CA_TH_LEVEL2, task_slow,     NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("fast",     CA_TH_LEVEL2, task_fast,     NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    /* Stats */
    ca_os_job("stats",    CA_TH_LEVEL1, task_stats,    NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    EVAL_CLI( ca_cli_register(&s_tw_cli_node); )
    EVAL_CLI( ca_cli_register(&s_tk_cli_node); )
    ca_os_run(true);
}

#else /* EN_BLOCK_POOL == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m20a, "=== Example 20 ===");
PRIVATE_STRING(m20b, "EN_BLOCK_POOL == 0, skipping block pool example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m20a);
    PUT_N(&priv_m20b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_BLOCK_POOL */
