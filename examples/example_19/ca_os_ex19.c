// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 19 — RW Lock + Barrier
 *
 * Demonstrates ca_rwlock_t and ca_barrier_t.
 *
 * PART A — RW Lock:
 *   A shared data structure (configuration table) is read
 *   frequently by N reader tasks and updated occasionally by a
 *   writer task. The RW Lock allows parallel reads, but guarantees
 *   exclusive access during writing.
 *
 *   task_reader[0..2] — reads the table every 200ms
 *   task_writer       — updates the table every 2s (write-preferring)
 *
 * PART B — Barrier:
 *   N worker tasks perform a calculation phase, then synchronize
 *   at the barrier. The last one to arrive (trigger) starts the next phase.
 *   Useful for step-based parallel algorithms.
 *
 *   task_worker[0..2]  — calculates, then ca_barrier_wait(), then next phase
 *   task_coordinator   — waits for notification from the trigger worker
 *
 * CLI COMMANDS:
 *   show   — read/write/barrier statistics
 *   help   — command list
 *
 * REQUIREMENTS os_config.h:
 *   EN_RWLOCK   1
 *   EN_BARRIER  1
 */

#include "caos.h"

#if EN_RWLOCK == 1 && EN_BARRIER == 1

/* ============================================================================
 * RW Lock — shared configuration table
 * ========================================================================== */

#define CFG_SIZE  8

typedef struct {
    u32 value[CFG_SIZE];
    u32 version;
} SharedConfig;

static ca_rwlock_t    rw_cfg;
static SharedConfig cfg;

static volatile u32 reads_ok;
static volatile u32 writes_ok;

/* ============================================================================
 * Barrier — 3 workers + coordinator
 * ========================================================================== */

#define N_WORKERS  3

static ca_barrier_t   bar;
static ca_semaphore_t sem_phase_done;  /* trigger → coordinator */
static volatile u32 barrier_phases;

/* ============================================================================
 * Task reader (3 instances — idx = reader index)
 * ========================================================================== */

static u32 task_reader(void *arg) {
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    (void)arg;
    while (1) {
        ca_os_sleep(200 + idx * 50u);

        ca_rwlock_read_lock(&rw_cfg);
        /* Reads consistent data while other readers can do so in parallel */
        u32 ver = cfg.version;
        u32 sum = 0;
        for (u32 i = 0; i < CFG_SIZE; i++) {
            sum += cfg.value[i];
        }
        ca_rwlock_read_unlock(&rw_cfg);

        reads_ok++;
        (void)ver; (void)sum;
    }
    return 0;
}

/* ============================================================================
 * Task writer
 * ========================================================================== */

static u32 task_writer(void *arg) {
    (void)arg;
    u32 seq = 0;
    while (1) {
        ca_os_sleep(2000);

        ca_rwlock_write_lock(&rw_cfg);
        /* Updates the entire table — exclusive access guaranteed */
        cfg.version = ++seq;
        for (u32 i = 0; i < CFG_SIZE; i++) {
            cfg.value[i] = seq * (i + 1u);
        }
        ca_rwlock_write_unlock(&rw_cfg);

        writes_ok++;
        ca_stdout_printf_n("CFG update v%u (reads so far: %u)", seq, reads_ok);
    }
    return 0;
}

/* ============================================================================
 * Task worker (3 instances)
 * ========================================================================== */

static u32 task_worker(void *arg) {
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    (void)arg;
    while (1) {
        /* Simulates work with variable duration for each worker */
        ca_os_sleep(100u + idx * 80u);

        /* All workers synchronize here */
        bool trigger = ca_barrier_wait(&bar);
        if (trigger) {
            /* Only the last worker to arrive executes this block */
            barrier_phases++;
            ca_stdout_printf_n("BARRIER phase %u - trigger=worker%u",
                barrier_phases, idx);
            /* Notify coordinator */
            ca_sem_post(&sem_phase_done);
        }
    }
    return 0;
}

/* ============================================================================
 * Task coordinator
 * ========================================================================== */

static u32 task_coordinator(void *arg) {
    (void)arg;
    while (1) {
        /* Waits for a barrier phase to be completed */
        ca_sem_wait(&sem_phase_done);
        ca_stdout_printf_n("COORD: phase %u done, reads=%u writes=%u",
            barrier_phases, reads_ok, writes_ok);
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
        CLI_PUT_N("--- Example 19 ---");
        CLI_PRINTF_N("  Reads ok       : %u", reads_ok);
        CLI_PRINTF_N("  Writes ok      : %u", writes_ok);
        CLI_PRINTF_N("  Barrier phases : %u", barrier_phases);
        CLI_PRINTF_N("  CFG version    : %u", cfg.version);
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

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
    reads_ok       = 0;
    writes_ok      = 0;
    barrier_phases = 0;

    /* Init RW Lock and table */
    ca_rwlock_init(&rw_cfg);
    cfg.version = 0;
    for (u32 i = 0; i < CFG_SIZE; i++) cfg.value[i] = 0;

    /* Init Barrier (3 workers) */
    ca_barrier_init(&bar, N_WORKERS);

    /* Init coordinator semaphore (count=0) */
    ca_sem_init(&sem_phase_done,"EX19_S1", 0, 16);

    /* === RW Lock === */
    ca_os_job("reader0", CA_TH_LEVEL2, task_reader, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("reader1", CA_TH_LEVEL2, task_reader, (void *)(uintptr_t)1, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("reader2", CA_TH_LEVEL2, task_reader, (void *)(uintptr_t)2, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("writer",  CA_TH_LEVEL3, task_writer, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    /* === Barrier === */
    ca_os_job("worker0", CA_TH_LEVEL2, task_worker, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("worker1", CA_TH_LEVEL2, task_worker, (void *)(uintptr_t)1, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("worker2", CA_TH_LEVEL2, task_worker, (void *)(uintptr_t)2, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("coord",   CA_TH_LEVEL1, task_coordinator, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    ca_os_run(true);
}

#else /* EN_RWLOCK == 0 || EN_BARRIER == 0 */

PRIVATE_STRING(m19a, "=== Example 19 ===");
PRIVATE_STRING(m19b, "EN_RWLOCK == 0 || EN_BARRIER == 0     (skipping RWLock/Barrier example)");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m19a);
    PUT_N(&priv_m19b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_RWLOCK && EN_BARRIER */
